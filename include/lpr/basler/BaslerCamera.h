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

    // Report the true current hardware AOI (Width/Height/Offset) + sensor ranges for a sub-camera
    // by serial, for the manual-control Pylon-Viewer-style AOI sliders.
    bool readAppliedAoi(const std::string& serial, Aoi& out) override;

    // Report read-only device health for a sub-camera by serial (manual-control diagnostics tiles).
    bool readAppliedDiag(const std::string& serial, Diag& out) override;

    // Take a one-shot exported preset (.pfs text) a sub-camera stashed after "Export Preset".
    bool readAppliedPreset(const std::string& serial, std::string& out) override;

    // Fetch the latest grabbed frame for a sub-camera by serial (for the manual-control live view).
    bool latestFrame(const std::string& serial, cv::Mat& out) override;

    // Re-read the camera's hardware settings and, if any changed since the last apply,
    // reconnect ONLY the affected cameras so exposure/GigE/AOI/trigger apply cleanly through
    // the tested connect path (a ~1-2s per-camera blip; the rest of the pipeline keeps running).
    void reapplySettings() override;

private:
    // Read the per-camera settings (exposure/trigger/control/config-files) into the members.
    // Shared by the constructor and reapplySettings(); may throw (callers guard).
    void readSettings();
    // A stable string of every hardware-relevant setting (per-camera exposure/trigger/control +
    // LPR-level GigE/AOI), read fresh. reapplySettings() reconnects only when this changes.
    std::string hwSnapshot() const;

    // Build a ConnectionSupervisor::Profile for a serial from settings.
    ConnectionSupervisor::Profile buildProfile(const std::string& serial,
                                               bool mono, bool master);

    // Fill p.cropXn/Yn/Wn/Hn (normalized hardware-AOI crop) for a role: the mono plate
    // camera of a pair is always cropped (ROI-polygon bbox, or a manual rect); the colour
    // camera crops only when rgb_crop_enable is on. No crop => leaves them 0 (full frame).
    void computeAoiCrop(ConnectionSupervisor::Profile& p, bool mono) const;

    // Test seam: the AOI emulation test drives computeAoiCrop() directly (reads the
    // per-camera crop settings without standing up the whole capture pipeline).
    friend struct BaslerAoiTestAccess;

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
    std::string monoSerial_;   // serial of the mono master (set via set_mono_adress), for role-preserving reconnect
    std::string lastHwSnapshot_;   // hwSnapshot() at last apply; reapplySettings() no-ops when unchanged
    int lastPacketSize_ = -1;      // last-applied global gige_packet_size; a change clears per-camera fallbacks

    // Stereo rectification (carried over from the old BaslerCamera).
    bool enableStereo_ = false;
    std::shared_ptr<AutoRoadStereoRectifier> stereoRectifier_;
};
