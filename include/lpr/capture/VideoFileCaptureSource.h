#pragma once
// VideoFileCaptureSource (was video_checking) - cv::VideoCapture file/URL reader.
#include "lpr/capture/CaptureSource.h"
#include <opencv2/videoio.hpp>
#include <atomic>
#include <string>

namespace lpr {

class VideoFileCaptureSource : public CaptureSource {
public:
    void setAddress(const std::string& address, int delayMs) override;
    void run()  override;
    void stop() override;
    bool isLive() const override;

    // Report effective throughput (fps) + read-failure count so an IP/RTSP camera
    // appears in the health dashboard's Grab test, not just Basler cameras.
    bool readAppliedDiag(const std::string& serial, Diag& out) override;

    void setLoop(bool loop) { loop_ = loop; }   // replay a finished file forever

private:
    std::string        address_;
    int                delayMs_ = 0;
    cv::VideoCapture   cap_;
    std::atomic<bool>  running_{false};
    bool               loop_ = false;
    bool               isLiveUrl_ = false;      // rtsp://, http://, … (vs a local file)
    std::atomic<double> measuredFps_{-1.0};     // rolling effective read rate
    std::atomic<long>  readFailures_{0};        // mid-stream read/decoder failures this process
};

} // namespace lpr
