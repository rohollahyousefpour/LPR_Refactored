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
        applyBandwidth(*device);

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

        // 4) Store everything under the map lock (insertion is the only thing
        //    that happens off the capture thread).
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            ctx_.exposure[serial] = std::move(exp);
            ctx_.syncRole[serial] = std::move(role);
            ctx_.devices[serial]  = std::move(device);
        }

        // 5) Reset backoff for this serial.
        { std::lock_guard<std::mutex> lk(reconMutex_); attempts_[serial] = 0; }

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

void ConnectionSupervisor::setManualExposure(const std::string& serial, double norm) {
    // Copy the motion-blur cap from the profile first (own lock), then act on the
    // device (own lock) -- never nest, to match openConfigureStore's lock order.
    double cap = 50000.0;
    { std::lock_guard<std::mutex> pk(profMutex_);
      auto pit = profiles_.find(serial);
      if (pit != profiles_.end() && pit->second.exposureLimits.maxExposureUs > 0)
          cap = double(pit->second.exposureLimits.maxExposureUs); }

    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto dit = ctx_.devices.find(serial);
    if (dit == ctx_.devices.end() || !dit->second) {
        LOGW() << "[exposure][" << serial << "] manual exposure: camera not connected";
        return;
    }
    // value <= 1.0 => normalized fraction of the range; value > 1.0 => absolute
    // microseconds (the backend sends real exposure times, which are large).
    if (norm > 1.0) ManualExposureStrategy::setExposureUs(*dit->second, norm, cap);
    else            ManualExposureStrategy::setNormalizedExposure(*dit->second, norm, cap);
    ctx_.exposure[serial] = std::make_unique<ManualExposureStrategy>();  // suspend auto loop
    LOGI() << "[exposure][" << serial << "] MANUAL exposure "
           << (norm > 1.0 ? "abs=" : "norm=") << norm
           << " (cap=" << cap << "us, auto suspended)";
}

void ConnectionSupervisor::setManualGain(const std::string& serial, double norm) {
    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto dit = ctx_.devices.find(serial);
    if (dit == ctx_.devices.end() || !dit->second) {
        LOGW() << "[exposure][" << serial << "] manual gain: camera not connected";
        return;
    }
    if (norm > 1.0) ManualExposureStrategy::setGainAbs(*dit->second, norm);
    else            ManualExposureStrategy::setNormalizedGain(*dit->second, norm);
    ctx_.exposure[serial] = std::make_unique<ManualExposureStrategy>();  // suspend auto loop
    LOGI() << "[exposure][" << serial << "] MANUAL gain "
           << (norm > 1.0 ? "abs=" : "norm=") << norm << " (auto suspended)";
}

void ConnectionSupervisor::revertToSettings(const std::string& serial) {
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

void ConnectionSupervisor::applyRoi(CameraDevice& dev, const Profile& p) {
    if (p.w <= 0 || p.h <= 0) return;   // full frame
    auto* c = dev.raw(); if (!c) return;
    // Option A (default): do NOT crop the sensor. Capture the full "crude" frame so it can
    // be sent whole (full_image + the crud_image used for drawing the ROI polygon), and let
    // detection crop to the ROI in software (RoiCropRecognizer), exactly like the IP cameras.
    // Set basler_sensor_aoi=1 to restore the hardware AOI (smaller frames / less GigE
    // bandwidth, but then no full-scene image is available).
    const bool hwAoi = SettingsManager::instance().getLpr<int>("basler_sensor_aoi").value_or(0) != 0;
    if (!hwAoi) {
        LOGI() << "[supervisor][roi][" << dev.serial()
               << "] full-frame mode (no sensor AOI); ROI applied in software for detection";
        return;
    }
    try {
        auto align = [](int v, int mn, int inc) { return inc > 0 ? mn + ((v - mn) / inc) * inc : v; };
        if (c->Width.IsWritable())
            c->Width.SetValue(align(p.w, (int)c->Width.GetMin(), (int)c->Width.GetInc()));
        if (c->Height.IsWritable())
            c->Height.SetValue(align(p.h, (int)c->Height.GetMin(), (int)c->Height.GetInc()));
        if (c->OffsetX.IsWritable())
            c->OffsetX.SetValue(align(p.x, (int)c->OffsetX.GetMin(), (int)c->OffsetX.GetInc()));
        if (c->OffsetY.IsWritable())
            c->OffsetY.SetValue(align(p.y, (int)c->OffsetY.GetMin(), (int)c->OffsetY.GetInc()));
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "[supervisor] roi -- " + dev.serial());
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[supervisor] roi -- " + dev.serial());
    }
    catch (...) {
        AppLogger::LogUnknownException("[supervisor] roi -- " + dev.serial());
    }
}

void ConnectionSupervisor::applyBandwidth(CameraDevice& dev) {
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
        const int idx = std::clamp(coord.registerCamera(serial, nic), 0, num_cam - 1);

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
        const int64_t wantPkt = SettingsManager::instance().getLpr<int>("gige_packet_size").value_or(1500);
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
            double fps = 0.0;
            if      (c->AcquisitionFrameRate.IsReadable())    fps = c->AcquisitionFrameRate.GetValue();
            else if (c->AcquisitionFrameRateAbs.IsReadable()) fps = c->AcquisitionFrameRateAbs.GetValue();
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
