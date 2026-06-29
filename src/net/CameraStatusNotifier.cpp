#include "lpr/net/CameraStatusNotifier.h"
#include "lpr/util/Uuid.h"
#include "lpr/Log.h"

#include <nlohmann/json.hpp>

namespace lpr {

using json = nlohmann::json;

CameraStatusNotifier::CameraStatusNotifier(IMessageTransport& transport, Config cfg)
    : transport_(transport), cfg_(std::move(cfg)) {}

std::string CameraStatusNotifier::buildMessage(const std::string& cameraId, bool connected) {
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "camera_connection"},
        {"messageBody", {
            {"camera_id", cameraId},
            {"Connection", connected}
        }}
    };
    return document.dump();
}

void CameraStatusNotifier::notify(const std::string& cameraId, bool connected) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cfg_.onlyOnChange) {
            auto it = last_.find(cameraId);
            if (it != last_.end() && it->second == connected) return;   // no change -> skip
            last_[cameraId] = connected;
        }
    }
    LOGI() << "CameraStatusNotifier: >>> camera " << cameraId
           << (connected ? " connected" : " DISCONNECTED") << " to '" << cfg_.subject << "'";
    transport_.publish(cfg_.subject, buildMessage(cameraId, connected));
}

std::function<void(const std::string&, bool)> CameraStatusNotifier::asCallback() {
    return [this](const std::string& gate, bool connected) { this->notify(gate, connected); };
}

} // namespace lpr
