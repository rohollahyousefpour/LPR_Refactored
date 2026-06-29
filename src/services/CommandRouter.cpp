#include "lpr/services/CommandRouter.h"
#include "lpr/Log.h"
#include "lpr/JsonFind.h"

#include <algorithm>

namespace lpr {

CommandRouter::CommandRouter(RecordingService& recording, LiveViewService& live)
    : recording_(recording), live_(live) {}

std::string CommandRouter::cameraIdOf(const json& msg) {
    // Accept camera_id (snake) or cameraId (camel), at any nesting.
    const json* id = findAnyKeyDeep(msg, {"camera_id", "cameraId", "gate_id", "gate"});
    if (!id) { LOGW() << "CommandRouter: missing camera id in command"; return {}; }
    try {
        if (id->is_string()) return id->get<std::string>();
        if (id->is_number_integer()) return std::to_string(id->get<long long>());
        if (id->is_number())  return std::to_string(id->get<int>());
    } catch (const std::exception& e) {
        LOGW() << "CommandRouter: bad camera id: " << e.what();
    }
    return {};
}

int CommandRouter::durationOf(const json& msg, int defValue, int lo, int hi) {
    int d = defValue;
    const json* dur = findKeyDeep(msg, "duration");
    if (dur) {
        try {
            if      (dur->is_number()) d = dur->get<int>();
            else if (dur->is_string()) d = std::stoi(dur->get<std::string>());
        } catch (const std::exception&) { /* keep default */ }
    }
    return std::min(std::max(d, lo), hi);
}

void CommandRouter::liveView(const json& msg) {
    const std::string id = cameraIdOf(msg);
    if (id.empty()) return;
    const int duration = durationOf(msg, /*def*/2, /*lo*/2, /*hi*/1200);   // original clamp
    live_.enableLive(id, duration);
}

void CommandRouter::startRecording(const json& msg) {
    const std::string id = cameraIdOf(msg);
    if (id.empty()) return;
    const int duration = durationOf(msg, /*def*/0, /*lo*/0, /*hi*/86400);
    recording_.startRecording(id, duration);
}

void CommandRouter::stopRecording(const std::string& cameraId) {
    recording_.stopRecording(cameraId);
}

void CommandRouter::getSetting(const json& msg) {
    if (getSetting_) getSetting_(msg);
    else LOGW() << "CommandRouter: getSetting received but no handler set";
}

void CommandRouter::setCameraConfig(const json& msg) {
    if (setConfig_) setConfig_(msg);
    else LOGW() << "CommandRouter: setCameraConfig received but no handler set";
}

} // namespace lpr
