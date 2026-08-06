#pragma once
// Application - the top-level wiring that replaces the constructor/run glue of
// Managment_Cameras. It implements the bootstrap flow:
//
//   connect (+token) -> subscribe to settings -> [backend pushes settings] ->
//   SettingsManager::loadAll -> load models (backend per settings) -> build the recognizer
//   chain (the four plate_detection variants) -> build cameras by link type ->
//   start DetectionWorker -> PlateProcessor -> PlateSender; with RecordingService +
//   LiveViewService as frame observers feeding MediaSender, CommandRouter on the command
//   subjects, and CameraStatusNotifier on camera connect/disconnect.
//
// Offline mode (no broker): pass a settings JSON directly via bootstrapFromJson() for
// local runs/tests.
#include "lpr/net/IMessageTransport.h"
#include "lpr/manager/CameraManager.h"
#include "lpr/manager/DetectionWorker.h"
#include "lpr/manager/LiveOverlay.h"
#include "lpr/capture/FrameQueue.h"
#include "lpr/detect/PlateRecognizer.h"
#include "lpr/detect/VehicleDetector.h"
#include "lpr/detect/RoiCropRecognizer.h"
#include "lpr/track/VehicleAwarePlateRecognizer.h"
#include "lpr/track/PlateTrackingRecognizer.h"
#include "lpr/process/PlateProcessor.h"
#include "lpr/services/RecordingService.h"
#include "lpr/services/LiveViewService.h"
#include "lpr/services/CommandRouter.h"
#include "lpr/net/PlateSender.h"
#include "lpr/net/MediaSender.h"
#include "lpr/net/CameraStatusNotifier.h"
#include "lpr/net/HeartbeatMonitor.h"
#include "lpr/config/AppConfig.h"
#ifdef LPR_WITH_PYLON
#include "lpr/basler/BaslerFactory.h"
#endif

#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <chrono>

namespace lpr {

class Application {
public:
    struct Options {
        // Transport
        bool        useNats       = false;                 // false -> InMemoryTransport (offline)
        std::string natsUrl       = "nats://127.0.0.1:4222";
        std::string natsToken;                             // bootstrap auth token (env AUTH_TOKEN)
        std::string natsUser;                              // env NATS_USER
        std::string natsPassword;                          // env NATS_PASSWORD
        std::string natsCertFile;                          // env NATS_CERT_FILE (mutual TLS)
        std::string natsKeyFile;                           // env NATS_KEY_FILE
        std::string natsCaFile;                            // env NATS_CA_FILE
        bool        natsTlsFirst = false;                  // env NATS_TLS_FIRST (handshake_first)
        std::string natsExpectedHostname = "ALPR";         // env NATS_TLS_HOSTNAME (cert CN/SAN)
        // Real protocol subjects (from the original NatsClient):
        std::string authenticateSubject  = "authenticate";          // publish {token}, reply response.token.<token>
        std::string settingsRequestSubject = "alpr.settings.request"; // reply alpr.settings.response.<clientId>
        // Local config file (the original's Config): provides MODEL paths + geometry.
        std::string configPath;                            // env LPR_CONFIG / --config
        // Settings keys for model paths (fallback when no config file is given)
        std::string keyBackend       = "model_type";   // ALPR "type of model": openvino|tensorrt|onnx|opencv|hailo
        std::string defaultBackend   = "openvino";      // used when the setting is absent
        std::string keyDevice        = "device";        // "CPU"|"GPU"|"CUDA"...
        std::string keyPlateDetector = "plate_detector_model";
        std::string keyOcrModel      = "ocr_model";
        std::string keyCarModel      = "car_model";
        std::string keyOcrAlphabet   = "ocr_alphabet";
    };

    explicit Application(Options opts);
    ~Application();

    bool start(std::atomic<bool>& running);         // connect (retrying), then handshake
    void bootstrapFromJson(const nlohmann::json& settingsBody);  // offline / direct bootstrap
    void run();                                     // block until stop()
    void stop();

    // Restart hook. The backend sends a `reset_lpr` command on `command.<clientId>`
    // (see NatsHandle::publish_command in the Rust backend); onCommand acknowledges it
    // and then invokes this handler. main() wires it to a graceful stop + self re-exec,
    // so the whole reader process restarts on demand — the C++ side of the
    // "restart the LPR" API (backend/front/C++). The handler must only flip flags and
    // return quickly (it runs on the NATS callback thread); the real re-launch happens
    // on the main thread after the command ack has been published.
    using RestartHandler = std::function<void()>;
    void setRestartHandler(RestartHandler h) { onRestart_ = std::move(h); }

    IMessageTransport* transport() { return transport_.get(); }

private:
    void  onSettings(const std::string& payload);   // settings-response handler -> bootstrap
    // NATS request/reply handshake (matches the original NatsClient):
    void  authenticate();                            // publish {token} -> response.token.<token>
    void  onAuthReply(const std::string& payload);   // parse client_id, then subscribe+request
    void  subscribeToCommands();                     // command.<clientId> -> CommandRouter
    void  onCommand(const std::string& payload);
    void  requestSettings();                         // alpr.settings.request -> response.<clientId>
    void  publishManualLive(const FrameItem& f);     // one "live_manual_control" msg (both sensors)
    std::unique_ptr<IPlateRecognizer> buildRecognizerChain();   // the four variants + ROI wrap

    Options opts_;
    std::unique_ptr<AppConfig> config_;              // local config file (model paths), if provided
    std::string clientId_;                           // assigned by the authenticate reply (lpr_id)

    // Manual-control live: while a gate is being adjusted, publish ONE "live_manual_control"
    // message per (throttled) frame carrying every sensor + its current exposure/gain.
    struct ExpGain { double exposureUs = -1; double gain = -1; };
    struct ManualLiveState {
        std::chrono::steady_clock::time_point expireAt;
        long counter = 0;
        std::map<std::string, ExpGain> vals;         // serial -> last operator-set exposure/gain
    };
    std::mutex manualLiveMtx_;
    std::map<std::string, ManualLiveState> manualLive_;   // gate -> state

    // Latest full frame per camera (for the on-demand screenshot command).
    std::mutex                     lastFrameMtx_;
    std::map<std::string, cv::Mat> lastFrame_;             // gate -> most recent frame
    void sendScreenshot(const std::string& gate);         // capture + publish current frame

    std::unique_ptr<IMessageTransport> transport_;

    // Pipeline (built on bootstrap)
    std::shared_ptr<FrameQueue>                  frames_;
    std::unique_ptr<PlateRecognizer>             plateStage_;
    std::unique_ptr<VehicleDetector>             vehicleDet_;
    std::unique_ptr<VehicleAwarePlateRecognizer> vehicleRec_;
    std::unique_ptr<LiveOverlay>                 overlay_;   // detection boxes -> live stream
    std::unique_ptr<PlateTrackingRecognizer>     plateTrackRec_;
    std::unique_ptr<RoiCropRecognizer>           roiRec_;
    IPlateRecognizer*                            topRecognizer_ = nullptr;

    std::unique_ptr<CameraManager>        cameras_;
    std::unique_ptr<PlateProcessor>       processor_;
    std::unique_ptr<PlateSender>          sender_;
    std::unique_ptr<RecordingService>     recording_;
    std::unique_ptr<LiveViewService>      live_;
    std::unique_ptr<MediaSender>          media_;
    std::unique_ptr<CameraStatusNotifier> notifier_;
    std::unique_ptr<HeartbeatMonitor>     heartbeat_;
    std::unique_ptr<CommandRouter>        router_;
    std::unique_ptr<DetectionWorker>      worker_;   // declared last -> destroyed first

    std::mutex        bootMtx_;
    std::atomic<bool> booted_{false};
    std::atomic<bool> running_{false};
    RestartHandler    onRestart_;                    // set by main(); fired on a reset_lpr command
};

} // namespace lpr
