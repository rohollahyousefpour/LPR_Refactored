#pragma once
//
// ConnectionSupervisor
// --------------------
// Owns connecting and reconnecting cameras. For each registered camera profile
// it can (re)open the device, load its feature file, install its exposure
// strategy and sync role, apply bandwidth via the coordinator, and store it in
// the context. On a fault it removes the device (on the capture thread) and
// starts a per-serial backoff worker that retries until it succeeds or is told
// to stop.
//
// This is the relocation of BaslerDiscovery.cpp + BaslerReconnect.cpp +
// addCameraToMap/tryConnectOne, unified into one "openConfigureStore" path.
//
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

#include "CameraContext.h"
#include "IExposureStrategy.h"


class ConnectionSupervisor : public IExposureController {
public:
    struct Profile {
        std::string serial;
        bool        triggerOn    = false;  // off -> config-2, on -> config-1
        bool        freeRun      = false;  // true -> FreeRunConfigurator (trigger off, no strobe)

        // Exposure role:
        //   cameraNativeAuto      -> CameraAutoExposureStrategy (on-sensor, "pylon")
        //   fixedExposure = true  -> FixedExposureStrategy (mono plate cam + IR)
        //   else autoExposure     -> AutoExposureStrategy  (host adaptive, capped)
        //   else                  -> ManualExposureStrategy (host-driven)
        bool        cameraNativeAuto = false;
        bool        fixedExposure  = false;
        double      fixedExposureUs = 200.0;  // TUNE: low, motion-freezing for IR plate
        double      fixedGain       = 0.0;    // TUNE
        bool        autoExposure    = true;

        std::string pfsFile;               // optional Basler feature file
        AutoExposureStrategy::Limits exposureLimits;  // per-camera bounds + ROI
        int x = 0, y = 0, w = 0, h = 0;    // ROI; 0 size => full frame
    };

    ConnectionSupervisor(CameraContext& ctx);
    ~ConnectionSupervisor();

    void addProfile(const Profile& p);     // register intent for a serial
    bool connect(const std::string& serial); // synchronous open+configure+store

    // IExposureController: manual override + restore configured settings. Called
    // on the capture thread (from the command drain).
    void setManualExposure(const std::string& serial, double value, ExposureUnit unit) override;
    void setManualGain(const std::string& serial, double value, ExposureUnit unit) override;
    void revertToSettings(const std::string& serial) override;

    // Read the exposure (microseconds) + gain the named camera actually holds now,
    // so the manual-control UI can report ground truth. False if not connected.
    bool readExposureGain(const std::string& serial, double& exposureUs, double& gain);

    // Capture-thread entry: remove the faulted device, then start a reconnect
    // worker. Removal here (not in the worker) preserves the pointer-safety
    // invariant in CameraContext.
    void handleFault(const std::string& serial);

    // Drop every per-camera packet-size override (jumbo auto-fallbacks). Called when the
    // operator changes the global gige_packet_size so the new value takes effect on the
    // next reconnect instead of a flapped camera staying pinned to its 1500 fallback.
    void clearPacketOverrides();

    void stopAll();                        // stop + join every reconnect worker

private:
    bool openConfigureStore(const std::string& serial);   // the unified path
    void applyBandwidth(CameraDevice& dev, const Profile& p);
    void applyRoi(CameraDevice& dev, const Profile& p);
    // Build (and for fixed/camera-native, configure) the exposure strategy for a
    // profile. Shared by the connect path and revertToSettings.
    std::unique_ptr<IExposureStrategy> makeExposureStrategy(const Profile& prof,
                                                            CameraDevice& dev);
    void startReconnect(const std::string& serial);
    void reconnectLoop(std::string serial);
    int  backoffSeconds(int attempts) const;
    // Re-apply a live manual-control override (exposure/gain) after a reconnect, so a
    // camera blip mid-tuning doesn't silently drop back to auto. No-op if the serial
    // has no active manual override.

    CameraContext& ctx_;

    std::mutex                          profMutex_;
    std::map<std::string, Profile>      profiles_;
    std::map<std::string, std::string>  baseline_;   // serial -> load-time node snapshot (pfs string)

    // Active manual-control overrides, so they survive a reconnect. Cleared by
    // revertToSettings. exp/gain hold the last requested value + its unit.
    struct ManualOverride {
        bool         hasExp = false;  double exp = 0;   ExposureUnit expUnit = ExposureUnit::Auto;
        bool         hasGain = false; double gain = 0;  ExposureUnit gainUnit = ExposureUnit::Auto;
    };
    std::mutex                              manualMutex_;
    std::map<std::string, ManualOverride>   manual_;

    struct Worker {
        std::thread       th;
        std::atomic<bool> stop{false};
        bool              running = false;
    };
    std::mutex                          reconMutex_;
    std::map<std::string, Worker>       workers_;
    std::map<std::string, int>          attempts_;   // backoff state per serial
    // (re)connect timestamp per serial. Used to tell a FLAPPING camera (opens fine
    // but can't grab -> faults within seconds) from a genuine transient drop of a
    // camera that had been running stably. The former escalates the backoff; the
    // latter resets it. Guarded by reconMutex_.
    std::map<std::string, std::chrono::steady_clock::time_point> connectedAt_;
    // Per-camera packet-size override (bytes). Set by handleFault when a camera keeps
    // FLAPPING on jumbo, so its next (re)connect uses standard 1500-byte packets while
    // the healthy jumbo cameras are untouched. Read by applyBandwidth. Guarded by
    // reconMutex_. Absent/0 => use the global gige_packet_size setting.
    std::map<std::string, int64_t> packetOverride_;

    static constexpr int kBackoffBaseSec = 1;
    static constexpr int kBackoffCapSec  = 180;
    static constexpr int kMaxShift       = 12;
    // A camera must stay connected at least this long for a later drop to count as
    // "transient" (reset backoff). Drop sooner => flapping => escalate the backoff.
    static constexpr int kStableResetSec = 30;
    // After this many escalating flaps on JUMBO, auto-fall the camera back to 1500-byte
    // packets (jumbo usually fails because it isn't passing end-to-end on that path).
    static constexpr int kJumboFallbackShift = 3;
};
