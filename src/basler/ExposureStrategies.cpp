#include "IExposureStrategy.h"
#include "sunset.h"
#include "AppLogger.h"
#include "ExposureController.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace UCP = Basler_UniversalCameraParams;

namespace {

// ---- Node access (legacy *Raw or modern float), unit-agnostic to the caller --

bool readExposureGain(CameraDevice::PylonCamera* c,
                      double& exposure, double& gain, bool& useRaw) {
    if (c->ExposureTimeRaw.IsWritable() && c->GainRaw.IsWritable()) {
        exposure = c->ExposureTimeRaw.GetValue();
        gain     = c->GainRaw.GetValue();
        useRaw   = true; return true;
    }
    if (c->ExposureTime.IsWritable() && c->Gain.IsWritable()) {
        exposure = c->ExposureTime.GetValue();
        gain     = c->Gain.GetValue();
        useRaw   = false; return true;
    }
    return false;
}

double nodeExposureMin(CameraDevice::PylonCamera* c, bool useRaw) {
    return useRaw ? double(c->ExposureTimeRaw.GetMin()) : c->ExposureTime.GetMin();
}
double readExposure(CameraDevice::PylonCamera* c, bool useRaw) {
    return useRaw ? double(c->ExposureTimeRaw.GetValue()) : c->ExposureTime.GetValue();
}

void writeExposure(CameraDevice::PylonCamera* c, double value, bool useRaw) {
    if (c->ExposureAuto.IsWritable()) c->ExposureAuto.SetValue(UCP::ExposureAuto_Off);
    if (c->ExposureMode.IsWritable()) c->ExposureMode.SetValue(UCP::ExposureMode_Timed);
    if (useRaw && c->ExposureTimeRaw.IsWritable()) c->ExposureTimeRaw.SetValue(int64_t(value));
    else if (c->ExposureTime.IsWritable())         c->ExposureTime.SetValue(value);
}
void writeGain(CameraDevice::PylonCamera* c, double value, bool useRaw) {
    if (c->GainAuto.IsWritable()) c->GainAuto.SetValue(UCP::GainAuto_Off);
    if (useRaw && c->GainRaw.IsWritable()) c->GainRaw.SetValue(int64_t(value));
    else if (c->Gain.IsWritable())         c->Gain.SetValue(value);
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
            double e = 0, g = 0; bool raw = false; readExposureGain(c, e, g, raw);
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

        double exposure, gain; bool useRaw;
        if (!readExposureGain(c, exposure, gain, useRaw)) {
            LOGW() << "[auto-exposure][" << dev.serial()
                   << "] no writable exposure/gain nodes (checked ExposureTimeRaw/GainRaw and "
                      "ExposureTime/Gain)";
            return;
        }
        const double expMin = nodeExposureMin(c, useRaw);
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

        if (std::fabs(dec.exposure - exposure) > 1.0) writeExposure(c, dec.exposure, useRaw);
        if (std::fabs(dec.gain - gain) > 1e-6)        writeGain(c, dec.gain, useRaw);
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
        const bool useRaw = c->ExposureTimeRaw.IsWritable();
        const double lo = useRaw ? double(c->ExposureTimeRaw.GetMin()) : c->ExposureTime.GetMin();
        const double hiNode = useRaw ? double(c->ExposureTimeRaw.GetMax()) : c->ExposureTime.GetMax();
        const double hi = std::min(hiNode, maxExposureUs);   // unified ceiling
        const double want = lo + norm * (hi - lo);
        writeExposure(c, want, useRaw);
        const double actual = readExposure(c, useRaw);       // read-back
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
        const bool useRaw = c->GainRaw.IsWritable();
        const double lo = useRaw ? double(c->GainRaw.GetMin()) : c->Gain.GetMin();
        const double hi = useRaw ? double(c->GainRaw.GetMax()) : c->Gain.GetMax();
        writeGain(c, lo + norm * (hi - lo), useRaw);
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
        const bool useRaw = c->ExposureTimeRaw.IsWritable();
        const double lo = useRaw ? double(c->ExposureTimeRaw.GetMin()) : c->ExposureTime.GetMin();
        const double hiNode = useRaw ? double(c->ExposureTimeRaw.GetMax()) : c->ExposureTime.GetMax();
        const double hi = std::min(hiNode, maxExposureUs);   // never exceed the motion cap
        const double want = std::clamp(us, lo, hi);
        writeExposure(c, want, useRaw);
        const double actual = readExposure(c, useRaw);       // read-back
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
        const bool useRaw = c->GainRaw.IsWritable();
        const double lo = useRaw ? double(c->GainRaw.GetMin()) : c->Gain.GetMin();
        const double hi = useRaw ? double(c->GainRaw.GetMax()) : c->Gain.GetMax();
        const double want = std::clamp(gain, lo, hi);
        writeGain(c, want, useRaw);
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
        const bool useRawE = c->ExposureTimeRaw.IsWritable();
        const bool useRawG = c->GainRaw.IsWritable();
        writeExposure(c, exposureUs_, useRawE);
        writeGain(c, gain_, useRawG);
        const double actualE = readExposure(c, useRawE);
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
