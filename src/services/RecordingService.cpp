#include <cstdlib>
#include "lpr/services/RecordingService.h"
#include "lpr/Log.h"

#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace lpr {

namespace {
std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y_%m_%d_%H_%M_%S");
    return oss.str();
}
std::string datetimeText() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}
} // namespace

RecordingService::RecordingService(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.watchdogMs > 0) {
        watchdogRunning_ = true;
        watchdog_ = std::thread(&RecordingService::watchdogLoop, this);
    }
}

RecordingService::~RecordingService() {
    watchdogRunning_ = false;
    { std::lock_guard<std::mutex> lk(watchdogMtx_); watchdogCv_.notify_all(); }
    if (watchdog_.joinable()) watchdog_.join();
    // close any still-open segments cleanly
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& kv : recs_) closeSegment(kv.first, kv.second);
    recs_.clear();
}

void RecordingService::watchdogLoop() {
    std::unique_lock<std::mutex> lk(watchdogMtx_);
    while (watchdogRunning_) {
        watchdogCv_.wait_for(lk, std::chrono::milliseconds(cfg_.watchdogMs),
                             [this] { return !watchdogRunning_.load(); });
        if (!watchdogRunning_) break;
        tick();
    }
}

void RecordingService::closeExpiredLocked() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = recs_.begin(); it != recs_.end();) {
        if (it->second.hasDuration && now >= it->second.stopAt) {
            closeSegment(it->first, it->second);
            it = recs_.erase(it);
        } else {
            ++it;
        }
    }
}

void RecordingService::tick() {
    std::lock_guard<std::mutex> lk(mtx_);
    closeExpiredLocked();
}

std::string RecordingService::makeBaseName(const std::string& gate) const {
    const std::string ts = nowStamp();
    std::filesystem::path dir = std::filesystem::path(cfg_.baseDir) / gate / ts;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) LOGE() << "RecordingService: cannot create " << dir.string() << ": " << ec.message();
    return (dir / ("CAM_" + gate + "_" + ts)).string();
}

std::string RecordingService::segmentPath(const Rec& r) const {
    return r.baseName + "_seg" + std::to_string(r.segmentIndex) + cfg_.extension;
}

void RecordingService::openSegment(const std::string& gate, Rec& r) {
    std::string fc = cfg_.fourcc;
    fc.resize(4, ' ');                                  // guard: pad/truncate to 4 chars
    int fourcc = cv::VideoWriter::fourcc(fc[0], fc[1], fc[2], fc[3]);
    r.currentPath = segmentPath(r);

    // Force SOFTWARE encoding. Some Windows FFMPEG builds bind avc1/H.264 to the hardware encoder
    // h264_d3d12va, which only accepts d3d12 GPU surfaces (not the CPU BGR/yuv420p frames we feed)
    // and fails to open the codec. VIDEO_ACCELERATION_NONE keeps H.264 but routes it through the
    // software encoder (libx264), which accepts our frames and stays browser-playable.
    const std::vector<int> params = { cv::VIDEOWRITER_PROP_HW_ACCELERATION,
                                      cv::VIDEO_ACCELERATION_NONE };

    // Tune libx264 for small files (only affects the software H.264 path; ignored by mp4v).
    // Honored by OpenCV's FFMPEG backend when the writer is constructed.
    {
        const std::string opts = "preset;" + cfg_.x264Preset + "|crf;" + std::to_string(cfg_.crf);
#if defined(_WIN32)
        _putenv_s("OPENCV_FFMPEG_WRITER_OPTIONS", opts.c_str());
#else
        setenv("OPENCV_FFMPEG_WRITER_OPTIONS", opts.c_str(), 1);
#endif
    }

    bool ok = r.writer.open(r.currentPath, cv::CAP_FFMPEG, fourcc, cfg_.fps, cfg_.frameSize, params);
    if (!ok || !r.writer.isOpened()) {
        // Fallback: software MPEG-4 (mp4v) in the same .mp4 container.
        LOGW() << "RecordingService[" << gate << "]: '" << cfg_.fourcc
               << "' writer failed to open; falling back to mp4v (software)";
        r.writer.release();
        int fb = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
        ok = r.writer.open(r.currentPath, cv::CAP_FFMPEG, fb, cfg_.fps, cfg_.frameSize, params);
    }

    if (ok && r.writer.isOpened())
        LOGI() << "RecordingService[" << gate << "]: opened " << r.currentPath;
    else
        LOGE() << "RecordingService[" << gate << "]: failed to open " << r.currentPath
               << " (size=" << cfg_.frameSize.width << "x" << cfg_.frameSize.height
               << " fps=" << cfg_.fps << ")";
}

void RecordingService::closeSegment(const std::string& gate, Rec& r) {
    if (r.writer.isOpened()) {
        r.writer.release();
        LOGI() << "RecordingService[" << gate << "]: closed " << r.currentPath;
        if (onSegment_ && !r.currentPath.empty()) onSegment_(gate, r.currentPath);
    }
    r.currentPath.clear();
}

void RecordingService::stampAndWrite(Rec& r, const cv::Mat& img) {
    cv::Mat out;
    cv::resize(img, out, cfg_.frameSize);
    if (cfg_.timestampOverlay) {
        const std::string text = datetimeText();
        int baseline = 0;
        cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 1.0, 2, &baseline);
        cv::Point org((out.cols - ts.width) / 2, ts.height + 10);
        cv::putText(out, text, org, cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255, 255, 255), 2);
    }
    r.writer.write(out);
}

bool RecordingService::startRecording(const std::string& gate, int durationSeconds) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (recs_.count(gate)) { LOGW() << "RecordingService[" << gate << "]: already recording"; return false; }
    Rec r;
    r.baseName     = makeBaseName(gate);
    r.segmentIndex = 0;
    r.segmentStart = std::chrono::steady_clock::now();
    r.hasDuration  = durationSeconds > 0;
    if (r.hasDuration) r.stopAt = r.segmentStart + std::chrono::seconds(durationSeconds);
    recs_.emplace(gate, std::move(r));
    LOGI() << "RecordingService[" << gate << "]: started (duration=" << durationSeconds << "s)";
    return true;
}

void RecordingService::stopRecording(const std::string& gate) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = recs_.find(gate);
    if (it == recs_.end()) return;
    closeSegment(gate, it->second);
    recs_.erase(it);
    LOGI() << "RecordingService[" << gate << "]: stopped";
}

bool RecordingService::isRecording(const std::string& gate) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return recs_.count(gate) != 0;
}

std::size_t RecordingService::activeCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return recs_.size();
}

void RecordingService::onFrame(const std::string& gate, const cv::Mat& img) {
    if (img.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = recs_.find(gate);
    if (it == recs_.end()) return;
    Rec& r = it->second;

    const auto now = std::chrono::steady_clock::now();
    if (r.hasDuration && now >= r.stopAt) {          // duration elapsed -> stop
        closeSegment(gate, r);
        recs_.erase(it);
        LOGI() << "RecordingService[" << gate << "]: duration reached, stopped";
        return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - r.segmentStart).count() >= cfg_.segmentSeconds) {
        closeSegment(gate, r);                       // 15-min rollover
        ++r.segmentIndex;
        r.segmentStart = now;
    }
    if (!r.writer.isOpened()) openSegment(gate, r);
    if (r.writer.isOpened()) stampAndWrite(r, img);
}

} // namespace lpr
