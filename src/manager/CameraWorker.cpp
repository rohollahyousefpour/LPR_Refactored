#include "lpr/manager/CameraWorker.h"
#include "lpr/Log.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>

namespace lpr {

CameraWorker::CameraWorker(std::string gate, SourceFactoryFn factory, MotionConfig cfg)
    : gate_(std::move(gate)), factory_(std::move(factory)), cfg_(cfg), roi_(cfg.roi) {}

CameraWorker::~CameraWorker() { stop(); }

void CameraWorker::setRoi(const cv::Rect& roi) {
    std::lock_guard<std::mutex> lk(roiMtx_);
    roi_ = roi;
}

void CameraWorker::handleCommand(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(sourceMtx_);
    if (source_) source_->handleCommand(key, value);
}

bool CameraWorker::readExposureGain(const std::string& serial, double& exposureUs, double& gain) {
    std::lock_guard<std::mutex> lk(sourceMtx_);
    return source_ && source_->readAppliedExposureGain(serial, exposureUs, gain);
}

bool CameraWorker::latestFrame(const std::string& serial, cv::Mat& out) {
    std::lock_guard<std::mutex> lk(sourceMtx_);
    return source_ && source_->latestFrame(serial, out);
}

void CameraWorker::start() {
    if (running_.exchange(true)) return;
    if (supervisor_.joinable()) supervisor_.join();   // join a previously self-stopped run
    supervisor_ = std::thread(&CameraWorker::supervise, this);
}

void CameraWorker::stop() {
    running_ = false;                                 // idempotent; do NOT early-return on
    { std::lock_guard<std::mutex> lk(waitMtx_); waitCv_.notify_all(); }   // self-stopped workers
    { std::lock_guard<std::mutex> lk(sourceMtx_); if (source_) source_->stop(); }
    if (supervisor_.joinable()) supervisor_.join();
}

void CameraWorker::supervise() {
    int backoff = cfg_.reconnectBaseMs;

    while (running_) {
        try {
            std::lock_guard<std::mutex> lk(sourceMtx_);
            source_ = factory_();
        } catch (const std::exception& ex) {
            LOGE() << "CameraWorker[" << gate_ << "]: source factory threw: " << ex.what();
            source_.reset();
        }
        if (!source_) {
            LOGE() << "CameraWorker[" << gate_ << "]: failed to create source";
            std::unique_lock<std::mutex> lk(waitMtx_);
            waitCv_.wait_for(lk, std::chrono::milliseconds(backoff), [this] { return !running_; });
            backoff = std::min(backoff * 2, cfg_.reconnectMaxMs);
            continue;
        }

        source_->onFrame([this](const cv::Mat& f, const cv::Mat& m, long t) { onFrame(f, m, t); });
        source_->onError([this] { onError(); });

        errored_ = false;
        sourceFinished_ = false;
        framesThisSession_ = 0;
        curr_.release(); next_.release(); prev_.release();

        sourceThread_ = std::thread([this] {
            try { source_->run(); }
            catch (const std::exception& ex) { LOGE() << "CameraWorker[" << gate_ << "]: " << ex.what(); onError(); }
            sourceFinished_ = true;
            std::lock_guard<std::mutex> lk(waitMtx_);
            waitCv_.notify_all();
        });

        {
            std::unique_lock<std::mutex> lk(waitMtx_);
            waitCv_.wait(lk, [this] { return errored_.load() || sourceFinished_.load() || !running_; });
        }

        { std::lock_guard<std::mutex> lk(sourceMtx_); if (source_) source_->stop(); }
        if (sourceThread_.joinable()) sourceThread_.join();
        { std::lock_guard<std::mutex> lk(sourceMtx_); source_.reset(); }

        if (!running_) break;

        const bool stable = framesThisSession_.load() > 0;

        if (sourceFinished_ && !errored_) {
            LOGI() << "CameraWorker[" << gate_ << "]: stream ended cleanly; stopping";
            break;
        }

        emitStatus(false);
        connected_  = false;   // so the next successful frame re-emits "connected"
        firstFrame_ = true;     // and the first-frame hook (ROI calc) runs again after reconnect
        if (stable) backoff = cfg_.reconnectBaseMs;                 // (b) reset after a stable session
        LOGW() << "CameraWorker[" << gate_ << "]: reconnecting in " << backoff << "ms";
        std::unique_lock<std::mutex> lk(waitMtx_);
        waitCv_.wait_for(lk, std::chrono::milliseconds(backoff), [this] { return !running_; });
        backoff = std::min(backoff * 2, cfg_.reconnectMaxMs);
    }

    running_ = false;
}

void CameraWorker::onError() {
    errored_ = true;
    std::lock_guard<std::mutex> lk(waitMtx_);
    waitCv_.notify_all();
}

void CameraWorker::emitStatus(bool connected) {
    if (statusCb_) statusCb_(gate_, connected);
}

int CameraWorker::countChangedPixels(const cv::Mat& motion) const {
    cv::Scalar mean, stddev;
    cv::meanStdDev(motion, mean, stddev);
    if (stddev[0] <= cfg_.minDeviation)
        return (mean[0] > 0) ? -1 : 0;

    int changed = 0;
    for (int j = 0; j < motion.rows; j += 2) {
        const uchar* row = motion.ptr<uchar>(j);
        for (int i = 0; i < motion.cols; i += 2)
            if (row[i] == 255) ++changed;
    }
    return changed;
}

void CameraWorker::onFrame(const cv::Mat& frame, const cv::Mat& mono, long timestamp) {
    // Resolve the detection image vs the RGB evidence image, with graceful degradation: if one
    // stream of a mono+RGB pair is down (its Mat arrives empty), the surviving stream is used for
    // BOTH detection and evidence. Single camera (mono always empty) => same frame for both.
    //   detectOnSecondary (IP):  detect prefers mono(secondary), evidence prefers RGB(primary).
    //   else (Basler/single):    detect prefers primary,         evidence prefers secondary.
    cv::Mat detectImg, evidenceImg;
    if (cfg_.detectOnSecondary) {
        detectImg   = !mono.empty()  ? mono  : frame;
        evidenceImg = !frame.empty() ? frame : mono;
    } else {
        detectImg   = !frame.empty() ? frame : mono;
        evidenceImg = !mono.empty()  ? mono  : frame;
    }
    if (detectImg.empty()) return;                 // nothing usable this tick

    ++framesThisSession_;
    if (!connected_) { connected_ = true; emitStatus(true); }

    const bool haveSeparateEvidence = !evidenceImg.empty() && evidenceImg.data != detectImg.data;

    // Continuous live view: prefer the COLOR / RGB evidence image when a mono+RGB pair is present,
    // so the dashboard live feed is color while the mono/IR stream stays dedicated to plate
    // detection and the plate crop. This makes Basler behave like IP (which already shows the RGB
    // stream live). Fall back to the detection image for a single camera (no separate evidence).
    // NOTE: detection overlay boxes are computed in detection-image (mono) coordinates; they line
    // up on the color live view only when the two streams share the same FOV/resolution (a
    // co-located / rectified mono+RGB pair). If a deployment's color and mono differ a lot in
    // framing, the boxes on the live view may be offset (detection itself is unaffected).
    const cv::Mat& liveImg = haveSeparateEvidence ? evidenceImg : detectImg;
    if (firstFrame_) { firstFrame_ = false; if (firstFrameCb_) firstFrameCb_(gate_, liveImg); }
    if (rawObserver_) {
        FrameItem raw;
        raw.image = liveImg;          // shared header; live encodes immediately, no clone needed
        raw.timestamp = timestamp;
        raw.gate = gate_;
        rawObserver_(std::move(raw));
    }

    auto forward = [&]() {
        if (!frameSink_) return;
        FrameItem item;
        item.image         = detectImg.clone();
        item.evidenceImage = haveSeparateEvidence ? evidenceImg.clone() : cv::Mat();
        item.timestamp     = timestamp;
        item.gate          = gate_;
        frameSink_(std::move(item));
    };

    if (!cfg_.enableMotionGate) { forward(); }
    else {
        cv::Mat region = detectImg;       // motion gate runs on the detection image
        { std::lock_guard<std::mutex> lk(roiMtx_);
          if (roi_.area() > 0) {
              cv::Rect r = roi_ & cv::Rect(0, 0, detectImg.cols, detectImg.rows);
              if (r.area() > 0) region = detectImg(r);
          } }

        if (curr_.empty())      { cv::cvtColor(region, curr_, cv::COLOR_BGR2GRAY); }
        else if (next_.empty()) { cv::cvtColor(region, next_, cv::COLOR_BGR2GRAY); }
        else {
            curr_.copyTo(prev_);
            next_.copyTo(curr_);
            cv::cvtColor(region, next_, cv::COLOR_BGR2GRAY);

            cv::Mat d1, d2;
            cv::absdiff(prev_, curr_, d1);
            cv::absdiff(next_, curr_, d2);
            cv::bitwise_and(d1, d2, motion_);
            cv::threshold(motion_, motion_, 15, 255, cv::THRESH_BINARY);
            cv::erode(motion_, motion_, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));

            if (countChangedPixels(motion_) >= cfg_.minChangedPixels)
                forward();
        }
    }

    if (cfg_.delayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delayMs));
}

} // namespace lpr
