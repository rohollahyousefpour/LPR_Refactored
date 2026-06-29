#include "lpr/net/HeartbeatMonitor.h"
#include "lpr/util/Uuid.h"
#include "lpr/Log.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace lpr {

using json = nlohmann::json;

namespace {
std::string fixed2(double v) {
    std::ostringstream os; os << std::fixed << std::setprecision(2) << v; return os.str();
}
} // namespace

HeartbeatMonitor::HeartbeatMonitor(IMessageTransport& transport, Config cfg)
    : transport_(transport), cfg_(std::move(cfg)) {}

HeartbeatMonitor::~HeartbeatMonitor() { stop(); }

std::string HeartbeatMonitor::buildHeartbeatMessage() const {
    json doc = {
        {"messageId", generateUuidV4()},
        {"messageType", "heartbeat"},
        {"lpr_id", cfg_.lprId},
        {"messageBody", {{"info", "I am Live"}}}
    };
    return doc.dump();
}

std::string HeartbeatMonitor::buildResourcesMessage(const ResourceStats& s) const {
    json doc = {
        {"messageId", generateUuidV4()},
        {"messageType", "resources"},
        {"messageBody", {
            {"lpr_id", cfg_.lprId},
            {"CPU_USAGE", fixed2(s.cpuPercent)},
            {"RAM_USAGE", fixed2(s.ramPercent)},
            {"Free_Space_Percentage", fixed2(s.freeDiskPercent)}
        }}
    };
    return doc.dump();
}

void HeartbeatMonitor::sendHeartbeat() {
    LOGI() << "HeartbeatMonitor: >>> heartbeat to '" << cfg_.heartbeatSubject << "'";
    transport_.publish(cfg_.heartbeatSubject, buildHeartbeatMessage());
}

void HeartbeatMonitor::sendResources() {
    ResourceStats s = provider_ ? provider_() : ResourceStats{};
    LOGI() << "HeartbeatMonitor: >>> resources to '" << cfg_.resourcesSubject << "'";
    transport_.publish(cfg_.resourcesSubject, buildResourcesMessage(s));
}

void HeartbeatMonitor::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&HeartbeatMonitor::run, this);
}

void HeartbeatMonitor::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void HeartbeatMonitor::run() {
    LOGI() << "HeartbeatMonitor: started (heartbeat=" << cfg_.intervalMs
           << "ms, resources=" << cfg_.resourcesIntervalMs << "ms)";
    long sinceHeartbeat = cfg_.intervalMs;           // fire both promptly on start
    long sinceResources = cfg_.resourcesIntervalMs;
    const long step = 50;
    while (running_) {
        if (sinceHeartbeat >= cfg_.intervalMs)          { sendHeartbeat(); sinceHeartbeat = 0; }
        if (sinceResources >= cfg_.resourcesIntervalMs) { sendResources(); sinceResources = 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        sinceHeartbeat += step;
        sinceResources += step;
    }
    LOGI() << "HeartbeatMonitor: stopped";
}

} // namespace lpr
