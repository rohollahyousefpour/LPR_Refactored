#include "lpr/net/ModuleDiag.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace lpr::diag {
namespace {
std::mutex                                                                g_mtx;
Sink                                                                      g_sink;
// (serial|code) -> last emit time, so a per-frame failure loop is throttled to
// one log line per window instead of thousands.
std::unordered_map<std::string, std::chrono::steady_clock::time_point>    g_last;
constexpr auto kThrottle = std::chrono::seconds(20);
} // namespace

void setSink(Sink s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_sink = std::move(s);
}

void cameraFault(const std::string& cameraId,
                 const std::string& serial,
                 const char* level,
                 const std::string& code,
                 const std::string& reason) {
    Sink sink;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_sink) return;   // forwarding disabled / not yet wired
        const std::string key = serial + "|" + code;
        const auto now = std::chrono::steady_clock::now();
        auto it = g_last.find(key);
        if (it != g_last.end() && now - it->second < kThrottle) return;  // throttled
        g_last[key] = now;
        sink = g_sink;   // copy the callable, then publish outside the lock
    }

    nlohmann::json j = {
        {"kind",      "camera"},
        {"event",     code},
        {"level",     level ? level : "WARNING"},
        {"camera_id", cameraId},
        {"serial",    serial},
        {"message",   reason},
    };
    try { sink(j.dump()); } catch (...) { /* best-effort */ }
}

} // namespace lpr::diag
