#include "lpr/services/LiveViewService.h"
#include "lpr/Log.h"

#include <algorithm>

namespace lpr {

LiveViewService::LiveViewService(Config cfg) : cfg_(cfg) {
    if (cfg_.sendEveryN < 1) cfg_.sendEveryN = 1;
}

void LiveViewService::enableLive(const std::string& gate, int durationSeconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Multiple operators can watch the SAME camera at once — the backend fans one
    // physical stream out to every subscriber. Each subscribe re-issues the
    // streaming command with THAT viewer's duration, so we must keep the LONGEST
    // deadline (a union of viewers), never blindly overwrite it. Otherwise a
    // second viewer asking for a shorter time would cut the first viewer's stream
    // off early. `durationSeconds <= 0` means "until disabled" (unbounded), which
    // always wins over any finite request.
    const auto newExpiry =
        std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    auto it = live_.find(gate);
    if (it == live_.end()) {
        Live l;
        l.hasExpiry = durationSeconds > 0;
        if (l.hasExpiry) l.expireAt = newExpiry;
        live_.emplace(gate, l);
        LOGI() << "LiveViewService[" << gate << "]: live for " << durationSeconds << "s";
        return;
    }
    Live& l = it->second;
    if (durationSeconds <= 0) {
        l.hasExpiry = false;                       // unbounded wins
    } else if (l.hasExpiry && newExpiry > l.expireAt) {
        l.expireAt = newExpiry;                     // extend to the later deadline
    }
    // else: existing deadline is already later, or already unbounded → keep it.
    LOGI() << "LiveViewService[" << gate << "]: live +"
           << durationSeconds << "s (keep-longest, "
           << (l.hasExpiry ? "bounded" : "until-disabled") << ")";
}

void LiveViewService::disableLive(const std::string& gate) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (live_.erase(gate)) LOGI() << "LiveViewService[" << gate << "]: live off";
}

bool LiveViewService::isLive(const std::string& gate) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = live_.find(gate);
    if (it == live_.end()) return false;
    if (it->second.hasExpiry && std::chrono::steady_clock::now() >= it->second.expireAt) return false;
    return true;
}

std::size_t LiveViewService::liveCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return live_.size();
}

void LiveViewService::onFrame(const std::string& gate, const cv::Mat& img) {
    if (img.empty()) return;
    LiveSink sink;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = live_.find(gate);
        if (it == live_.end()) return;
        Live& l = it->second;
        if (l.hasExpiry && std::chrono::steady_clock::now() >= l.expireAt) {
            live_.erase(it);                              // expired -> stop streaming
            LOGI() << "LiveViewService[" << gate << "]: live expired";
            return;
        }
        if (++l.counter % cfg_.sendEveryN != 0) return;   // frame-skip
        sink = sink_;                                     // copy under lock, call outside
    }
    if (sink) sink(gate, img);
}

} // namespace lpr
