#pragma once
// HeartbeatMonitor - the clean port of Monitor_HeartRate + Handle_Clients::send_heartbeat /
// send_resource_data. A background thread that alternates two liveness messages to the
// backend through IMessageTransport:
//   heartbeat  -> "socketio.heartbeat"  {messageType:"heartbeat", lpr_id, messageBody:{info}}
//   resources  -> "socketio.resources"  {messageType:"resources", messageBody:{lpr_id, CPU/RAM/disk}}
//
// CPU/RAM/disk gathering is platform-specific, so it is an injected provider (default: zeros),
// keeping this class cross-platform and testable.
#include "lpr/net/IMessageTransport.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace lpr {

struct ResourceStats {
    double cpuPercent  = 0.0;
    double ramPercent  = 0.0;
    double freeDiskPercent = 0.0;
};

class HeartbeatMonitor {
public:
    using ResourceProvider = std::function<ResourceStats()>;

    struct Config {
        std::string lprId;                                   // client identity (lpr_id)
        std::string heartbeatSubject = "socketio.heartbeat";
        std::string resourcesSubject = "socketio.resources";
        long        resourcesIntervalMs = 60000;   // resources cadence (original: every 1 minute)
        int         intervalMs       = 30000;                // between sends (alternating)
    };

    HeartbeatMonitor(IMessageTransport& transport, Config cfg);
    HeartbeatMonitor(IMessageTransport& transport) : HeartbeatMonitor(transport, Config{}) {}
    ~HeartbeatMonitor();

    void setResourceProvider(ResourceProvider p) { provider_ = std::move(p); }

    void start();
    void stop();

    // Pure builders, exposed for testing.
    std::string buildHeartbeatMessage() const;
    std::string buildResourcesMessage(const ResourceStats& s) const;

    void sendHeartbeat();
    void sendResources();

private:
    void run();

    IMessageTransport& transport_;
    Config             cfg_;
    ResourceProvider   provider_;
    std::thread        thread_;
    std::atomic<bool>  running_{false};
};

} // namespace lpr
