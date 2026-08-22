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
    // Fires with a reference frame for the operator's editors: colorVariant=false is the
    // DETECTION frame (mono for a pair) — the ROI/zone reference; colorVariant=true is the
    // COLOUR evidence frame of a mono+colour pair — the colour-crop reference.
    using FirstFrameCallback = std::function<void(const std::string& gate, const cv::Mat& frame, bool colorVariant)>;

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

    // Live-update the motion-gate tuning (from a settings save) without a reconnect —
    // only the per-frame gate thresholds change; the ROI, source, and mono/RGB pairing
    // are left as-is. Fields are POD and read on the capture thread, so a concurrent
    // read only ever sees a benign field-mix.
    void setMotionTuning(double minDev, double maxDev, int minChangedPixels, int delayMs) {
        cfg_.minDeviation     = minDev;
        cfg_.maxDeviation     = maxDev;
        cfg_.minChangedPixels = minChangedPixels;
        cfg_.delayMs          = delayMs;
    }

    // Forward a runtime command to the live source (thread-safe).
    void handleCommand(const std::string& key, const std::string& value);

    // Ask the live source to re-read its settings and apply any hardware changes
    // (Basler: targeted reconnect). Thread-safe; no-op if the source isn't up yet.
    void reapplySourceSettings() {
        std::lock_guard<std::mutex> lk(sourceMtx_);
        if (source_) source_->reapplySettings();
    }

    // Read the exposure (us) + gain a sub-camera (by serial) actually holds now.
    bool readExposureGain(const std::string& serial, double& exposureUs, double& gain);

    // Read the sub-camera's current hardware AOI + sensor ranges (manual-control AOI sliders).
    bool readAoi(const std::string& serial, CaptureSource::Aoi& out);

    // Read the sub-camera's read-only device health (manual-control diagnostics tiles).
    bool readDiag(const std::string& serial, CaptureSource::Diag& out);

    // Take (return + clear) a one-shot exported preset (.pfs text) stashed after "Export Preset".
    bool readPreset(const std::string& serial, std::string& out);

    // Fetch the latest frame a sub-camera (by serial) grabbed, for the manual-control live view.
    bool latestFrame(const std::string& serial, cv::Mat& out);

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
    bool       firstFrame_ = true;        // detection (mono) reference — for the ROI/zone editor
    bool       firstColorFrame_ = true;   // colour evidence reference — for the colour-crop editor
    // Frame size at the last crud send. A size change (e.g. a live AOI/crop change from manual
    // control shrinks/grows the frame) re-arms the crud so the ROI editor shows the CURRENT crop.
    cv::Size   lastCrudSize_{0, 0};
    cv::Size   lastColorCrudSize_{0, 0};
};

} // namespace lpr
