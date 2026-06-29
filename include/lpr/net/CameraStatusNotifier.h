#pragma once
// CameraStatusNotifier - the clean port of Handle_Clients::send_camera_connection, wired to
// the camera layer's status callback. When a camera connects or (the part you asked for)
// DISCONNECTS, it publishes a "camera_connection" message to the backend via the transport.
//
// CameraWorker/CameraManager already emit status as std::function<void(gate, bool connected)>
// (edge-triggered on connect/disconnect). asCallback() returns exactly that type, so wiring is:
//
//   cameraManager.setStatusCallback(notifier.asCallback());
#include "lpr/net/IMessageTransport.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lpr {

class CameraStatusNotifier {
public:
    struct Config {
        std::string subject     = "socketio.camera_connection";
        bool        onlyOnChange = true;   // suppress repeats of the same state for a camera
    };

    CameraStatusNotifier(IMessageTransport& transport, Config cfg);
    CameraStatusNotifier(IMessageTransport& transport) : CameraStatusNotifier(transport, Config{}) {}

    // Publish the connection state for a camera (deduped if onlyOnChange).
    void notify(const std::string& cameraId, bool connected);

    // Adapter for CameraManager::setStatusCallback.
    std::function<void(const std::string&, bool)> asCallback();

    // Pure builder for the camera_connection document. Public for testing.
    static std::string buildMessage(const std::string& cameraId, bool connected);

private:
    IMessageTransport& transport_;
    Config             cfg_;
    std::mutex                            mtx_;
    std::unordered_map<std::string, bool> last_;   // cameraId -> last published state
};

} // namespace lpr
