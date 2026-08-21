#include "ConnectionSupervisor.h"
#include "GigEBandwidthCoordinator.h"
#include "SettingsManager_.h"
#include "sunset.h"
#include "AppLogger.h"

#include <pylon/PylonIncludes.h>
#include <pylon/FeaturePersistence.h>
#include <opencv2/core.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>

namespace UCP = Basler_UniversalCameraParams;

ConnectionSupervisor::ConnectionSupervisor(CameraContext& ctx)
    : ctx_(ctx) {}

ConnectionSupervisor::~ConnectionSupervisor() { stopAll(); }

void ConnectionSupervisor::addProfile(const Profile& p) {
    std::lock_guard<std::mutex> lk(profMutex_);
    profiles_[p.serial] = p;
}

bool ConnectionSupervisor::connect(const std::string& serial) {
    return openConfigureStore(serial);
}

// -------------------- the one unified open path --------------------
bool ConnectionSupervisor::openConfigureStore(const std::string& serial) {
    if (serial.empty() || serial == "-1") return false;

    Profile prof;
    {
        std::lock_guard<std::mutex> lk(profMutex_);
        auto it = profiles_.find(serial);
        if (it == profiles_.end()) {
            LOGW() << "[supervisor] no profile for " << serial;
            return false;
        }
        prof = it->second;
    }

    Pylon::IPylonDevice* dev = nullptr;
    try {
        // Enumerate + create under the coordinator's lock so simultaneous
        // reconnects across instances don't storm GigE discovery.
        auto enumLk = GigEBandwidthCoordinator::instance().acquireEnumerationLock();

        Pylon::CDeviceInfo info;
        info.SetSerialNumber(serial.c_str());
        dev = Pylon::CTlFactory::GetInstance().CreateFirstDevice(info);  // throws if none
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[supervisor] create -- " + serial);
        return false;
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[supervisor] create -- " + serial);
        return false;
    }
    catch (...) {
        AppLogger::LogUnknownException("[supervisor] create -- " + serial);
        return false;
    }
    if (!dev) { LOGW() << "[supervisor] device not found: " << serial; return false; }

    // From here a CameraDevice owns 'dev' (Cleanup_Delete); on any failure its
    // destructor releases it -- no manual cleanup needed.
    auto device = std::make_unique<CameraDevice>(dev, serial);
    if (!device->open()) {
        LOGE() << "[supervisor] open failed: " << serial;
        return false;   // device dtor releases
    }

    try {
        // 1) Optional feature file FIRST (gives the full per-camera feature set).
        //    Guard on existence: a configured-but-missing pfs must NOT abort configure()
        //    (that would skip bandwidth/sync and, for a mono master, take down its pair).
        //    Warn and continue instead.
        if (!prof.pfsFile.empty()) {
            std::error_code ec;
            if (!std::filesystem::exists(prof.pfsFile, ec) || ec) {
                LOGW() << "[supervisor] pfs '" << prof.pfsFile << "' not found for "
                       << serial << " -- skipping feature-file load";
            } else {
                Pylon::CFeaturePersistence::Load(prof.pfsFile.c_str(),
                                                 &device->raw()->GetNodeMap(), true);
                LOGI() << "[supervisor] loaded pfs " << prof.pfsFile << " for " << serial;
            }
        }

        // 2) ROI, bandwidth, then sync role (role wins on trigger/line nodes).
        applyRoi(*device, prof);
        applyBandwidth(*device, prof);

        std::shared_ptr<ISyncConfigurator> role =
            prof.freeRun ? std::make_shared<FreeRunConfigurator>()
                         : chooseSyncConfigurator(prof.triggerOn);
        const bool syncOk = role->configure(*device);
        if (!syncOk) {
            // Decision: do NOT refuse the camera -- a half-working setup may still
            // be useful. But signal loudly so the system/operator knows sync is
            // broken (e.g. a required trigger/line node could not be applied).
            LOGE() << "[supervisor] " << serial
                   << " sync role '" << role->role()
                   << "' did NOT fully apply -- camera will run but sync is unreliable";
            if (ctx_.sink) ctx_.sink->onCaptureError(-2);   // -2 = sync misconfig (non-fatal)
        }

        // 3) Build the exposure strategy for this camera by role.
        std::unique_ptr<IExposureStrategy> exp = makeExposureStrategy(prof, *device);

        // 3b) Snapshot the fully-configured node map as the "load-time settings"
        //     baseline, so a manual-control session can be reverted to exactly the
        //     state the camera had when the system loaded it from NATS.
        try {
            Pylon::String_t snap;
            Pylon::CFeaturePersistence::SaveToString(snap, &device->raw()->GetNodeMap());
            std::lock_guard<std::mutex> pk(profMutex_);
            baseline_[serial] = std::string(snap.c_str());
        } catch (const Pylon::GenericException& e) {
            LOGW() << "[supervisor] baseline snapshot failed for " << serial
                   << ": " << e.GetDescription();
        }

        // 3c) If the operator was mid manual-tuning when the camera dropped, apply the
        //     override to THIS device NOW (before it is published) and hold it. Doing it
        //     here — on the reconnect thread but on a not-yet-shared device — avoids the
        //     old post-publish reapply that mutated already-published exposure/nodes and
        //     could race the capture thread (use-after-free + concurrent Pylon access).
        {
            ManualOverride ov; bool have = false;
            { std::lock_guard<std::mutex> mk(manualMutex_);
              auto it = manual_.find(serial);
              if (it != manual_.end() && (it->second.hasExp || it->second.hasGain)) { ov = it->second; have = true; } }
            if (have) {
                double cap = prof.exposureLimits.maxExposureUs > 0
                             ? double(prof.exposureLimits.maxExposureUs) : 50000.0;
                if (ov.hasExp) {
                    const bool abs = (ov.expUnit == ExposureUnit::Absolute) ||
                                     (ov.expUnit == ExposureUnit::Auto && ov.exp > 1.0);
                    if (abs) ManualExposureStrategy::setExposureUs(*device, ov.exp, std::numeric_limits<double>::max());
                    else     ManualExposureStrategy::setNormalizedExposure(*device, ov.exp, cap);
                }
                if (ov.hasGain) {
                    const bool abs = (ov.gainUnit == ExposureUnit::Absolute) ||
                                     (ov.gainUnit == ExposureUnit::Auto && ov.gain > 1.0);
                    if (abs) ManualExposureStrategy::setGainAbs(*device, ov.gain);
                    else     ManualExposureStrategy::setNormalizedGain(*device, ov.gain);
                }
                exp = std::make_unique<ManualExposureStrategy>();   // hold, suspend the auto loop
                LOGI() << "[exposure][" << serial << "] manual override re-applied at (re)connect";
            }
        }

        // 4) Store everything under the map lock (insertion is the only thing
        //    that happens off the capture thread).
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            ctx_.exposure[serial] = std::move(exp);
            ctx_.syncRole[serial] = std::move(role);
            ctx_.devices[serial]  = std::move(device);
        }

        // 5) Record the (re)connect time. Do NOT reset the backoff here: a camera that
        //    OPENS + configures fine but then can't GRAB (marginal cable, a switch/port
        //    that won't pass jumbo, or an oversubscribed link) would otherwise reset to
        //    "attempt #1, backoff 1s" on EVERY reconnect and hammer re-open + bandwidth
        //    re-negotiation + GigE discovery forever. handleFault() resets the backoff
        //    only after a genuinely STABLE run (>= kStableResetSec); a quick drop escalates.
        { std::lock_guard<std::mutex> lk(reconMutex_); connectedAt_[serial] = std::chrono::steady_clock::now(); }

        LOGI() << "[supervisor] connected " << serial;
        return true;
    }
    catch (const cv::Exception& e) {
        AppLogger::LogCvException(e, "[supervisor] configure -- " + serial);
        return false;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[supervisor] configure -- " + serial);
        return false;   // device (still local unique_ptr) releases on scope exit
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[supervisor] configure -- " + serial);
        return false;
    }
    catch (...) {
        AppLogger::LogUnknownException("[supervisor] configure -- " + serial);
        return false;
    }
}

std::unique_ptr<IExposureStrategy>
ConnectionSupervisor::makeExposureStrategy(const Profile& prof, CameraDevice& dev) {
    if (prof.cameraNativeAuto) {
        // On-sensor continuous auto-exposure + auto-gain ("same as pylon").
        auto ca = std::make_unique<CameraAutoExposureStrategy>(prof.exposureLimits);
        ca->configure(dev);                 // enable continuous auto now
        return ca;
    }
    if (prof.fixedExposure) {
        // Mono plate camera under IR: set a low fixed exposure once and hold.
        auto fx = std::make_unique<FixedExposureStrategy>(prof.fixedExposureUs, prof.fixedGain);
        fx->configure(dev);                 // apply the fixed value now
        return fx;
    }
    if (prof.autoExposure)
        return std::make_unique<AutoExposureStrategy>(prof.exposureLimits);
    return std::make_unique<ManualExposureStrategy>();
}

// ---- IExposureController (manual override + restore configured settings) ----
// All three run on the capture thread (command drain). They touch ctx_.exposure
// and ctx_.devices, which reconnect workers also write, so take devMutex.

void ConnectionSupervisor::setManualExposure(const std::string& serial, double value, ExposureUnit unit) {
    // Resolve the unit: an explicit Absolute/Normalized wins; Auto keeps the legacy
    // heuristic (value > 1.0 is absolute microseconds, else a 0..1 fraction).
    const bool absolute = (unit == ExposureUnit::Absolute) ||
                          (unit == ExposureUnit::Auto && value > 1.0);

    // Copy the motion-blur cap from the profile first (own lock), then act on the
    // device (own lock) -- never nest, to match openConfigureStore's lock order.
    double cap = 50000.0;
    { std::lock_guard<std::mutex> pk(profMutex_);
      auto pit = profiles_.find(serial);
      if (pit != profiles_.end() && pit->second.exposureLimits.maxExposureUs > 0)
          cap = double(pit->second.exposureLimits.maxExposureUs); }

    // Remember the override so a reconnect re-applies it (cleared by revertToSettings).
    { std::lock_guard<std::mutex> mk(manualMutex_);
      auto& m = manual_[serial]; m.hasExp = true; m.exp = value; m.expUnit = unit; }

    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto dit = ctx_.devices.find(serial);
    if (dit == ctx_.devices.end() || !dit->second) {
        LOGW() << "[exposure][" << serial << "] manual exposure: camera not connected";
        return;
    }
    //   * Normalized: map 0..1 over [min, motion-blur cap] so the slider keeps fine
    //     resolution in the useful range.
    //   * Absolute: the operator asked for a specific exposure -- honor it up to the
    //     sensor's OWN hardware maximum, not the auto motion-blur cap, so manual setup
    //     (focus / static scene / long-exposure night test) has the full range.
    if (absolute)
        ManualExposureStrategy::setExposureUs(*dit->second, value, std::numeric_limits<double>::max());
    else
        ManualExposureStrategy::setNormalizedExposure(*dit->second, value, cap);
    ctx_.exposure[serial] = std::make_unique<ManualExposureStrategy>();  // suspend auto loop
    LOGI() << "[exposure][" << serial << "] MANUAL exposure "
           << (absolute ? "abs=" : "norm=") << value
           << (absolute ? " (hardware max, auto suspended)"
                        : (" (cap=" + std::to_string(cap) + "us, auto suspended)"));
}

void ConnectionSupervisor::setManualGain(const std::string& serial, double value, ExposureUnit unit) {
    const bool absolute = (unit == ExposureUnit::Absolute) ||
                          (unit == ExposureUnit::Auto && value > 1.0);

    { std::lock_guard<std::mutex> mk(manualMutex_);
      auto& m = manual_[serial]; m.hasGain = true; m.gain = value; m.gainUnit = unit; }

    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto dit = ctx_.devices.find(serial);
    if (dit == ctx_.devices.end() || !dit->second) {
        LOGW() << "[exposure][" << serial << "] manual gain: camera not connected";
        return;
    }
    if (absolute) ManualExposureStrategy::setGainAbs(*dit->second, value);
    else          ManualExposureStrategy::setNormalizedGain(*dit->second, value);
    ctx_.exposure[serial] = std::make_unique<ManualExposureStrategy>();  // suspend auto loop
    LOGI() << "[exposure][" << serial << "] MANUAL gain "
           << (absolute ? "abs=" : "norm=") << value << " (auto suspended)";
}

void ConnectionSupervisor::revertToSettings(const std::string& serial) {
    // Ending the manual session: drop any stored override so a later reconnect does
    // NOT resurrect the manual exposure/gain.
    { std::lock_guard<std::mutex> mk(manualMutex_); manual_.erase(serial); }

    // Copy profile + baseline snapshot first (own lock), then act on the device.
    Profile prof; std::string baseline; bool haveProf = false;
    { std::lock_guard<std::mutex> pk(profMutex_);
      auto pit = profiles_.find(serial);
      if (pit != profiles_.end()) { prof = pit->second; haveProf = true; }
      auto bit = baseline_.find(serial);
      if (bit != baseline_.end()) baseline = bit->second; }

    if (!haveProf) {
        LOGW() << "[exposure][" << serial << "] revert: no stored profile";
        return;
    }

    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto dit = ctx_.devices.find(serial);
    if (dit == ctx_.devices.end() || !dit->second) {
        LOGW() << "[exposure][" << serial << "] revert: camera not connected";
        return;
    }
    auto& dev = dit->second;

    // Stop grabbing first. During a live grab, Basler keeps the acquisition-LOCKED nodes
    // (TriggerMode/TriggerSource, ROI, PixelFormat) read-only, so a restore-while-grabbing silently
    // skips them -- which is exactly why a triggered camera would NOT return to its configured
    // state on Reset. Stopping unlocks them. (Safe: revert runs on the capture thread, in the same
    // loop as retrieveBGR, so no grab is in flight here.)
    const bool wasGrabbing = dev->isGrabbing();
    if (wasGrabbing) dev->stopGrabbing();

    // 1) Restore EVERY node to its load-time value (gamma, white balance, ROI, exposure, trigger,
    //    ... whatever a manual session changed). With the grab stopped, the trigger/geometry nodes
    //    now actually apply (verify=false still tolerates any node a given model refuses).
    if (!baseline.empty()) {
        try {
            Pylon::CFeaturePersistence::LoadFromString(Pylon::String_t(baseline.c_str()),
                                                       &dev->raw()->GetNodeMap(), false);
        } catch (const Pylon::GenericException& e) {
            LOGW() << "[exposure][" << serial << "] baseline restore: " << e.GetDescription();
        }
    }

    // 2) Re-apply the configured sync role authoritatively (free-run vs Line1-hardware vs software
    //    trigger), so the trigger state matches the loaded settings regardless of what a manual or
    //    'Trigger Mode' session left behind.
    {
        auto sit = ctx_.syncRole.find(serial);
        if (sit != ctx_.syncRole.end() && sit->second) sit->second->configure(*dev);
    }

    // 3) Reinstall the configured exposure strategy so the per-frame loop resumes correctly
    //    (re-enables camera-native/fixed; fresh host-auto).
    ctx_.exposure[serial] = makeExposureStrategy(prof, *dev);

    // 4) Resume grabbing.
    if (wasGrabbing) dev->startGrabbing();

    LOGI() << "[exposure][" << serial << "] reverted to load-time camera settings"
           << (wasGrabbing ? " (grab stopped/restarted to restore trigger+ROI)" : "");
}

bool ConnectionSupervisor::readExposureGain(const std::string& serial, double& exposureUs, double& gain) {
    // Read the exposure (microseconds) and gain the camera ACTUALLY holds right now, so the
    // manual-control UI can report ground truth (post-clamp / post-increment coercion) instead of
    // the requested value. Called on the capture thread (from the live-frame path), same thread as
    // the grab, so node reads don't race. Prefers the modern float nodes (ExposureTime us / Gain dB)
    // and falls back to the legacy *Raw nodes.
    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto it = ctx_.devices.find(serial);
    if (it == ctx_.devices.end() || !it->second) return false;
    auto* c = it->second->raw();
    if (!c) return false;
    try {
        if      (c->ExposureTime.IsReadable())    exposureUs = c->ExposureTime.GetValue();
        else if (c->ExposureTimeAbs.IsReadable()) exposureUs = c->ExposureTimeAbs.GetValue();  // GigE SFNC-1 (us)
        else if (c->ExposureTimeRaw.IsReadable()) exposureUs = double(c->ExposureTimeRaw.GetValue());
        else return false;
        if      (c->Gain.IsReadable())    gain = c->Gain.GetValue();
        else if (c->GainAbs.IsReadable()) gain = c->GainAbs.GetValue();
        else if (c->GainRaw.IsReadable()) gain = double(c->GainRaw.GetValue());
        else gain = 0.0;
        return true;
    }
    catch (const Pylon::GenericException&) { return false; }
    catch (...) { return false; }
}

bool ConnectionSupervisor::readAoi(const std::string& serial, lpr::CaptureSource::Aoi& out) {
    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto it = ctx_.devices.find(serial);
    if (it == ctx_.devices.end() || !it->second) return false;
    auto* c = it->second->raw();
    if (!c) return false;
    try {
        if (c->Width.IsReadable())   { out.width  = c->Width.GetValue();  out.widthMax  = c->Width.GetMax();  out.widthInc  = std::max<int64_t>(1, c->Width.GetInc()); }
        if (c->Height.IsReadable())  { out.height = c->Height.GetValue(); out.heightMax = c->Height.GetMax(); out.heightInc = std::max<int64_t>(1, c->Height.GetInc()); }
        if (c->OffsetX.IsReadable()) { out.offsetX = c->OffsetX.GetValue(); out.offsetXInc = std::max<int64_t>(1, c->OffsetX.GetInc()); }
        if (c->OffsetY.IsReadable()) { out.offsetY = c->OffsetY.GetValue(); out.offsetYInc = std::max<int64_t>(1, c->OffsetY.GetInc()); }
        return out.widthMax > 0 && out.heightMax > 0;
    }
    catch (const Pylon::GenericException&) { return false; }
    catch (...) { return false; }
}

void ConnectionSupervisor::applyRoi(CameraDevice& dev, const Profile& p) {
    // Hardware sensor AOI crop from the NORMALIZED rect in the profile (set per role in
    // buildProfile: mono plate cam always, colour cam only when enabled). Shrinking the
    // transmitted frame is a REAL GigE bandwidth cut (fewer bytes/frame) — unlike the
    // software ROI, which still captures + sends the whole sensor. cropWn/Hn<=0 => full frame.
    if (p.cropWn <= 0.0 || p.cropHn <= 0.0) {
        LOGI() << "[supervisor][aoi][" << dev.serial()
               << "] full-frame (no hardware AOI); detection still crops to the ROI in software";
        return;
    }
    auto* c = dev.raw(); if (!c) return;
    try {
        // Round DOWN to the nearest valid increment at/above the node minimum (a value
        // below min would make SetValue throw), returned as int64.
        auto align = [](int64_t v, int64_t mn, int64_t inc) -> int64_t {
            const int64_t a = inc > 0 ? mn + ((v - mn) / inc) * inc : v;
            return a < mn ? mn : a;
        };
        // Zero the offsets FIRST so Width/Height can reach the full sensor max; then read
        // the sensor size, set the (smaller) Width/Height, then position the offsets. This
        // is the order Basler requires — a new Width with an old non-zero offset can exceed
        // the max and throw.
        if (c->OffsetX.IsWritable()) c->OffsetX.SetValue(c->OffsetX.GetMin());
        if (c->OffsetY.IsWritable()) c->OffsetY.SetValue(c->OffsetY.GetMin());
        const int64_t sensorW = c->Width.IsReadable()  ? c->Width.GetMax()  : 0;
        const int64_t sensorH = c->Height.IsReadable() ? c->Height.GetMax() : 0;
        if (sensorW <= 0 || sensorH <= 0) { LOGW() << "[supervisor][aoi][" << dev.serial() << "] sensor size unknown -> skip"; return; }

        int64_t w = align((int64_t)std::llround(p.cropWn * sensorW), c->Width.GetMin(),  c->Width.GetInc());
        int64_t h = align((int64_t)std::llround(p.cropHn * sensorH), c->Height.GetMin(), c->Height.GetInc());
        w = std::clamp<int64_t>(w, c->Width.GetMin(),  sensorW);
        h = std::clamp<int64_t>(h, c->Height.GetMin(), sensorH);
        if (c->Width.IsWritable())  c->Width.SetValue(w);
        if (c->Height.IsWritable()) c->Height.SetValue(h);

        int64_t x = align((int64_t)std::llround(p.cropXn * sensorW), c->OffsetX.GetMin(), c->OffsetX.GetInc());
        int64_t y = align((int64_t)std::llround(p.cropYn * sensorH), c->OffsetY.GetMin(), c->OffsetY.GetInc());
        x = std::clamp<int64_t>(x, c->OffsetX.GetMin(), sensorW - w);   // keep offset+size within the sensor
        y = std::clamp<int64_t>(y, c->OffsetY.GetMin(), sensorH - h);
        if (c->OffsetX.IsWritable()) c->OffsetX.SetValue(x);
        if (c->OffsetY.IsWritable()) c->OffsetY.SetValue(y);

        LOGI() << "[supervisor][aoi][" << dev.serial() << "] hardware AOI " << w << "x" << h
               << " @ (" << x << "," << y << ") from norm(" << p.cropXn << "," << p.cropYn
               << "," << p.cropWn << "," << p.cropHn << ") sensor=" << sensorW << "x" << sensorH;
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[supervisor] aoi -- " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[supervisor] aoi -- " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[supervisor] aoi -- " + dev.serial());
    }
}

void ConnectionSupervisor::applyBandwidth(CameraDevice& dev, const Profile& prof) {
    auto* c = dev.raw(); if (!c) return;
    const std::string serial = dev.serial();
    try {
        // Multi-camera GigE bandwidth (this camera has no DeviceLinkThroughputLimit):
        //   * Packet size = MTU (1500 unmanaged switch; jumbo only end-to-end).
        //   * GevSCPD  (inter-packet delay)   throttles each camera to ~link/N so the
        //     summed throughput fits the pipe. Computed in TICKS from the tick
        //     frequency (NOT bytes), with a byte heuristic only as a fallback.
        //   * GevSCFTD (frame transmission delay) staggers each camera's frame start
        //     across the frame period (idx * framePeriod/N), also in ticks.
        //   * GevSCBWR reserves 10-30% for resends; GevSCBWA is logged so the per-NIC
        //     sum can be checked against the link. N and idx are fully dynamic.
        auto& coord = GigEBandwidthCoordinator::instance();
        const auto  cfg = coord.linkConfig();
        const std::string nic = dev.subnet();
        // Dynamic camera count: use the configured number of cameras (matches the
        // proven adjustCameraBandwidth), falling back to the live count on this NIC.
        // Index is stable per serial across reconnects.
        int num_cam = SettingsManager::instance().get_num_camera();
        if (num_cam <= 0) num_cam = std::max(1, coord.countInGroup(nic));
        // Stagger index must be bounded by the count ON THIS NIC (registerCamera hands out
        // per-NIC indices), NOT the global camera total — otherwise, if a NIC holds more
        // cameras than the configured global count, two per-NIC indices clamp to the same
        // value and get an identical GevSCFTD offset, defeating the stagger. (The divisor
        // stays the global total: conservative — never under-throttles a shared NIC.)
        const int nicCount = std::max(1, coord.countInGroup(nic));
        const int idx = std::clamp(coord.registerCamera(serial, nic), 0, nicCount - 1);

        LOGI() << "[supervisor][bw][" << serial << "] start, num_cam=" << num_cam
               << " idx=" << idx << " nic=" << nic;

        // 1) Stream channel 0
        if (c->GevStreamChannelSelector.IsWritable())
            c->GevStreamChannelSelector.SetValue(UCP::GevStreamChannelSelector_StreamChannel0);

        // 2) Packet size (GevSCPSPacketSize): bigger packets mean far fewer packets per frame,
        //    which is the single biggest fix for incomplete-frame (BadFrame / status=2) drops on
        //    multi-camera GigE. Default 8960 (jumbo). Settings key 'gige_packet_size'.
        //    IMPORTANT: jumbo REQUIRES it enabled end-to-end -- the NIC MTU must be >= 9000 and the
        //    switch must pass jumbo frames. If it isn't, oversized packets are dropped and EVERY
        //    frame fails, so set gige_packet_size back to 1500 for a non-jumbo network. The value
        //    is clamped to the camera's own supported min/max, and the byte math below uses the
        //    ACTUAL applied size (read back), so the bandwidth throttle stays correct either way.
        int64_t wantPkt = SettingsManager::instance().getLpr<int>("gige_packet_size").value_or(1500);
        // Per-camera auto-fallback: if THIS camera has been flapping on jumbo, handleFault
        // pins it to 1500 so it can recover without disturbing the healthy jumbo cameras.
        { std::lock_guard<std::mutex> lk(reconMutex_);
          auto ov = packetOverride_.find(serial);
          if (ov != packetOverride_.end() && ov->second > 0) {
              if (ov->second != wantPkt)
                  LOGW() << "[supervisor][bw][" << serial << "] using per-camera packet override "
                         << ov->second << " (global " << wantPkt << ") -- jumbo auto-fallback";
              wantPkt = ov->second;
          } }
        if (c->GevSCPSPacketSize.IsWritable()) {
            const int64_t pkt = std::clamp<int64_t>(wantPkt,
                c->GevSCPSPacketSize.GetMin(), c->GevSCPSPacketSize.GetMax());
            c->GevSCPSPacketSize.SetValue(pkt);
        }
        const int64_t pktApplied =
            c->GevSCPSPacketSize.IsReadable() ? c->GevSCPSPacketSize.GetValue() : wantPkt;
        const int64_t slotBytes = pktApplied + cfg.packetOverhead;

        // Tick frequency + usable link rate drive the *time-correct* delays below.
        // GevSCPD/GevSCFTD are in TIMESTAMP TICKS, not bytes -- so convert seconds
        // to ticks via GevTimestampTickFrequency. (DeviceLinkThroughputLimit isn't
        // used: this camera doesn't support it.)
        SettingsManager& sm = SettingsManager::instance();
        // Usable shared-link bandwidth (bytes/sec) that all N cameras must fit under. Prefer the
        // camera's ACTUAL negotiated GigE link speed (GevLinkSpeed, in Mbps) instead of assuming a
        // full gigabit -- so a port that fell back to 100 Mbps is throttled correctly and stops
        // dropping packets (the incomplete-frame / status=2 problem). Manual override: set LPR
        // 'gige_link_mbytes_per_sec' > 0 to force a value (e.g. when the HOST uplink, not the
        // camera's own link, is the real bottleneck). 0 or absent => auto-detect.
        double linkBytesPerSec;
        const int manualLinkMBps = sm.getLpr<int>("gige_link_mbytes_per_sec").value_or(0);
        if (manualLinkMBps > 0) {
            linkBytesPerSec = (double)manualLinkMBps * 1e6;
            LOGI() << "[supervisor][bw][" << serial << "] link=manual " << manualLinkMBps << " MB/s";
        } else {
            int64_t linkMbps = c->GevLinkSpeed.IsReadable() ? c->GevLinkSpeed.GetValue() : 0;
            if (linkMbps <= 0) linkMbps = 1000;   // fallback: assume gigabit if the node is unreadable
            // ~72% of the raw line rate is realistically usable for image payload (GigE Vision
            // overhead + inter-packet gaps): 1000 Mbps -> ~90 MB/s (matches the old fixed default),
            // 100 Mbps -> ~9 MB/s.
            linkBytesPerSec = (double)linkMbps * 1e6 / 8.0 * 0.72;
            LOGI() << "[supervisor][bw][" << serial << "] link=auto GevLinkSpeed=" << linkMbps
                   << " Mbps -> usable " << (int64_t)(linkBytesPerSec / 1e6) << " MB/s";
        }
        const double tickHz = c->GevTimestampTickFrequency.IsReadable()
                            ? (double)c->GevTimestampTickFrequency.GetValue() : 0.0;
        const int64_t scpdFloor = sm.getLpr<int>("gige_scpd_floor").value_or(2000);

        // Target frame-rate cap (what step 7 applies). Computed HERE from the link/N budget
        // and PayloadSize so the SCFTD stagger below is sized for the REAL (capped) frame
        // period, not the uncapped sensor rate. GevSCBWA isn't valid yet (SCPD/reserve set
        // below), so the link/N budget is used — the same value the fps cap falls back to.
        double capTargetFps = 0.0;
        {
            const int64_t payloadEarly = c->PayloadSize.IsReadable() ? c->PayloadSize.GetValue() : 0;
            const double  budgetEarly  = linkBytesPerSec / (double)num_cam;
            if (payloadEarly > 0 && budgetEarly > 0.0) {
                const int safetyPct = std::clamp<int>(sm.getLpr<int>("gige_fps_safety_pct").value_or(85), 10, 100);
                const int userCap   = sm.getLpr<int>("gige_frame_rate_cap").value_or(0);
                capTargetFps = (budgetEarly / (double)payloadEarly) * ((double)safetyPct / 100.0);
                if (userCap > 0) capTargetFps = std::min(capTargetFps, (double)userCap);
            }
        }

        // 3) Inter-packet delay (GevSCPD): stretch each camera to ~link/N so the N
        //    cameras' summed throughput fits the pipe. Delay per packet (seconds) =
        //    packetTxTime * (N-1), where packetTxTime = slotBytes / linkBytesPerSec.
        //    Converted to ticks. Falls back to the byte heuristic if tick freq is
        //    unavailable. Single camera gets a small floor.
        if (c->GevSCPD.IsWritable()) {
            int64_t scpd;
            if (num_cam <= 1) {
                scpd = scpdFloor;
            } else if (tickHz > 0.0 && linkBytesPerSec > 0.0) {
                const double pktTxSec = (double)slotBytes / linkBytesPerSec;
                const double delaySec = pktTxSec * (double)(num_cam - 1);
                scpd = (int64_t)(delaySec * tickHz);                 // seconds -> ticks
            } else {
                scpd = slotBytes * (int64_t)(num_cam - 1);           // byte fallback
            }
            scpd = std::clamp<int64_t>(scpd, c->GevSCPD.GetMin(), c->GevSCPD.GetMax());
            c->GevSCPD.SetValue(scpd);
            const int64_t got = c->GevSCPD.IsReadable() ? c->GevSCPD.GetValue() : -1;
            LOGI() << "[supervisor][bw][" << serial << "] SCPD set=" << scpd
                   << " actual=" << got << " ticks (tickHz=" << tickHz
                   << " slotBytes=" << slotBytes << " N=" << num_cam << ")";
        } else {
            LOGI() << "[supervisor][bw][" << serial << "] SCPD not writable -> skipped";
        }

        // 4) Frame transmission delay (GevSCFTD): stagger each camera's frame start
        //    across the frame period so they don't all transmit at once:
        //    offset = idx * (framePeriod / N), in ticks. Needs fps + tick freq;
        //    falls back to the byte heuristic otherwise.
        if (c->GevSCFTD.IsWritable()) {
            // Use the INTENDED capped rate (what step 7 applies) so the stagger matches the
            // real frame period; fall back to the current sensor rate only if unknown.
            double fps = capTargetFps;
            if (fps <= 0.1) {
                if      (c->AcquisitionFrameRate.IsReadable())    fps = c->AcquisitionFrameRate.GetValue();
                else if (c->AcquisitionFrameRateAbs.IsReadable()) fps = c->AcquisitionFrameRateAbs.GetValue();
            }
            int64_t scftd;
            if (tickHz > 0.0 && fps > 0.1) {
                const double framePeriodSec = 1.0 / fps;
                const double offsetSec = (framePeriodSec / (double)num_cam) * (double)idx;
                scftd = (int64_t)(offsetSec * tickHz);               // seconds -> ticks
            } else {
                scftd = slotBytes * (int64_t)idx;                    // byte fallback
            }
            scftd = std::clamp<int64_t>(scftd, c->GevSCFTD.GetMin(), c->GevSCFTD.GetMax());
            c->GevSCFTD.SetValue(scftd);
            const int64_t got = c->GevSCFTD.IsReadable() ? c->GevSCFTD.GetValue() : -1;
            LOGI() << "[supervisor][bw][" << serial << "] SCFTD set=" << scftd
                   << " actual=" << got << " ticks (idx=" << idx << "/" << num_cam << ")";
        } else {
            LOGI() << "[supervisor][bw][" << serial << "] SCFTD not writable -> skipped";
        }

        // 5) Bandwidth reserve (GevSCBWR): % of the assigned bandwidth held back for
        //    packet resends/control. Basler recommends 10-30% for robustness when
        //    not chasing maximum performance.
        if (c->GevSCBWR.IsWritable()) {
            const int64_t pct = sm.getLpr<int>("gige_bw_reserve_pct").value_or(18);
            const int64_t r = std::clamp<int64_t>(pct, c->GevSCBWR.GetMin(), c->GevSCBWR.GetMax());
            c->GevSCBWR.SetValue(r);
            LOGI() << "[supervisor][bw][" << serial << "] SCBWR reserve=" << r << "%";
        }

        // 6) Verification: GevSCBWA is the read-only bandwidth (B/s) the camera is
        //    actually assigned. Sum this across all cameras on the NIC and confirm
        //    the total stays under the usable link (~" << ... ").
        const int64_t bwa = c->GevSCBWA.IsReadable() ? c->GevSCBWA.GetValue() : -1;
        const int64_t scpdFinal  = c->GevSCPD.IsReadable()  ? c->GevSCPD.GetValue()  : -1;
        const int64_t scftdFinal = c->GevSCFTD.IsReadable() ? c->GevSCFTD.GetValue() : -1;
        LOGI() << "[supervisor][bw][" << serial << "] done pkt=" << pktApplied
               << " scpd=" << scpdFinal << " scftd=" << scftdFinal
               << " bandwidthAssigned=" << bwa << " B/s (sum across cameras must stay < "
               << (int64_t)linkBytesPerSec << " B/s) N=" << num_cam << " idx=" << idx;

        // 7) Frame-rate cap -- the piece the Pylon "optimize bandwidth" wizard does
        //    automatically, and the reason cameras drop here without it.
        //    SCPD/SCFTD only stagger/space packets; they do NOT stop the sensor from
        //    *generating* frames faster than its assigned bandwidth (GevSCBWA) can carry.
        //    In free-run a 1920x1200 camera runs at its full sensor rate (tens of fps),
        //    each frame ~PayloadSize bytes -- many times its ~link/N slice. The stream
        //    channel then overflows, packets are lost, frames return incomplete
        //    (status=2 BadFrame), and the camera faults + reconnects in a loop
        //    (exactly the "transient grab miss -> grab failed -> reconnect" pattern).
        //    Fix: cap AcquisitionFrameRate to what the assigned bandwidth can sustain,
        //    fps <= GevSCBWA / PayloadSize, scaled by a safety margin. This adapts to
        //    the ACTUAL link speed (GevSCBWA already reflects the auto-detected
        //    GevLinkSpeed + SCPD + reserve) and to the live camera count, so on a slow
        //    or crowded NIC the rate drops instead of the camera going offline.
        //
        //    ONLY for free-running cameras. A triggered slave (config-1: TriggerMode=On)
        //    captures one frame per external trigger from the master, so AcquisitionFrameRate
        //    does not govern its rate -- and forcing AcquisitionFrameRateEnable=true can, on
        //    some models, cap trigger acceptance and DROP synchronized frames. Its bandwidth
        //    is already bounded because the master's (free-run) rate, which drives the
        //    trigger, is itself capped here. So skip the cap for a slave. (SCPD/SCFTD/SCBWR
        //    above still apply -- the slave still needs its packet delay + stagger + reserve.)
        if (prof.triggerOn) {
            LOGI() << "[supervisor][fps][" << serial << "] triggered slave -> frame-rate cap"
                   << " skipped (rate follows master trigger)";
            return;
        }
        const int64_t payload = c->PayloadSize.IsReadable() ? c->PayloadSize.GetValue() : 0;
        // Budget = the camera's own assigned bandwidth if the node reports it (already
        // net of SCPD + reserve); else fall back to the computed link/N share.
        double budgetBps = (bwa > 0) ? (double)bwa : (linkBytesPerSec / (double)num_cam);
        const int fpsSafetyPct = std::clamp<int>(sm.getLpr<int>("gige_fps_safety_pct").value_or(85), 10, 100);
        const int userFpsCap   = sm.getLpr<int>("gige_frame_rate_cap").value_or(0);  // 0 => auto only
        if (payload > 0 && budgetBps > 0.0) {
            double capFps = (budgetBps / (double)payload) * ((double)fpsSafetyPct / 100.0);
            if (userFpsCap > 0) capFps = std::min(capFps, (double)userFpsCap);
            // Prefer the modern AcquisitionFrameRate node; fall back to the legacy
            // ...Abs node. Both need AcquisitionFrameRateEnable=true to take effect.
            bool applied = false; double got = -1.0;
            try {
                if (c->AcquisitionFrameRateEnable.IsWritable())
                    c->AcquisitionFrameRateEnable.SetValue(true);
                if (c->AcquisitionFrameRate.IsWritable()) {
                    const double lo = c->AcquisitionFrameRate.GetMin();
                    const double hi = c->AcquisitionFrameRate.GetMax();
                    const double v  = std::clamp(capFps, lo, hi);
                    c->AcquisitionFrameRate.SetValue(v);
                    got = c->AcquisitionFrameRate.IsReadable() ? c->AcquisitionFrameRate.GetValue() : v;
                    applied = true;
                } else if (c->AcquisitionFrameRateAbs.IsWritable()) {
                    const double lo = c->AcquisitionFrameRateAbs.GetMin();
                    const double hi = c->AcquisitionFrameRateAbs.GetMax();
                    const double v  = std::clamp(capFps, lo, hi);
                    c->AcquisitionFrameRateAbs.SetValue(v);
                    got = c->AcquisitionFrameRateAbs.IsReadable() ? c->AcquisitionFrameRateAbs.GetValue() : v;
                    applied = true;
                }
            } catch (const Pylon::GenericException& e) {
                LOGW() << "[supervisor][fps][" << serial << "] frame-rate cap write failed: "
                       << e.GetDescription();
            }
            if (applied)
                LOGI() << "[supervisor][fps][" << serial << "] cap=" << got << " fps"
                       << " (budget=" << (int64_t)budgetBps << " B/s / payload=" << payload
                       << " B, safety=" << fpsSafetyPct << "%"
                       << (userFpsCap > 0 ? (", userCap=" + std::to_string(userFpsCap)) : std::string())
                       << ", N=" << num_cam << ")";
            else
                LOGW() << "[supervisor][fps][" << serial << "] AcquisitionFrameRate not writable"
                       << " -- cannot cap rate; camera may overrun its bandwidth slot";
        } else {
            LOGW() << "[supervisor][fps][" << serial << "] no PayloadSize/bandwidth -> frame-rate cap skipped";
        }
    }
    catch (const Pylon::GenericException& e) {
        // Bandwidth tuning is not fatal.
        AppLogger::LogPylonException(e.GetDescription(), "[supervisor] bandwidth -- " + serial);
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[supervisor] bandwidth -- " + serial);
    }
    catch (...) {
        AppLogger::LogUnknownException("[supervisor] bandwidth -- " + serial);
    }
}

// -------------------- fault + reconnect --------------------

void ConnectionSupervisor::handleFault(const std::string& serial) {
    // A camera DROP: make it visible in the log (paired with the "[supervisor]
    // connected <serial>" line emitted on a successful reconnect), so an operator
    // can see the disconnect/recover cycle instead of only inferring the drop from
    // the CapturePipeline grab-failure warning one layer up.
    LOGW() << "[supervisor][" << serial << "] camera DROPPED -> removing device and scheduling reconnect";
    // Runs on the capture thread: remove + release the device here.
    std::unique_ptr<CameraDevice> dead;
    {
        std::lock_guard<std::mutex> lk(ctx_.devMutex);
        auto it = ctx_.devices.find(serial);
        if (it != ctx_.devices.end()) { dead = std::move(it->second); ctx_.devices.erase(it); }
        ctx_.exposure.erase(serial);
        ctx_.syncRole.erase(serial);
    }
    dead.reset();   // CameraDevice dtor stops/closes/detaches/destroys
    // NB: we deliberately do NOT unregister from the bandwidth coordinator, so
    // the camera keeps its stagger slot across the reconnect.

    // Flapping vs transient. If the camera dropped SHORTLY after (re)connecting, it is
    // unstable — it opens but the grab keeps failing (status=2 incomplete frames):
    // typically a marginal cable, a switch/port that doesn't pass jumbo, or an
    // oversubscribed link. ESCALATE the backoff so we stop re-opening + re-negotiating
    // bandwidth + storming GigE discovery every second (which also spams the log and
    // disturbs the healthy cameras' discovery). A camera that ran STABLE for a while
    // and then dropped is a genuine transient loss -> RESET for a fast recovery.
    {
        std::lock_guard<std::mutex> lk(reconMutex_);
        long stableSec = -1;
        auto cit = connectedAt_.find(serial);
        if (cit != connectedAt_.end()) {
            stableSec = (long)std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - cit->second).count();
            connectedAt_.erase(cit);
        }
        if (stableSec >= kStableResetSec) {
            attempts_[serial] = 0;                                  // stable then dropped -> quick reconnect
        } else {
            const int a = (attempts_[serial] = std::min(attempts_[serial] + 1, kMaxShift));
            if (a >= 3)
                LOGW() << "[supervisor][" << serial << "] FLAPPING (opens but can't grab; dropped after "
                       << (stableSec < 0 ? 0 : stableSec) << "s) -> backing off " << backoffSeconds(a)
                       << "s. Check THIS camera's cable / switch-port jumbo (or lower gige_packet_size to 1500) / GigE bandwidth.";
            // Auto-fallback: after several flaps on JUMBO, pin THIS camera to 1500-byte
            // packets for its next (re)connect. Jumbo failing (incomplete frames) is
            // almost always "jumbo not passing end-to-end on this path"; 1500 sidesteps
            // it. Per-camera, so the healthy jumbo cameras keep their big packets.
            if (a >= kJumboFallbackShift) {
                const int64_t globalPkt = SettingsManager::instance().getLpr<int>("gige_packet_size").value_or(1500);
                auto ovIt = packetOverride_.find(serial);
                const bool alreadyPinned = ovIt != packetOverride_.end() && ovIt->second > 0 && ovIt->second <= 1500;
                if (globalPkt > 1500 && !alreadyPinned) {
                    packetOverride_[serial] = 1500;
                    LOGW() << "[supervisor][" << serial << "] AUTO-FALLBACK: pinning packet size to 1500 (global jumbo "
                           << globalPkt << ") after repeated flapping -- jumbo likely not passing on this camera's path. "
                           << "It will retry at 1500 on the next reconnect.";
                }
            }
        }
    }
    startReconnect(serial);
}

void ConnectionSupervisor::startReconnect(const std::string& serial) {
    std::lock_guard<std::mutex> lk(reconMutex_);
    auto& w = workers_[serial];
    if (w.running) return;
    if (w.th.joinable()) w.th.join();    // join a finished prior worker
    w.stop = false;
    w.running = true;
    try {
        w.th = std::thread([this, serial] {
            try {
                reconnectLoop(serial);
            }
            catch (const Pylon::GenericException& e) {
                AppLogger::LogPylonException(e.GetDescription(), "[reconnect] " + serial);
            }
            catch (const std::exception& e) {
                AppLogger::LogException(e, "[reconnect] " + serial);
            }
            catch (...) {
                AppLogger::LogUnknownException("[reconnect] " + serial);
            }
            std::lock_guard<std::mutex> lk2(reconMutex_);
            auto it = workers_.find(serial);
            if (it != workers_.end()) it->second.running = false;
        });
    }
    catch (const std::system_error& e) {
        // Thread creation can fail under resource exhaustion. Mark not-running
        // so a later fault can retry, rather than believing a worker exists.
        w.running = false;
        AppLogger::LogException(e, "[reconnect] thread create -- " + serial);
    }
}

void ConnectionSupervisor::reconnectLoop(std::string serial) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(reconMutex_);
            auto it = workers_.find(serial);
            if (it == workers_.end() || it->second.stop) return;
        }
        // Already back? (e.g. raced with another path)
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            auto it = ctx_.devices.find(serial);
            if (it != ctx_.devices.end() && it->second) return;
        }

        int attempt;
        { std::lock_guard<std::mutex> lk(reconMutex_); attempt = attempts_[serial]; }
        const int wait = backoffSeconds(attempt);
        LOGI() << "[supervisor][reconnect][" << serial << "] attempt #" << (attempt + 1)
               << " (backoff " << wait << "s)";

        // Cooperative sleep.
        for (int slept = 0; slept < wait * 10; ++slept) {
            { std::lock_guard<std::mutex> lk(reconMutex_);
              auto it = workers_.find(serial);
              if (it == workers_.end() || it->second.stop) return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (openConfigureStore(serial)) return;   // success: attempts_ reset inside
        { std::lock_guard<std::mutex> lk(reconMutex_);
          attempts_[serial] = std::min(attempts_[serial] + 1, kMaxShift); }
    }
}

void ConnectionSupervisor::clearPacketOverrides() {
    std::lock_guard<std::mutex> lk(reconMutex_);
    if (!packetOverride_.empty()) {
        LOGI() << "[supervisor] cleared " << packetOverride_.size()
               << " per-camera packet-size override(s) -- gige_packet_size changed, jumbo will be retried";
        packetOverride_.clear();
    }
}

void ConnectionSupervisor::stopAll() {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(reconMutex_);
        for (auto& [serial, w] : workers_) w.stop = true;
        for (auto& [serial, w] : workers_) if (w.th.joinable()) toJoin.push_back(std::move(w.th));
        workers_.clear();
    }
    for (auto& t : toJoin) if (t.joinable()) t.join();
}

int ConnectionSupervisor::backoffSeconds(int attempts) const {
    const int shift = std::min(attempts, kMaxShift);
    long long v = (long long)kBackoffBaseSec << shift;
    return v > kBackoffCapSec ? kBackoffCapSec : (int)v;
}
