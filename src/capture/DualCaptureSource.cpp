#include "lpr/capture/DualCaptureSource.h"
#include "lpr/Log.h"

#include <algorithm>

namespace lpr {

using clock = std::chrono::steady_clock;

DualCaptureSource::DualCaptureSource(std::unique_ptr<CaptureSource> main,
                                     std::unique_ptr<CaptureSource> mono)
    : main_(std::move(main)), mono_(std::move(mono)) {}

DualCaptureSource::~DualCaptureSource() { stop(); }

void DualCaptureSource::setAddress(const std::string& address, int delayMs) {
    if (main_) main_->setAddress(address, delayMs);
}
void DualCaptureSource::setMonoAddress(const std::string& address, int delayMs) {
    if (mono_) mono_->setAddress(address, delayMs);
}
void DualCaptureSource::handleCommand(const std::string& key, const std::string& value) {
    if (main_) main_->handleCommand(key, value);
    if (mono_) mono_->handleCommand(key, value);
}

bool DualCaptureSource::fresh(clock::time_point t) const {
    if (t.time_since_epoch().count() == 0) return false;
    return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t).count() < kStaleMs;
}

bool DualCaptureSource::isLive() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return (haveMain_ && fresh(lastMain_)) || (haveMono_ && fresh(lastMono_));
}

void DualCaptureSource::interruptibleSleep(int ms) {
    std::unique_lock<std::mutex> lk(stopMtx_);
    stopCv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !running_; });
}

// Build a pair from whatever is currently fresh and emit it. The main stream drives whenever it
// produces a frame; the mono stream drives ONLY while main is not fresh (so we don't double-emit
// when both are healthy). A dead stream contributes an empty Mat, which CameraWorker turns into
// "use the survivor for both detection and evidence".
void DualCaptureSource::emitPair(bool triggerIsMain, long ts) {
    cv::Mat mainImg, monoImg;
    bool mainFresh, monoFresh;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        mainFresh = haveMain_ && fresh(lastMain_);
        monoFresh = haveMono_ && fresh(lastMono_);
        if (mainFresh) latestMain_.copyTo(mainImg);
        if (monoFresh) latestMono_.copyTo(monoImg);
    }
    if (!triggerIsMain && mainFresh) return;          // main is healthy -> it drives, not mono
    if (mainImg.empty() && monoImg.empty()) return;

    std::lock_guard<std::mutex> lk(emitMtx_);          // serialize -> CameraWorker.onFrame is single-entry
    emitFrame(mainImg, monoImg, ts);
}

void DualCaptureSource::runMonoLoop() {
    if (!mono_) return;
    mono_->onFrame([this](const cv::Mat& f, const cv::Mat&, long t) {
        if (f.empty()) return;
        { std::lock_guard<std::mutex> lk(mtx_); f.copyTo(latestMono_); lastMono_ = clock::now(); haveMono_ = true; }
        emitPair(/*triggerIsMain=*/false, t);
    });
    mono_->onError([] { LOGW() << "DualCaptureSource: mono stream error"; });

    int backoff = 500;
    while (running_) {
        try { mono_->run(); }
        catch (const std::exception& e) { LOGE() << "DualCaptureSource: mono run: " << e.what(); }
        catch (...)                     { LOGE() << "DualCaptureSource: mono run: unknown error"; }
        if (!running_) break;
        { std::lock_guard<std::mutex> lk(mtx_); haveMono_ = false; }
        LOGW() << "DualCaptureSource: mono stream down; retrying in " << backoff << "ms";
        interruptibleSleep(backoff);
        backoff = std::min(backoff * 2, 8000);
    }
}

void DualCaptureSource::run() {
    if (!main_) return;
    running_ = true;

    if (mono_) monoThread_ = std::thread([this] { runMonoLoop(); });

    main_->onFrame([this](const cv::Mat& f, const cv::Mat&, long t) {
        if (f.empty()) return;
        { std::lock_guard<std::mutex> lk(mtx_); f.copyTo(latestMain_); lastMain_ = clock::now(); haveMain_ = true; }
        emitPair(/*triggerIsMain=*/true, t);
    });
    main_->onError([] { LOGW() << "DualCaptureSource: main stream error"; });

    int backoff = 500;
    while (running_) {
        try { main_->run(); }
        catch (const std::exception& e) { LOGE() << "DualCaptureSource: main run: " << e.what(); }
        catch (...)                     { LOGE() << "DualCaptureSource: main run: unknown error"; }
        if (!running_) break;

        bool monoAlive;
        { std::lock_guard<std::mutex> lk(mtx_); haveMain_ = false; monoAlive = haveMono_ && fresh(lastMono_); }
        if (!monoAlive) {                       // both down -> let CameraWorker reconnect the whole source
            LOGW() << "DualCaptureSource: both streams down; signalling reconnect";
            emitError();
            break;
        }
        LOGW() << "DualCaptureSource: main stream down; serving from mono, retrying main in " << backoff << "ms";
        interruptibleSleep(backoff);
        backoff = std::min(backoff * 2, 8000);
    }

    running_ = false;
    if (mono_) mono_->stop();
    { std::lock_guard<std::mutex> lk(stopMtx_); }
    stopCv_.notify_all();
    if (monoThread_.joinable()) monoThread_.join();
}

void DualCaptureSource::stop() {
    running_ = false;
    if (main_) main_->stop();
    if (mono_) mono_->stop();
    { std::lock_guard<std::mutex> lk(stopMtx_); }
    stopCv_.notify_all();
    // monoThread_ is joined by run() (its owner) to avoid a double join.
}

} // namespace lpr
