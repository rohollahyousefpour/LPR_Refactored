#pragma once
// DualCaptureSource - composes two CaptureSources (main + mono) into one source that emits
// paired frames: emitFrame(main, mono, timestamp). Each sub-stream runs with its own reconnect
// loop and a liveness timestamp. If ONE stream drops, the survivor keeps the source alive and is
// emitted alone (its peer Mat arrives empty), so CameraWorker degrades to using that one camera
// for both detection and evidence. The whole source only fails (-> worker reconnect) when BOTH
// streams are down. Works for any source type (video/rtsp/gstreamer pairs).
#include "lpr/capture/CaptureSource.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <opencv2/core.hpp>

namespace lpr {

class DualCaptureSource : public CaptureSource {
public:
    DualCaptureSource(std::unique_ptr<CaptureSource> main, std::unique_ptr<CaptureSource> mono);
    ~DualCaptureSource() override;

    void setAddress(const std::string& address, int delayMs) override;
    void setMonoAddress(const std::string& address, int delayMs) override;

    void run()  override;     // blocks (drives main + supervises both) until stop()
    void stop() override;
    bool isLive() const override;

    void handleCommand(const std::string& key, const std::string& value) override;

private:
    void runMonoLoop();
    void emitPair(bool triggerIsMain, long ts);
    void interruptibleSleep(int ms);
    bool fresh(std::chrono::steady_clock::time_point t) const;

    std::unique_ptr<CaptureSource> main_;
    std::unique_ptr<CaptureSource> mono_;

    std::thread        monoThread_;
    std::atomic<bool>  running_{false};

    mutable std::mutex mtx_;             // guards the latest frames + liveness timestamps
    cv::Mat            latestMain_, latestMono_;
    std::chrono::steady_clock::time_point lastMain_{}, lastMono_{};
    bool               haveMain_ = false, haveMono_ = false;

    std::mutex              emitMtx_;    // serializes emitFrame so onFrame never re-enters concurrently
    std::mutex              stopMtx_;
    std::condition_variable stopCv_;

    static constexpr int kStaleMs = 3000;   // a stream is "live" if it produced within this window
};

} // namespace lpr
