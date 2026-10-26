#include "lpr/net/LogForwarder.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <nlohmann/json.hpp>

namespace lpr::logfwd {
namespace {
std::mutex                            g_mtx;
Sink                                  g_sink;        // guarded by g_mtx
std::string                           g_clientId;    // guarded by g_mtx
std::chrono::steady_clock::time_point g_expiry{};    // guarded by g_mtx
std::atomic<bool>                     g_active{false};
std::atomic<std::uint64_t>            g_seq{0};

// Prevents recursion: if publishing a line itself logs (e.g. the transport warns
// on a failed publish), that log must NOT be forwarded again from inside here.
thread_local bool t_inForward = false;

const char* levelName(lpr::Level l) {
    switch (l) {
        case lpr::Level::Trace:   return "TRACE";
        case lpr::Level::Debug:   return "DEBUG";
        case lpr::Level::Info:    return "INFO";
        case lpr::Level::Warning: return "WARN";
        case lpr::Level::Error:   return "ERROR";
        case lpr::Level::Fatal:   return "FATAL";
    }
    return "INFO";
}
} // namespace

void configure(Sink sink, std::string clientId) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sink     = std::move(sink);
    g_clientId = std::move(clientId);
}

void start(int durationSeconds) {
    if (durationSeconds <= 0) durationSeconds = 45;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    }
    g_active.store(true, std::memory_order_relaxed);
}

void stop() {
    g_active.store(false, std::memory_order_relaxed);
}

bool active() {
    return g_active.load(std::memory_order_relaxed);
}

void onLogLine(lpr::Level level, const std::string& isoTime, const std::string& message) {
    if (t_inForward) return;                                 // don't forward our own publish logs
    if (!g_active.load(std::memory_order_relaxed)) return;   // streaming off — cheap exit

    // Never forward the transport's own publish/enqueue/dequeue chatter: each such
    // line is produced *by* forwarding a line, and its background SENDER thread
    // logs it (so the thread_local guard above can't catch it) — forwarding it
    // back creates a self-amplifying feedback loop that saturates the send queue.
    // Filtering by source breaks the loop and also keeps the live view readable.
    if (message.rfind("NatsTransport:", 0) == 0) return;

    Sink        sink;
    std::string clientId;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        // Safety auto-stop: the keep-alive window lapsed without a refresh.
        if (std::chrono::steady_clock::now() >= g_expiry) {
            g_active.store(false, std::memory_order_relaxed);
            return;
        }
        if (!g_sink || g_clientId.empty()) return;
        sink     = g_sink;
        clientId = g_clientId;
    }

    t_inForward = true;
    try {
        const nlohmann::json j = {
            {"messageType", "module_log"},
            {"lpr_id",      clientId},
            {"messageBody", {
                {"seq",     g_seq.fetch_add(1, std::memory_order_relaxed)},
                {"ts",      isoTime},
                {"level",   levelName(level)},
                {"tag",     ""},
                {"message", message},
            }},
        };
        sink("socketio.module_log", j.dump());
    } catch (...) {
        // Never let a forwarding error disturb the logging hot path.
    }
    t_inForward = false;
}

} // namespace lpr::logfwd
