#pragma once
// LiveViewService - the clean port of Managment_Cameras::{send_live_view, stop_streaming}
// and the LIVE half of handle_live_and_recording. It tracks which gates are "live" (each
// with an expiry), and on each frame for a live gate forwards the frame to a sink. Like
// RecordingService it is a *frame observer*.
//
// Improvements over the original:
//   * expiry handled on the frame path (steady_clock) instead of a detached sleeper thread
//     per live request
//   * the destination is an injected sink (replaces handel_client->send_live_async), so this
//     class doesn't depend on the network layer
//   * configurable frame-skip (original hard-coded "every other frame")
#include <opencv2/core.hpp>

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lpr {

class LiveViewService {
public:
    struct Config {
        int sendEveryN = 2;   // forward 1 of every N frames (original sent every other frame)
    };

    using LiveSink = std::function<void(const std::string& gate, const cv::Mat& img)>;

    explicit LiveViewService(Config cfg);
    LiveViewService() : LiveViewService(Config{}) {}

    void enableLive(const std::string& gate, int durationSeconds);   // 0 = until disabled
    void disableLive(const std::string& gate);
    bool isLive(const std::string& gate) const;
    std::size_t liveCount() const;

    void onFrame(const std::string& gate, const cv::Mat& img);       // frame observer
    void setLiveSink(LiveSink cb) { sink_ = std::move(cb); }

private:
    struct Live {
        std::chrono::steady_clock::time_point expireAt;
        bool hasExpiry = false;
        long counter = 0;
    };

    Config            cfg_;
    LiveSink          sink_;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, Live> live_;
};

} // namespace lpr
