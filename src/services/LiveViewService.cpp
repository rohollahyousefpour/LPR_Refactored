#include "lpr/services/LiveViewService.h"
#include "lpr/Log.h"

#include <algorithm>

namespace lpr {

LiveViewService::LiveViewService(Config cfg) : cfg_(cfg) {
    if (cfg_.sendEveryN < 1) cfg_.sendEveryN = 1;
}

void LiveViewService::enableLive(const std::string& gate, int durationSeconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    Live& l = live_[gate];
    l.hasExpiry = durationSeconds > 0;
    if (l.hasExpiry)
        l.expireAt = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
    LOGI() << "LiveViewService[" << gate << "]: live for " << durationSeconds << "s";
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
