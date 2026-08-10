#pragma once
//
// BaslerCamera  (Facade)
// ----------------------
// The public face the rest of the system already talks to (virtual_cap_url).
// It OWNS the collaborators and the shared CameraContext, and delegates --
// it no longer implements discovery, capture, exposure, bandwidth, sync, or
// reconnect itself. One instance manages one camera or one RGB+Mono pair.
//
#include <memory>
#include <string>

#include "virtual_cap_url.h"
#include "sunset.h"

#include "CameraContext.h"
#include "CapturePipeline.h"
#include "ConnectionSupervisor.h"
#include "IFrameSink.h"
#include "StereoRectifySink.h"

class AutoRoadStereoRectifier;

class BaslerCamera : public virtual_cap_url {
public:
    BaslerCamera(int x, int y, int w, int h, std::string gate);
    ~BaslerCamera();   // base virtual_cap_url has a non-virtual dtor -> no 'override'

    // virtual_cap_url interface (signatures match the base exactly)
    void set_adress(std::string serial, int delay) override;
    void set_mono_adress(std::string serial, int delay) override;
    void run() override;
    bool is_live() override;
    void stop_vlc() override;
    // Keep the inherited string->json bridge (virtual_cap_url) visible alongside the
    // json override below, so the CaptureSource string-command path resolves cleanly.
    using virtual_cap_url::handleCommand;
    void handleCommand(const std::string& key, const nlohmann::json& value) override;

    // Report the true applied exposure/gain for a sub-camera (mono or RGB) by serial.
    bool readAppliedExposureGain(const std::string& serial, double& exposureUs, double& gain) override;

private:
    // Build a ConnectionSupervisor::Profile for a serial from settings.
    ConnectionSupervisor::Profile buildProfile(const std::string& serial,
                                               bool mono, bool master);

    CameraContext ctx_;

    std::unique_ptr<ConnectionSupervisor> supervisor_;
    std::unique_ptr<CapturePipeline>      pipeline_;
    std::unique_ptr<IFrameSink>           sink_;   // adapts to legacy send_frame

    // Geometry / config carried from construction.
    int x_, y_, w_, h_;
    std::string gate_;
    int  cameraId_ = 0;
    int  triggerOn_ = 0;
    int  maxExposure_ = 15000;
    int  minGain_ = 5, maxGain_ = 25;
    bool autoExposure_ = true;
    std::string monoPfs_, rgbPfs_;

    // Mono PLATE camera = SHORT FIXED exposure + LOW gain under strobed IR (ambient-light
    // rejection). Published LPR/IR guidance: 1/500-1/1000s freezes slow/parking traffic and the
    // retroreflective plate stays bright via IR. Overridable via settings mono_exposure_us /
    // mono_gain. Keep gain low: high gain adds noise that hurts OCR; raise IR power instead.
    int    monoExposureUs_ = 1000;   // 1/1000s: rejects ambient, freezes motion
    double monoGain_       = 5.0;    // low/clean; IR does the lighting
    // RGB / single camera adaptive target brightness (0..255); the loop adapts up/down from here.
    double rgbTarget_      = 100.0;
    // Continuous brightness-loop tuning (host control mode). Exposed via settings so the loop
    // can be tuned on-site without a rebuild. Percentile 85 meters the bright (IR-lit, retro-
    // reflective) plate rather than the whole scene, so a sunny day can't wash the plate out and
    // night/cloudy stay optimized -- one loop, no day/night clock.
    double expPercentile_  = 85.0;
    double expDeadband_    = 10.0;
    double expDamping_     = 0.6;
    double expStepRatio_   = 2.0;
    double expEma_         = 0.5;
    int    expIntervalMs_  = 1000;
    bool   expHighlight_   = false;   // expose for the brightest region (plate); keep gain low
    // Camera controller mode: "pylon" => on-sensor continuous auto + free-run (no triggers);
    // "host" => the host strategies (fixed mono + IR strobe/sync + host auto RGB).
    std::string controlMode_ = "pylon";

    bool   hasMono_ = false;   // true once a mono master is registered (=> pair)

    // Stereo rectification (carried over from the old BaslerCamera).
    bool enableStereo_ = false;
    std::shared_ptr<AutoRoadStereoRectifier> stereoRectifier_;
};
