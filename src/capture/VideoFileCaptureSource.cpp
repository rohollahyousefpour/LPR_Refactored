#include "lpr/capture/VideoFileCaptureSource.h"
#include "lpr/Log.h"
#include "lpr/util/Time.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

namespace lpr {

void VideoFileCaptureSource::setAddress(const std::string& address, int delayMs) {
    address_ = address;
    delayMs_ = delayMs;   // open is deferred to run() so we never block the manager thread
}

void VideoFileCaptureSource::run() {
    // Distinguish a missing file (config/path error) from a codec/open failure (build missing ffmpeg).
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool looksLikeFile = address_.find("://") == std::string::npos;  // not rtsp/http/etc.
    if (looksLikeFile && !fs::exists(address_, ec)) {
        LOGE() << "VideoFileCaptureSource: file does not exist '" << address_
               << "' (check the CameraAddress path in settings)";
        emitError();
        return;
    }

    // Try the default backend, then explicitly FFmpeg (most reliable for .avi/.mp4 if compiled in).
    bool opened = cap_.open(address_);
    if (!opened) opened = cap_.open(address_, cv::CAP_FFMPEG);
    if (!opened) {
        LOGE() << "VideoFileCaptureSource: failed to OPEN existing file '" << address_
               << "' - likely a missing/unsupported codec (build OpenCV with the ffmpeg feature)";
        emitError();
        return;
    }
    running_ = true;
    while (running_) {
        cv::Mat image;
        if (!cap_.read(image) || image.empty()) {
            if (loop_) { cap_.set(cv::CAP_PROP_POS_FRAMES, 0); continue; }
            LOGI() << "VideoFileCaptureSource: end of stream '" << address_ << "'";
            break;                        // clean end -> no emitError -> worker stops (play once)
        }
        emitFrame(image, cv::Mat(), nowEpochSeconds());
        if (delayMs_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));
    }
    running_ = false;
    cap_.release();
}

void VideoFileCaptureSource::stop() { running_ = false; }
bool VideoFileCaptureSource::isLive() const { return running_; }

} // namespace lpr
