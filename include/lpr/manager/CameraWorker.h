#pragma once
// CameraWorker - per-camera capture + motion-gate + reconnect. Decoupled from
// messaging/detection via std::function callbacks.
#include "lpr/capture/CaptureSource.h"
#include "lpr/capture/FrameItem.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

namespace lpr {

struct MotionConfig {
    int      delayMs          = 50;
    bool     enableMotionGate = true;
    double   minDeviation     = 20.0;
    double   maxDeviation     = 0.0;       // MaxDeviation (read by original; 0 = unused)
    int      minChangedPixels = 250;       // ObjectSize: changed pixels needed for real motion
    int      reconnectBaseMs  = 1000;
    int      reconnectMaxMs   = 30000;
    cv::Rect roi;                          // motion is computed inside this rect when area()>0
    // Mono+RGB pairing: when true, detection runs on the SECONDARY (mono) stream and the
    // PRIMARY (RGB) stream becomes the evidence image. Used for IP pairs where
    // CameraAddress=RGB(evidence) and MonoCameraAddress=mono(detection). Basler leaves this
    // false (its facade already delivers the pair mono-first).
    bool     detectOnSecondary = false;
};

class CameraWorker {
public:
    using SourceFactoryFn    = std::function<std::unique_ptr<CaptureSource>()>;
    using FrameSink          = std::function<void(FrameItem&&)>;
    using StatusCallback     = std::function<void(const std::string& gate, bool connected)>;
    using FirstFrameCallback = std::function<void(const std::string& gate, const cv::Mat& fullFrame)>;

    CameraWorker(std::string gate, SourceFactoryFn factory, MotionConfig cfg = {});
    ~CameraWorker();

    CameraWorker(const CameraWorker&) = delete;
    CameraWorker& operator=(const CameraWorker&) = delete;

    void setFrameSink(FrameSink sink)             { frameSink_ = std::move(sink); }
    void setStatusCallback(StatusCallback cb)     { statusCb_  = std::move(cb); }
    // Fires for EVERY captured frame, before the motion gate (used for continuous live view,
    // matching the original's independent live thread).
    void setRawFrameObserver(FrameSink obs)       { rawObserver_ = std::move(obs); }
    void setFirstFrameCallback(FirstFrameCallback cb) { firstFrameCb_ = std::move(cb); }

    // Runtime ROI update (e.g. computed from settings on the first frame).
    void setRoi(const cv::Rect& roi);

    // Forward a runtime command to the live source (thread-safe).
    void handleCommand(const std::string& key, const std::string& value);

    // Read the exposure (us) + gain a sub-camera (by serial) actually holds now.
    bool readExposureGain(const std::string& serial, double& exposureUs, double& gain);

    void start();
    void stop();
    bool isRunning() const { return running_; }
    const std::string& gate() const { return gate_; }

private:
    void supervise();
    void onFrame(const cv::Mat& frame, const cv::Mat& mono, long timestamp);
    void onError();
    int  countChangedPixels(const cv::Mat& motion) const;
    void emitStatus(bool connected);

    std::string     gate_;
    SourceFactoryFn factory_;
    MotionConfig    cfg_;

    FrameSink          frameSink_;
    FrameSink          rawObserver_;
    StatusCallback     statusCb_;
    FirstFrameCallback firstFrameCb_;

    std::unique_ptr<CaptureSource> source_;
    std::mutex                     sourceMtx_;     // guards source_ for handleCommand
    std::thread       supervisor_;
    std::thread       sourceThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> errored_{false};
    std::atomic<bool> sourceFinished_{false};
    std::atomic<int>  framesThisSession_{0};
    std::mutex              waitMtx_;
    std::condition_variable waitCv_;

    // motion-detection state
    cv::Mat    curr_, next_, prev_, motion_;
    cv::Rect   roi_;
    std::mutex roiMtx_;
    bool       connected_  = false;
    bool       firstFrame_ = true;
};

} // namespace lpr
