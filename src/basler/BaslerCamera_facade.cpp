#include "BaslerCamera.h"
#include "CommandQueue.h"
#include "SettingsManager_.h"
#include "AppLogger.h"

#include <pylon/PylonIncludes.h>

#include <climits>
#include <mutex>
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------------
// LegacySendSink: adapts the new IFrameSink to the old virtual_cap_url output.
//
// NOTE: in virtual_cap_url, send_frame and send_cap are boost::signals2 signals,
// NOT methods. Invoking the signal (operator()) emits to all connected slots.
// The frame signal is void(const cv::Mat&, const cv::Mat&, long) -- the
// timestamp is a 32-bit `long`, so we cast time_t down explicitly at this one
// boundary rather than truncating silently.
// ---------------------------------------------------------------------------
namespace {
class LegacySendSink : public IFrameSink {
public:
    explicit LegacySendSink(virtual_cap_url* owner) : owner_(owner) {}

    void onFramePair(const cv::Mat& left, const cv::Mat& right, std::time_t ts) override {
        try { owner_->send_frame(left, right, static_cast<long>(ts)); }   // emit signal
        catch (...) { AppLogger::LogUnknownException("[sink] send_frame"); }
    }
    void onIncompletePair(const cv::Mat& available, std::time_t ts) override {
        // Deliver what we have duplicated; the consumer can detect staleness.
        try { owner_->send_frame(available, available, static_cast<long>(ts)); }
        catch (...) { AppLogger::LogUnknownException("[sink] send_frame (incomplete)"); }
    }
    void onCaptureError(int code) override {
        try { owner_->send_cap(code); }   // emit signal
        catch (...) { AppLogger::LogUnknownException("[sink] send_cap"); }
    }
private:
    virtual_cap_url* owner_;
};
} // namespace

// ---------------------------------------------------------------------------

BaslerCamera::BaslerCamera(int x, int y, int w, int h, std::string gate)
    : x_(x), y_(y), w_(w), h_(h), gate_(std::move(gate))
{
    Pylon::PylonInitialize();

    // Settings reads can throw (missing config / bad type). Fall back to the
    // member defaults rather than failing construction.
    try {
        readSettings();
        // Baseline snapshot: a later settings save only reconnects a camera if a
        // hardware-relevant value has actually changed since this point.
        lastHwSnapshot_ = hwSnapshot();
        lastPacketSize_ = SettingsManager::instance().getLpr<int>("gige_packet_size").value_or(1500);
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "BaslerCamera ctor settings"); }
    catch (...) { AppLogger::LogUnknownException("BaslerCamera ctor settings"); }

    // Output chain: capture -> [StereoRectifySink] -> LegacySendSink -> signals.
    auto legacy = std::make_unique<LegacySendSink>(this);
    if (enableStereo_ && stereoRectifier_) {
        sink_ = std::make_unique<StereoRectifySink>(std::move(legacy),
                                                    enableStereo_, stereoRectifier_);
    } else {
        sink_ = std::move(legacy);
    }
    ctx_.sink   = sink_.get();

    supervisor_ = std::make_unique<ConnectionSupervisor>(ctx_);
    ctx_.exposureController = supervisor_.get();   // manual override + revert routing
    pipeline_   = std::make_unique<CapturePipeline>(
                    ctx_, [this](const std::string& serial) { supervisor_->handleFault(serial); });

    LOGI() << "[BaslerCamera] constructed gate=" << gate_;
}

BaslerCamera::~BaslerCamera() {
    // A destructor must never throw. stop_vlc() is already guarded, but wrap
    // the whole teardown defensively (PylonTerminate included).
    try {
        stop_vlc();
    } catch (...) {}
    try {
        Pylon::PylonTerminate();
    } catch (...) {}
}

// Read the per-camera hardware settings into the members. Extracted from the ctor so
// reapplySettings() can refresh them before rebuilding a profile. May throw (callers guard).
void BaslerCamera::readSettings() {
    SettingsManager& s = SettingsManager::instance();
    cameraId_     = std::atoi(gate_.c_str());
    maxExposure_  = s.getCameraSettingByIdAndKey<int>(cameraId_, "maxExposure").value_or(15000);
    maxGain_      = s.getCameraSettingByIdAndKey<int>(cameraId_, "maxGain").value_or(25);
    minGain_      = s.getCameraSettingByIdAndKey<int>(cameraId_, "minGain").value_or(5);
    triggerOn_    = s.getCameraSettingByIdAndKey<int>(cameraId_, "trigger_mode").value_or(0);
    autoExposure_ = s.getCameraSettingByIdAndKey<int>(cameraId_, "continuous_exposure").value_or(1) != 0;
    rgbPfs_       = s.getCameraSettingByIdAndKey<std::string>(cameraId_, "CamconfigFile").value_or("");
    // Mono/IR plate camera feature file. Settings-driven and OPTIONAL: default empty
    // means "no pfs" (configure() skips the load, exactly like the RGB path). This
    // avoids aborting the mono master on a missing file, which would otherwise take
    // down its whole stereo pair.
    monoPfs_      = s.getCameraSettingByIdAndKey<std::string>(cameraId_, "MonoCamconfigFile").value_or("");
    // Auto-exposure target brightness (0..255) for the single/RGB camera. LOWER it for
    // retroreflective plates that wash out: a high target meters the (darker) whole scene
    // and over-exposes the bright plate. ~40-70 exposes for the plate, letting the
    // background go dark (fine for OCR). Default preserves prior behaviour (100).
    rgbTarget_    = (double)s.getCameraSettingByIdAndKey<int>(cameraId_, "exposure_target").value_or((int)rgbTarget_);
    // Continuous brightness-loop tuning (host mode). Defaults preserve prior behaviour.
    expPercentile_ = (double)s.getCameraSettingByIdAndKey<int>(cameraId_, "exposure_percentile").value_or((int)expPercentile_);
    expDeadband_   = (double)s.getCameraSettingByIdAndKey<int>(cameraId_, "exposure_deadband").value_or((int)expDeadband_);
    expDamping_    = (double)s.getCameraSettingByIdAndKey<float>(cameraId_, "exposure_damping").value_or((float)expDamping_);
    expStepRatio_  = (double)s.getCameraSettingByIdAndKey<float>(cameraId_, "exposure_step_ratio").value_or((float)expStepRatio_);
    expEma_        = (double)s.getCameraSettingByIdAndKey<float>(cameraId_, "exposure_ema").value_or((float)expEma_);
    expIntervalMs_ = s.getCameraSettingByIdAndKey<int>(cameraId_, "exposure_interval_ms").value_or(expIntervalMs_);
    expHighlight_  = s.getCameraSettingByIdAndKey<int>(cameraId_, "exposure_highlight_priority").value_or(expHighlight_ ? 1 : 0) != 0;
    // Mono/IR plate camera: short FIXED exposure (ambient rejection) + low gain; the strobed
    // IR lights the retroreflective plate. Optional settings override the research defaults.
    monoExposureUs_ = s.getCameraSettingByIdAndKey<int>(cameraId_, "mono_exposure_us").value_or(monoExposureUs_);
    monoGain_       = s.getCameraSettingByIdAndKey<int>(cameraId_, "mono_gain").value_or(int(monoGain_));
    // Controller mode: "pylon" (default) = on-sensor continuous auto + free-run (no triggers);
    // "host" = host strategies (fixed mono + IR strobe sync + host auto RGB).
    controlMode_  = s.getCameraSettingByIdAndKey<std::string>(cameraId_, "camera_control").value_or("pylon");
}

// Fingerprint of every setting that only takes effect at connect: the per-camera exposure/
// trigger/control block PLUS the LPR-level GigE budget + hardware-AOI keys that applyBandwidth()
// re-reads on connect. reapplySettings() reconnects only when this string changes, so tuning that
// applies live (motion gate, gige_adapt_*) never triggers a needless reconnect.
std::string BaslerCamera::hwSnapshot() const {
    SettingsManager& s = SettingsManager::instance();
    std::ostringstream o;
    const int id = cameraId_;
    auto ci = [&](const char* k){ o << k << '=' << s.getCameraSettingByIdAndKey<int>(id, k).value_or(INT_MIN) << ';'; };
    auto cf = [&](const char* k){ o << k << '=' << s.getCameraSettingByIdAndKey<float>(id, k).value_or(-1e30f) << ';'; };
    auto cs = [&](const char* k){ o << k << '=' << s.getCameraSettingByIdAndKey<std::string>(id, k).value_or("") << ';'; };
    auto li = [&](const char* k){ o << k << '=' << s.getLpr<int>(k).value_or(INT_MIN) << ';'; };
    ci("maxExposure"); ci("maxGain"); ci("minGain"); ci("trigger_mode"); ci("continuous_exposure");
    cs("CamconfigFile"); cs("MonoCamconfigFile");
    ci("exposure_target"); ci("exposure_percentile"); ci("exposure_deadband");
    cf("exposure_damping"); cf("exposure_step_ratio"); cf("exposure_ema");
    ci("exposure_interval_ms"); ci("exposure_highlight_priority");
    ci("mono_exposure_us"); ci("mono_gain"); cs("camera_control");
    // LPR-level, re-read by ConnectionSupervisor::applyBandwidth() / the AOI gate at connect.
    li("basler_sensor_aoi"); li("gige_packet_size"); li("gige_link_mbytes_per_sec");
    li("gige_scpd_floor"); li("gige_fps_safety_pct"); li("gige_frame_rate_cap"); li("gige_bw_reserve_pct");
    // Per-camera hardware AOI crop (re-read by computeAoiCrop() at connect). Changing any
    // of these -- or the ROI polygon that roi-mode crops to -- must re-crop, so include the
    // keys AND the polygon bounding box so reapplySettings() reconnects the camera.
    cs("mono_crop_mode"); cf("mono_crop_x"); cf("mono_crop_y"); cf("mono_crop_w"); cf("mono_crop_h");
    ci("rgb_crop_enable"); cs("rgb_crop_mode"); cf("rgb_crop_x"); cf("rgb_crop_y"); cf("rgb_crop_w"); cf("rgb_crop_h");
    // Config-mode (temporarily un-crop for setup) + safety margin: both change the applied crop,
    // so reconnect when they change (config-mode toggling is exactly how the full-sensor snapshot works).
    ci("aoi_config_mode"); ci("aoi_crop_margin");
    {
        const auto pts = s.getCameraPoints(id);   // normalized; bbox feeds roi-mode crop
        float mnx = 2, mny = 2, mxx = -1, mxy = -1;
        for (const auto& pt : pts) { mnx = std::min(mnx, pt.x); mxx = std::max(mxx, pt.x);
                                     mny = std::min(mny, pt.y); mxy = std::max(mxy, pt.y); }
        o << "roi_bbox=" << mnx << ',' << mny << ',' << mxx << ',' << mxy << ';';
    }
    return o.str();
}

// Live re-apply of hardware settings via TARGETED RECONNECT. On a settings save the
// CameraWorker calls this; if any connect-time setting changed we refresh the members,
// rebuild each connected camera's profile from the new values, and fault it so the
// supervisor reconnects it through the same tested path used at startup -- which re-applies
// exposure (new profile), GigE budget + AOI (applyBandwidth re-reads), and trigger/control.
// One camera blips (~1-2s) at a time; the pipeline and the other cameras keep running.
void BaslerCamera::reapplySettings() {
    try {
        const std::string snap = hwSnapshot();
        if (snap == lastHwSnapshot_) return;   // nothing that needs a reconnect changed
        lastHwSnapshot_ = snap;
        readSettings();                         // refresh members so buildProfile() uses new values

        // If the operator changed the global packet size, drop any per-camera jumbo
        // auto-fallbacks so the NEW value applies uniformly on the reconnect below
        // (otherwise a previously-flapped camera would stay pinned to its 1500 fallback).
        const int curPkt = SettingsManager::instance().getLpr<int>("gige_packet_size").value_or(1500);
        if (curPkt != lastPacketSize_) {
            lastPacketSize_ = curPkt;
            if (supervisor_) supervisor_->clearPacketOverrides();
        }

        std::vector<std::string> serials;
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            serials.reserve(ctx_.devices.size());
            for (auto& [s, dev] : ctx_.devices) if (dev) serials.push_back(s);
        }
        for (const auto& serial : serials) {
            const bool mono = (!monoSerial_.empty() && serial == monoSerial_);
            supervisor_->addProfile(buildProfile(serial, mono, /*master*/mono));   // overwrite stored profile
            supervisor_->handleFault(serial);                                       // drop + async reconnect
        }
        LOGI() << "[BaslerCamera] hardware settings changed -> reconnecting "
               << serials.size() << " camera(s) to apply exposure/GigE/AOI/trigger LIVE";
    }
    catch (const std::exception& e) { AppLogger::LogException(e, "BaslerCamera::reapplySettings"); }
    catch (...) { AppLogger::LogUnknownException("BaslerCamera::reapplySettings"); }
}

void BaslerCamera::computeAoiCrop(ConnectionSupervisor::Profile& p, bool mono) const {
    p.cropXn = p.cropYn = p.cropWn = p.cropHn = 0.0;   // default: full frame
    // The crop math lives in SettingsManager::aoiCropNorm, shared with detectionCropNorm so the
    // detection ROI/mask is crop-aware and not re-applied on the already-cropped sensor frame.
    if (auto r = SettingsManager::instance().aoiCropNorm(cameraId_, mono)) {
        p.cropXn = r->x; p.cropYn = r->y; p.cropWn = r->width; p.cropHn = r->height;
        LOGI() << "[BaslerCamera][aoi] cam=" << cameraId_ << (mono ? " mono" : " rgb")
               << " crop norm(" << r->x << "," << r->y << "," << r->width << "," << r->height << ")";
    }
}

ConnectionSupervisor::Profile
BaslerCamera::buildProfile(const std::string& serial, bool mono, bool /*master*/) {
    ConnectionSupervisor::Profile p;
    p.serial       = serial;

    // ---- "pylon" mode (default): triggers OFF + on-sensor continuous auto ----
    // Each camera free-runs and exposes itself via ExposureAuto/GainAuto=Continuous,
    // exactly like pylon Viewer's defaults, bounded by a motion-blur exposure cap and
    // an OCR-safe gain ceiling. No master/slave, no IR strobe line. Applies to both
    // the mono and the RGB of a pair.
    if (controlMode_ != "host") {
        p.freeRun          = true;
        p.triggerOn        = false;
        p.cameraNativeAuto = true;
        p.pfsFile          = mono ? monoPfs_ : rgbPfs_;
        computeAoiCrop(p, mono);   // hardware AOI crop (mono always / colour if enabled)

        AutoExposureStrategy::Limits lim;
        lim.maxExposureUs = maxExposure_;   // motion-blur cap (settings)
        lim.target        = rgbTarget_;     // target brightness (0..255)
        lim.minGain       = minGain_;
        lim.maxGain       = maxGain_;
        p.exposureLimits  = lim;
        return p;
    }

    // ---- "host" mode: original strategies (fixed mono + IR sync + host auto RGB) ----
    //   mono in a pair      -> master, free-run (config-2)
    //   RGB in a pair       -> slave, triggered (config-1)   [hasMono_]
    //   single RGB, tm == 1 -> slave, triggered (config-1)
    //   single RGB, tm == 0 -> free-run         (config-2)
    // trigger_mode only decides the SINGLE-camera case; a pair RGB is always a
    // slave regardless of trigger_mode.
    if (mono) {
        p.triggerOn = false;                       // mono master always free-runs
    } else {
        p.triggerOn = (hasMono_ || triggerOn_ == 1); // pair RGB OR single tm==1 => slave
    }
    p.pfsFile      = mono ? monoPfs_ : rgbPfs_;
    computeAoiCrop(p, mono);   // hardware AOI crop (mono always / colour if enabled)

    if (mono) {
        // Mono = PLATE camera under strobed IR (the master's ExposureActive pulse drives the IR).
        // Best practice (Bosch "LPR scene mode" / ambient-light rejection): a SHORT, FIXED
        // exposure so daylight/headlights don't matter and the IR lights the retroreflective
        // plate, with LOW gain (let the illumination do the work; high gain ruins OCR). Fixed
        // (not adaptive) keeps motion frozen and avoids gain hunting on empty/dark frames.
        //   ~1000us (1/1000s) freezes slow/parking traffic and rejects ambient; raise toward
        //   2000us (1/500s) only if plates read too dim and the IR can't be increased.
        p.fixedExposure   = true;
        p.fixedExposureUs = double(monoExposureUs_);
        p.fixedGain       = monoGain_;
        p.autoExposure    = false;
    } else {
        // RGB / single camera = adaptive auto-exposure driven purely by measured brightness,
        // with a motion-blur cap. No day/night clock: when the scene darkens the controller
        // raises exposure to the cap then gain; when it brightens it lowers gain then exposure.
        p.fixedExposure   = false;
        p.autoExposure    = autoExposure_;

        AutoExposureStrategy::Limits lim;
        lim.maxExposureUs = maxExposure_;     // motion-blur cap (from settings)
        lim.target        = rgbTarget_;       // mid brightness; adapts up/down from here
        lim.minGain       = minGain_;
        lim.maxGain       = maxGain_;
        // Meter the bright plate, not the scene mean: this is what keeps a sunny day from washing
        // the plate out and keeps night/cloudy optimized. lim.roi left empty (plates may be
        // anywhere); raise exposure_percentile if a very bright background still over-exposes.
        lim.percentile    = expPercentile_;   // default 85 (vs 50=median)
        lim.deadband      = expDeadband_;
        lim.damping       = expDamping_;
        lim.maxStepRatio  = expStepRatio_;
        lim.emaAlpha      = expEma_;
        lim.minInterval   = std::chrono::milliseconds(expIntervalMs_);
        lim.highlightPriority = expHighlight_;
        p.exposureLimits = lim;
    }
    return p;
}

void BaslerCamera::set_adress(std::string serial, int delay) {
    try {
        ctx_.delay = delay;
        // A lone color camera (or the slave in a pair). Not mono, not master here;
        // master/mono is registered via set_mono_adress.
        supervisor_->addProfile(buildProfile(serial, /*mono*/false, /*master*/false));
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            ctx_.expectedCount = std::max<int>(ctx_.expectedCount, (int)ctx_.devices.size() + 1);
        }
        if (!supervisor_->connect(serial))
            supervisor_->handleFault(serial);   // schedule async retry
    }
    catch (const Pylon::GenericException& e) { AppLogger::LogPylonException(e.GetDescription(), "set_adress -- " + serial); }
    catch (const std::exception& e) { AppLogger::LogException(e, "set_adress -- " + serial); }
    catch (...) { AppLogger::LogUnknownException("set_adress -- " + serial); }
}

void BaslerCamera::set_mono_adress(std::string serial, int delay) {
    try {
        ctx_.delay = delay;
        ctx_.expectedCount = 2;   // mono present => this instance is a pair
        hasMono_ = true;          // any RGB in this instance is now a slave
        monoSerial_ = serial;     // remember the master's role for a role-preserving reconnect

        // Mono is the master: trigger off, config-2 (Exposure Active out on Line2).
        supervisor_->addProfile(buildProfile(serial, /*mono*/true, /*master*/true));
        if (!supervisor_->connect(serial))
            supervisor_->handleFault(serial);

        // If an RGB was already registered as free-run (set_adress called first),
        // re-register it as a slave now that we know this is a pair, and
        // reconnect it so config-1 is applied.
        std::vector<std::string> rgbSerials;
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            for (auto& [s, dev] : ctx_.devices)
                if (s != serial && dev) rgbSerials.push_back(s);  // anything not the mono
        }
        for (auto& s : rgbSerials) {
            LOGI() << "[BaslerCamera] re-slaving RGB " << s << " (mono added after it)";
            supervisor_->addProfile(buildProfile(s, /*mono*/false, /*master*/false));
            supervisor_->handleFault(s);   // drop + reconnect -> config-1 applied
        }
    }
    catch (const Pylon::GenericException& e) { AppLogger::LogPylonException(e.GetDescription(), "set_mono_adress -- " + serial); }
    catch (const std::exception& e) { AppLogger::LogException(e, "set_mono_adress -- " + serial); }
    catch (...) { AppLogger::LogUnknownException("set_mono_adress -- " + serial); }
}

void BaslerCamera::run() {
    try {
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            if (ctx_.devices.empty()) {
                LOGE() << "[BaslerCamera] run() with no cameras";
                if (ctx_.sink) ctx_.sink->onCaptureError(-1);
                return;
            }
        }
        pipeline_->run();   // blocks until stop_vlc()
    }
    catch (const Pylon::GenericException& e) {
        AppLogger::LogPylonException(e.GetDescription(), "BaslerCamera::run");
        if (ctx_.sink) ctx_.sink->onCaptureError(-1);
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "BaslerCamera::run");
        if (ctx_.sink) ctx_.sink->onCaptureError(-1);
    }
    catch (...) {
        AppLogger::LogUnknownException("BaslerCamera::run");
        if (ctx_.sink) ctx_.sink->onCaptureError(-1);
    }
}

bool BaslerCamera::is_live() { return ctx_.live.load(); }

void BaslerCamera::stop_vlc() {
    try {
        if (pipeline_)   pipeline_->stop();      // ctx_.live = false; loop exits
        if (supervisor_) supervisor_->stopAll(); // join reconnect workers

        std::map<std::string, std::unique_ptr<CameraDevice>> dead;
        {
            std::lock_guard<std::mutex> lk(ctx_.devMutex);
            dead.swap(ctx_.devices);
            ctx_.exposure.clear();
            ctx_.syncRole.clear();
        }
        dead.clear();   // CameraDevice dtors release every device
        ctx_.commands.clear();
    }
    catch (const Pylon::GenericException& e) { AppLogger::LogPylonException(e.GetDescription(), "BaslerCamera::stop_vlc"); }
    catch (const std::exception& e) { AppLogger::LogException(e, "BaslerCamera::stop_vlc"); }
    catch (...) { AppLogger::LogUnknownException("BaslerCamera::stop_vlc"); }
}

void BaslerCamera::handleCommand(const std::string& key, const json& value) {
    // Parsing (makeCommand) can throw on a malformed JSON payload -- never let
    // a bad command from the wire crash the caller's thread.
    try {
        if (auto cmd = makeCommand(key, value))
            ctx_.commands.push(std::move(cmd));
    }
    catch (const json::exception& e) { LOGW() << "[handleCommand] bad payload for '" << key << "': " << e.what(); }
    catch (const std::exception& e) { AppLogger::LogException(e, "handleCommand -- " + key); }
    catch (...) { AppLogger::LogUnknownException("handleCommand -- " + key); }
}

bool BaslerCamera::readAppliedExposureGain(const std::string& serial, double& exposureUs, double& gain) {
    return supervisor_ && supervisor_->readExposureGain(serial, exposureUs, gain);
}

bool BaslerCamera::readAppliedAoi(const std::string& serial, Aoi& out) {
    return supervisor_ && supervisor_->readAoi(serial, out);
}

bool BaslerCamera::latestFrame(const std::string& serial, cv::Mat& out) {
    std::lock_guard<std::mutex> lk(ctx_.devMutex);
    auto it = ctx_.lastFrames.find(serial);
    if (it == ctx_.lastFrames.end() || it->second.empty()) return false;
    out = it->second.clone();   // caller owns it; decouple from the live cache
    return true;
}
