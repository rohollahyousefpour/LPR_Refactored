#include "IExposureStrategy.h"
#include "sunset.h"
#include "AppLogger.h"
#include "ExposureController.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace UCP = Basler_UniversalCameraParams;

namespace {

// ---- Node access: exposure/gain live under one of three SFNC generations.
// Modern USB3/SFNC-2 uses the float ExposureTime/Gain; GigE SFNC-1 (e.g. the
// acA1920-40gc) uses the float ExposureTimeAbs/GainAbs (us / dB); the oldest
// firmware uses the integer ExposureTimeRaw/GainRaw. We pick by which node is
// actually attached (IsReadable) -- NOT by writability, because a node is
// read-only while the camera's own auto still owns it, and probing the wrong
// generation's node throws "No node attached". Callers switch auto OFF (via
// ensureManual) before writing so the chosen node becomes writable. -----------
enum class ExpKind  { Modern, Abs, Raw, None };
enum class GainKind { Modern, Abs, Raw, None };

ExpKind pickExposure(CameraDevice::PylonCamera* c) {
    if (c->ExposureTime.IsReadable())    return ExpKind::Modern;
    if (c->ExposureTimeAbs.IsReadable()) return ExpKind::Abs;
    if (c->ExposureTimeRaw.IsReadable()) return ExpKind::Raw;
    return ExpKind::None;
}
GainKind pickGain(CameraDevice::PylonCamera* c) {
    if (c->Gain.IsReadable())    return GainKind::Modern;
    if (c->GainAbs.IsReadable()) return GainKind::Abs;
    if (c->GainRaw.IsReadable()) return GainKind::Raw;
    return GainKind::None;
}

double expMinOf(CameraDevice::PylonCamera* c, ExpKind k) {
    switch (k) { case ExpKind::Modern: return c->ExposureTime.GetMin();
                 case ExpKind::Abs:    return c->ExposureTimeAbs.GetMin();
                 case ExpKind::Raw:    return double(c->ExposureTimeRaw.GetMin());
                 default:              return 0.0; }
}
double expMaxOf(CameraDevice::PylonCamera* c, ExpKind k) {
    switch (k) { case ExpKind::Modern: return c->ExposureTime.GetMax();
                 case ExpKind::Abs:    return c->ExposureTimeAbs.GetMax();
                 case ExpKind::Raw:    return double(c->ExposureTimeRaw.GetMax());
                 default:              return 0.0; }
}
double expValOf(CameraDevice::PylonCamera* c, ExpKind k) {
    switch (k) { case ExpKind::Modern: return c->ExposureTime.GetValue();
                 case ExpKind::Abs:    return c->ExposureTimeAbs.GetValue();
                 case ExpKind::Raw:    return double(c->ExposureTimeRaw.GetValue());
                 default:              return 0.0; }
}
double gnMinOf(CameraDevice::PylonCamera* c, GainKind k) {
    switch (k) { case GainKind::Modern: return c->Gain.GetMin();
                 case GainKind::Abs:    return c->GainAbs.GetMin();
                 case GainKind::Raw:    return double(c->GainRaw.GetMin());
                 default:               return 0.0; }
}
double gnMaxOf(CameraDevice::PylonCamera* c, GainKind k) {
    switch (k) { case GainKind::Modern: return c->Gain.GetMax();
                 case GainKind::Abs:    return c->GainAbs.GetMax();
                 case GainKind::Raw:    return double(c->GainRaw.GetMax());
                 default:               return 0.0; }
}
double gnValOf(CameraDevice::PylonCamera* c, GainKind k) {
    switch (k) { case GainKind::Modern: return c->Gain.GetValue();
                 case GainKind::Abs:    return c->GainAbs.GetValue();
                 case GainKind::Raw:    return double(c->GainRaw.GetValue());
                 default:               return 0.0; }
}

// Switch the camera's own auto OFF so the manual exposure/gain nodes become
// writable. Must run BEFORE reading node min/max in any manual setter.
void ensureManual(CameraDevice::PylonCamera* c) {
    if (c->ExposureAuto.IsWritable()) c->ExposureAuto.SetValue(UCP::ExposureAuto_Off);
    if (c->GainAuto.IsWritable())     c->GainAuto.SetValue(UCP::GainAuto_Off);
    if (c->ExposureMode.IsWritable()) c->ExposureMode.SetValue(UCP::ExposureMode_Timed);
}

void writeExposure(CameraDevice::PylonCamera* c, double value, ExpKind k) {
    if (c->ExposureAuto.IsWritable()) c->ExposureAuto.SetValue(UCP::ExposureAuto_Off);
    if (c->ExposureMode.IsWritable()) c->ExposureMode.SetValue(UCP::ExposureMode_Timed);
    switch (k) {
        case ExpKind::Modern: if (c->ExposureTime.IsWritable())    c->ExposureTime.SetValue(value); break;
        case ExpKind::Abs:    if (c->ExposureTimeAbs.IsWritable()) c->ExposureTimeAbs.SetValue(value); break;
        case ExpKind::Raw:    if (c->ExposureTimeRaw.IsWritable()) c->ExposureTimeRaw.SetValue(int64_t(value)); break;
        default: break;
    }
}
void writeGain(CameraDevice::PylonCamera* c, double value, GainKind k) {
    if (c->GainAuto.IsWritable()) c->GainAuto.SetValue(UCP::GainAuto_Off);
    switch (k) {
        case GainKind::Modern: if (c->Gain.IsWritable())    c->Gain.SetValue(value); break;
        case GainKind::Abs:    if (c->GainAbs.IsWritable()) c->GainAbs.SetValue(value); break;
        case GainKind::Raw:    if (c->GainRaw.IsWritable()) c->GainRaw.SetValue(int64_t(value)); break;
        default: break;
    }
}

bool readExposureGain(CameraDevice::PylonCamera* c,
                      double& exposure, double& gain, ExpKind& ek, GainKind& gk) {
    ek = pickExposure(c); gk = pickGain(c);
    if (ek == ExpKind::None || gk == GainKind::None) return false;
    exposure = expValOf(c, ek);
    gain     = gnValOf(c, gk);
    return true;
}

// ---- Robust metering: percentile over an ROI of a downscaled gray image ----
double meteredBrightness(const cv::Mat& bgr, cv::Rect roi, double percentile) {
    cv::Mat region = bgr;
    if (roi.area() > 0) {
        cv::Rect r = roi & cv::Rect(0, 0, bgr.cols, bgr.rows);
        if (r.area() > 0) region = bgr(r);
    }
    cv::Mat scaled;
    const double scale = 320.0 / std::max(region.cols, 1);
    if (scale < 1.0) cv::resize(region, scaled, cv::Size(), scale, scale, cv::INTER_AREA);
    else             scaled = region;

    cv::Mat gray;
    if (scaled.channels() == 3) cv::cvtColor(scaled, gray, cv::COLOR_BGR2GRAY);
    else                        gray = scaled;

    int hist[256] = {0};
    const long n = long(gray.rows) * gray.cols;
    if (n == 0) return 0.0;
    for (int y = 0; y < gray.rows; ++y) {
        const uchar* p = gray.ptr<uchar>(y);
        for (int x = 0; x < gray.cols; ++x) ++hist[p[x]];
    }
    const long target = long(n * std::clamp(percentile, 0.0, 100.0) / 100.0);
    long acc = 0; int v = 0;
    for (; v < 256; ++v) { acc += hist[v]; if (acc >= target) break; }
    return double(v);
}

} // namespace

AutoExposureStrategy::AutoExposureStrategy(Limits limits)
    : lim_(limits) {}

bool AutoExposureStrategy::throttled() {
    const auto now = std::chrono::steady_clock::now();
    return lastChange_.time_since_epoch().count() != 0 &&
           std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChange_) < lim_.minInterval;
}

void AutoExposureStrategy::apply(CameraDevice& dev, const cv::Mat& bgr) {
    auto* c = dev.raw();
    if (!c || bgr.empty()) return;

    try {
        // ---- Brightness-driven target + motion-blur cap (no day/night clock) ----
        const double desired   = lim_.target;
        const double tol       = lim_.deadband;        // deadband around the target
        const double regimeCap = double(lim_.maxExposureUs);
        const double stepRatio = lim_.maxStepRatio;

        // ---- 1) Robust, smoothed intensity measurement (median, not mean) ----
        const double measured = meteredBrightness(bgr, lim_.roi, lim_.percentile);
        measuredEma_ = (measuredEma_ < 0)
                         ? measured
                         : lim_.emaAlpha * measured + (1 - lim_.emaAlpha) * measuredEma_;
        const double m = measuredEma_;
        const auto now = std::chrono::steady_clock::now();

        // Heartbeat: emit a status line at most every ~3s EVEN WHEN NOT ADJUSTING, so the loop
        // is observable -- what it meters (raw + smoothed) vs the target, and where exposure/
        // gain currently sit. Actual adjustments below always log (and reset this timer).
        const bool dueLog = (lastLog_.time_since_epoch().count() == 0) ||
            (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog_) >=
             std::chrono::milliseconds(3000));
        auto status = [&](const char* state) {
            double e = 0, g = 0; ExpKind ek; GainKind gk; readExposureGain(c, e, g, ek, gk);
            LOGI() << "[auto-exposure][" << dev.serial() << "] " << state
                   << " measured=" << measured << " ema=" << m << " target=" << desired
                   << " exposure=" << e << "us gain=" << g
                   << " pct=" << lim_.percentile << " cap=" << regimeCap;
            lastLog_ = now;
        };

        if (throttled()) { if (dueLog) status("HOLD throttled"); return; }

        if (std::fabs(desired - m) < tol) {          // deadband: good enough, leave it
            if (dueLog) status("HOLD in-band ");
            return;
        }

        // The camera's own auto must be OFF, or it keeps the manual exposure/gain nodes
        // read-only - which surfaces as "no exposure/gain nodes" and stalls the host loop. We
        // switch to manual/timed BEFORE reading the nodes (writeExposure/writeGain also do this,
        // but they run after the read, so doing it here breaks the deadlock on the first frame).
        if (c->ExposureAuto.IsWritable()) c->ExposureAuto.SetValue(UCP::ExposureAuto_Off);
        if (c->GainAuto.IsWritable())     c->GainAuto.SetValue(UCP::GainAuto_Off);
        if (c->ExposureMode.IsWritable()) c->ExposureMode.SetValue(UCP::ExposureMode_Timed);

        double exposure, gain; ExpKind ek; GainKind gk;
        if (!readExposureGain(c, exposure, gain, ek, gk)) {
            LOGW() << "[auto-exposure][" << dev.serial()
                   << "] no exposure/gain nodes attached (tried ExposureTime/Abs/Raw and "
                      "Gain/Abs/Raw)";
            return;
        }
        const double expMin = expMinOf(c, ek);
        const double expMax = regimeCap;             // motion-blur ceiling for THIS regime
        const double gMin   = double(lim_.minGain);
        const double gMax   = double(lim_.maxGain);
        const double gainStep = std::max(1.0, lim_.gainStepFrac * (gMax - gMin));

        // ---- 2) Pure control law (unit-tested): one measurement -> damped, clamped
        // correction; exposure-before-gain when dark, gain-before-exposure when bright. ----
        ExposureParams P;
        P.target = desired; P.deadband = tol; P.damping = lim_.damping;
        P.maxStepRatio = stepRatio; P.expMin = expMin; P.expMax = expMax;
        P.gMin = gMin; P.gMax = gMax; P.gainStep = gainStep;
        P.highlightPriority = lim_.highlightPriority;
        const ExposureDecision dec = decideExposure(m, exposure, gain, P);
        if (!dec.changed) { if (dueLog) status("HOLD no-op  "); return; }

        if (std::fabs(dec.exposure - exposure) > 1.0) writeExposure(c, dec.exposure, ek);
        if (std::fabs(dec.gain - gain) > 1e-6)        writeGain(c, dec.gain, gk);
        lastChange_ = now;
        // Always log an actual adjustment so you can WATCH the loop converge (reads back the
        // exposure/gain the camera actually accepted -- cap/interval limits show up here).
        status(dec.dark ? "DARK  ->    " : "BRIGHT->    ");
    }
    catch (const cv::Exception& e) {              // cvtColor/resize/mean failure
        AppLogger::LogCvException(e, "[auto-exposure] " + dev.serial());
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[auto-exposure] " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[auto-exposure] " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[auto-exposure] " + dev.serial());
    }
}

// ---------------------- ManualExposureStrategy ----------------------

void ManualExposureStrategy::setNormalizedExposure(CameraDevice& dev, double norm,
                                                   double maxExposureUs) {
    auto* c = dev.raw(); if (!c) return;
    norm = std::clamp(norm, 0.0, 1.0);
    try {
        ensureManual(c);
        const ExpKind ek = pickExposure(c);
        const double lo = expMinOf(c, ek);
        const double hiNode = expMaxOf(c, ek);
        const double hi = std::min(hiNode, maxExposureUs);   // unified ceiling
        const double want = lo + norm * (hi - lo);
        writeExposure(c, want, ek);
        const double actual = expValOf(c, ek);               // read-back
        if (std::fabs(actual - want) > std::max(2.0, 0.05 * want))
            LOGW() << "[manual-exposure][" << dev.serial() << "] requested " << want
                   << " clamped to " << actual;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[manual-exposure] " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[manual-exposure] " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[manual-exposure] " + dev.serial());
    }
}

void ManualExposureStrategy::setNormalizedGain(CameraDevice& dev, double norm) {
    auto* c = dev.raw(); if (!c) return;
    norm = std::clamp(norm, 0.0, 1.0);
    try {
        ensureManual(c);
        const GainKind gk = pickGain(c);
        const double lo = gnMinOf(c, gk);
        const double hi = gnMaxOf(c, gk);
        writeGain(c, lo + norm * (hi - lo), gk);
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[manual-gain] " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[manual-gain] " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[manual-gain] " + dev.serial());
    }
}

void ManualExposureStrategy::setExposureUs(CameraDevice& dev, double us, double maxExposureUs) {
    auto* c = dev.raw(); if (!c) return;
    try {
        ensureManual(c);
        const ExpKind ek = pickExposure(c);
        const double lo = expMinOf(c, ek);
        const double hiNode = expMaxOf(c, ek);
        const double hi = std::min(hiNode, maxExposureUs);   // never exceed the motion cap
        const double want = std::clamp(us, lo, hi);
        writeExposure(c, want, ek);
        const double actual = expValOf(c, ek);               // read-back
        LOGI() << "[manual-exposure][" << dev.serial() << "] set " << want << "us"
               << (std::fabs(actual - want) > std::max(2.0, 0.05 * want)
                       ? (" (clamped to " + std::to_string(actual) + ")") : "");
    }
    catch (const Pylon::GenericException& e) { AppLogger::LogPylonException(e.GetDescription(), "[manual-exposure] " + dev.serial()); }
    catch (const std::exception& e) { AppLogger::LogException(e, "[manual-exposure] " + dev.serial()); }
    catch (...) { AppLogger::LogUnknownException("[manual-exposure] " + dev.serial()); }
}

void ManualExposureStrategy::setGainAbs(CameraDevice& dev, double gain) {
    auto* c = dev.raw(); if (!c) return;
    try {
        ensureManual(c);
        const GainKind gk = pickGain(c);
        const double lo = gnMinOf(c, gk);
        const double hi = gnMaxOf(c, gk);
        const double want = std::clamp(gain, lo, hi);
        writeGain(c, want, gk);
        LOGI() << "[manual-gain][" << dev.serial() << "] set gain=" << want;
    }
    catch (const Pylon::GenericException& e) { AppLogger::LogPylonException(e.GetDescription(), "[manual-gain] " + dev.serial()); }
    catch (const std::exception& e) { AppLogger::LogException(e, "[manual-gain] " + dev.serial()); }
    catch (...) { AppLogger::LogUnknownException("[manual-gain] " + dev.serial()); }
}

// ---------------------- FixedExposureStrategy ----------------------

void FixedExposureStrategy::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return;
    try {
        ensureManual(c);
        const ExpKind ek = pickExposure(c);
        const GainKind gk = pickGain(c);
        writeExposure(c, exposureUs_, ek);
        writeGain(c, gain_, gk);
        const double actualE = expValOf(c, ek);
        applied_ = true;
        LOGI() << "[fixed-exposure][" << dev.serial() << "] set exposure="
               << exposureUs_ << "us (actual " << actualE << ") gain=" << gain_;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[fixed-exposure] " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[fixed-exposure] " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[fixed-exposure] " + dev.serial());
    }
}

// ---------------------- CameraAutoExposureStrategy ----------------------
// Enable the camera's on-sensor continuous auto-exposure + auto-gain with target
// and limits. Optional nodes are written by NAME via GenApi parameter wrappers,
// trying the modern SFNC name first and the legacy name second, so the same code
// works across USB3/GigE and firmware generations. Each Try* is a no-op (returns
// false) when the node is absent/not writable on the connected model.
void CameraAutoExposureStrategy::configure(CameraDevice& dev) {
    auto* c = dev.raw(); if (!c) return;
    const std::string cam = dev.serial();
    try {
        auto& nm = c->GetNodeMap();

        // Exposure auto bounds: upper = motion-blur cap; lower = sensor minimum
        // (so bright/sunny scenes can drop all the way down and stay sharp).
        const double capUs = double(lim_.maxExposureUs);
        if (!Pylon::CFloatParameter(nm, "AutoExposureTimeUpperLimit").TrySetValue(capUs))
             Pylon::CFloatParameter(nm, "AutoExposureTimeAbsUpperLimit").TrySetValue(capUs);
        if (!Pylon::CFloatParameter(nm, "AutoExposureTimeLowerLimit").TrySetToMinimum())
             Pylon::CFloatParameter(nm, "AutoExposureTimeAbsLowerLimit").TrySetToMinimum();

        // Gain auto bounds: upper = OCR-safe ceiling; lower = minimum.
        if (!Pylon::CFloatParameter(nm, "AutoGainUpperLimit").TrySetValue(double(lim_.maxGain)))
             Pylon::CIntegerParameter(nm, "AutoGainRawUpperLimit").TrySetValue(int64_t(lim_.maxGain));
        if (!Pylon::CFloatParameter(nm, "AutoGainLowerLimit").TrySetValue(double(lim_.minGain)))
             Pylon::CIntegerParameter(nm, "AutoGainRawLowerLimit").TrySetValue(int64_t(lim_.minGain));

        // Target brightness: modern is a 0..1 float; legacy is a 0..255 int.
        const double tb = std::clamp(lim_.target / 255.0, 0.0, 1.0);
        if (!Pylon::CFloatParameter(nm, "AutoTargetBrightness").TrySetValue(tb))
             Pylon::CIntegerParameter(nm, "AutoTargetValue").TrySetValue(int64_t(lim_.target));

        // Prefer raising exposure (up to the cap) before gain -> cleaner for OCR.
        Pylon::CEnumParameter(nm, "AutoFunctionProfile").TrySetValue("MinimizeGain");

        // Hand control to the sensor (typed nodes: always present in the Universal API).
        if (c->ExposureAuto.IsWritable()) c->ExposureAuto.SetValue(UCP::ExposureAuto_Continuous);
        if (c->GainAuto.IsWritable())     c->GainAuto.SetValue(UCP::GainAuto_Continuous);

        applied_ = true;
        LOGI() << "[camera-auto][" << cam << "] ExposureAuto+GainAuto=Continuous"
               << " target=" << lim_.target << " expCap=" << capUs << "us"
               << " gain=[" << lim_.minGain << ".." << lim_.maxGain << "]";
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[camera-auto] " + cam);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "[camera-auto] " + cam); }
    catch (...) { AppLogger::LogUnknownException("[camera-auto] " + cam); }
}
