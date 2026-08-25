#pragma once
// CommandRouter - the clean port of the command-handling surface of Managment_Cameras
// (send_live_view, start_recording, stop_recording, get_setting, set_camera_config) that
// the original wired straight into Handle_Clients/NATS signals. Here the routing is
// decoupled from the transport: the NATS/TCP layer simply connects its signals to these
// methods (exactly as Handle_Clients did), and this class translates the JSON envelope
// into calls on the services. That makes command handling testable without any network.
//
//   nats.live_signal      -> router.liveView(msg)
//   nats.streaming_signal -> router.startRecording(msg)
//   nats.Lpr_Settting     -> router.getSetting(msg)
//   nats.Camera_Config    -> router.setCameraConfig(msg)
//
// get_setting / set_camera_config in the original do heavy application bootstrap
// (load settings, create cameras, start the pipeline). That is Application-level work, not
// a service, so the router forwards them to injected handlers the Application provides.
#include "lpr/services/RecordingService.h"
#include "lpr/services/LiveViewService.h"

#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace lpr {

class CommandRouter {
public:
    using json = nlohmann::json;
    using SettingHandler = std::function<void(const json&)>;

    CommandRouter(RecordingService& recording, LiveViewService& live);

    void liveView(const json& msg);          // enable live view for a camera (clamped duration; 0 = until stopped)
    void stopLiveView(const std::string& cameraId);   // stop live view (last viewer left)
    void startRecording(const json& msg);    // begin recording a camera for a duration
    void stopRecording(const std::string& cameraId);
    void getSetting(const json& msg);         // -> injected handler (app bootstrap)
    void setCameraConfig(const json& msg);    // -> injected handler

    void setGetSettingHandler(SettingHandler h)     { getSetting_ = std::move(h); }
    void setSetCameraConfigHandler(SettingHandler h) { setConfig_ = std::move(h); }

    // Parse {messageBody:{data:{cameraId, duration}}} the way the original messages do.
    static std::string cameraIdOf(const json& msg);
    static int durationOf(const json& msg, int defValue, int lo, int hi);

private:
    RecordingService& recording_;
    LiveViewService&  live_;
    SettingHandler    getSetting_;
    SettingHandler    setConfig_;
};

} // namespace lpr
