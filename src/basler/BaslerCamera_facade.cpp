#include "BaslerCamera.h"
#include "CommandQueue.h"
#include "SettingsManager_.h"
#include "AppLogger.h"

#include <pylon/PylonIncludes.h>

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
        SettingsManager& s = SettingsManager::instance();
        cameraId_     = std::atoi(gate_.c_str());
        maxExposure_  = s.getCameraSettingByIdAndKey<int>(cameraId_, "maxExposure").value_or(15000);
        maxGain_      = s.getCameraSettingByIdAndKey<int>(cameraId_, "maxGain").value_or(25);
        minGain_      = s.getCameraSettingByIdAndKey<int>(cameraId_, "minGain").value_or(5);
        triggerOn_    = s.getCameraSettingByIdAndKey<int>(cameraId_, "trigger_mode").value_or(0);
        autoExposure_ = s.getCameraSettingByIdAndKey<int>(cameraId_, "continuous_exposure").value_or(1) != 0;
        rgbPfs_       = s.getCameraSettingByIdAndKey<std::string>(cameraId_, "CamconfigFile").value_or("");
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
    catch (const std::exception& e) { AppLogger::LogException(e, "BaslerCamera ctor settings"); }
    catch (...) { AppLogger::LogUnknownException("BaslerCamera ctor settings"); }
    monoPfs_ = "acA_cam_mono.pfs";

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
        p.x = x_; p.y = y_; p.w = w_; p.h = h_;

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
    p.x = x_; p.y = y_; p.w = w_; p.h = h_;

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
