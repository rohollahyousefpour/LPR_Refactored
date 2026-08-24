#include "lpr/capture/VideoFileCaptureSource.h"
#include "lpr/Log.h"
#include "lpr/util/Time.h"
#include "lpr/net/ModuleDiag.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

namespace lpr {

void VideoFileCaptureSource::setAddress(const std::string& address, int delayMs) {
    address_ = address;
    delayMs_ = delayMs;   // open is deferred to run() so we never block the manager thread
    // A live stream (rtsp/http/…) never "ends"; a read failure is a DISCONNECT to
    // recover from, not a clean stop. A local file legitimately ends.
    isLiveUrl_ = address_.find("://") != std::string::npos
                 && address_.compare(0, 7, "file://") != 0;
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
        LOGE() << "VideoFileCaptureSource: failed to OPEN '" << address_
               << "' - unreachable stream or missing/unsupported codec";
        lpr::diag::cameraFault(cameraId_, address_, "ERROR", "stream_open_failed",
            isLiveUrl_ ? "باز کردنِ استریمِ دوربینِ IP/RTSP ناموفق شد — دوربین در دسترس نیست یا آدرس/کدک نادرست است"
                       : "باز کردنِ فایلِ ویدیو ناموفق شد — کدکِ پشتیبانی‌نشده یا مسیرِ نادرست");
        emitError();   // -> worker reconnects with backoff
        return;
    }
    if (isLiveUrl_) {
        lpr::diag::cameraFault(cameraId_, address_, "INFO", "stream_connected",
            "استریمِ دوربینِ IP/RTSP برقرار شد");
    }
    running_ = true;
    // Rolling fps: count frames over a ~2s window and publish the effective rate.
    auto winStart = std::chrono::steady_clock::now();
    int  winFrames = 0;
    while (running_) {
        cv::Mat image;
        if (!cap_.read(image) || image.empty()) {
            if (loop_) { cap_.set(cv::CAP_PROP_POS_FRAMES, 0); continue; }
            if (isLiveUrl_) {
                // A live stream never ends cleanly -> this is a DISCONNECT. Fault +
                // emitError so the worker recycles + reconnects (previously it fell
                // through as "clean end" and the camera stopped for good).
                ++readFailures_;
                measuredFps_ = -1.0;
                LOGW() << "VideoFileCaptureSource: stream lost '" << address_ << "' -> reconnect";
                lpr::diag::cameraFault(cameraId_, address_, "ERROR", "stream_lost",
                    "ارتباط با استریمِ دوربینِ IP/RTSP قطع شد — تلاش برای اتصالِ مجدد");
                emitError();
                break;
            }
            LOGI() << "VideoFileCaptureSource: end of stream '" << address_ << "'";
            break;                        // real file EOF -> clean end -> worker stops
        }
        // fps accounting
        if (++winFrames >= 15) {
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - winStart).count();
            if (secs > 0) measuredFps_ = winFrames / secs;
            winStart = now; winFrames = 0;
        }
        // No artificial inter-frame sleep: the frame sink now applies BACKPRESSURE for
        // video (CameraManager wires pushBlocking), so the reader is paced by the
        // detector and every frame is processed. A fixed delay here would only add
        // latency and, with a slow detector, starve/drop nothing but waste wall-clock.
        // (delayMs_ is retained for API compat but intentionally not used for video.)
        emitFrame(image, cv::Mat(), nowEpochSeconds());
    }
    running_ = false;
    measuredFps_ = -1.0;
    cap_.release();
}

void VideoFileCaptureSource::stop() { running_ = false; }
bool VideoFileCaptureSource::isLive() const { return running_; }

bool VideoFileCaptureSource::readAppliedDiag(const std::string& /*serial*/, Diag& out) {
    if (!running_) return false;   // nothing to report until the stream is up
    out.fps = measuredFps_.load();                       // effective read rate
    out.incompleteFrames = readFailures_.load();         // mid-stream drops this process
    out.model = isLiveUrl_ ? "IP / RTSP" : "Video";      // shown as the device "model" tile
    return true;
}

} // namespace lpr
