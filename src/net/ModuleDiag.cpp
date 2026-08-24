#include "lpr/net/ModuleDiag.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace lpr::diag {
namespace {
std::mutex g_mtx;
Sink       g_sink;

// Per-CAMERA emit state (keyed by serial), so a persistently-failing camera does
// not print an ERROR every window forever and flood the log page. Policy:
//   * A state TRANSITION (a different `code` than last time — e.g. an error after
//     a "connected", or a new failure kind) emits immediately and resets backoff.
//   * The SAME code repeating backs off exponentially: 20s, 40s, 80s, 160s, then
//     capped at 300s. So a camera down for an hour logs ~15 lines, not ~180.
// A recovery (INFO "…connected") is itself a transition, so the next real failure
// logs promptly again.
struct EmitState {
    std::chrono::steady_clock::time_point last{};
    std::string code;
    int         streak = 0;
    bool        primed = false;
};
std::unordered_map<std::string, EmitState> g_state;
constexpr auto kBase = std::chrono::seconds(20);
constexpr auto kCap  = std::chrono::seconds(300);
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
        const auto now = std::chrono::steady_clock::now();
        auto& st = g_state[serial];
        if (st.primed && st.code == code) {
            // Same condition still repeating -> exponential backoff (capped).
            const int shift = st.streak < 4 ? st.streak : 4;   // 20,40,80,160,320
            auto window = kBase * (1 << shift);
            if (window > kCap) window = kCap;                  // cap at 5 min
            if (now - st.last < window) return;                // suppressed this round
            if (st.streak < 5) ++st.streak;
        } else {
            // First sighting, or a transition to a new condition (incl. recovery)
            // -> emit now and restart the backoff.
            st.streak = 0;
        }
        st.primed = true;
        st.code   = code;
        st.last   = now;
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
