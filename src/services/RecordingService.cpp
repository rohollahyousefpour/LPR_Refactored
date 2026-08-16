#include <cstdlib>
#include "lpr/services/RecordingService.h"
#include "lpr/Log.h"

#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cstdint>

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
        // Periodic disk retention, throttled to pruneIntervalSec (first pass runs promptly so a
        // backlog from before this build is cleaned on boot). Filesystem I/O runs WITHOUT the
        // watchdog lock held.
        if (cfg_.pruneIntervalSec > 0 && (cfg_.retentionDays > 0 || cfg_.maxTotalBytes > 0)) {
            const auto now = std::chrono::steady_clock::now();
            const bool first = lastPrune_.time_since_epoch().count() == 0;
            if (first || std::chrono::duration_cast<std::chrono::seconds>(now - lastPrune_).count()
                             >= cfg_.pruneIntervalSec) {
                lastPrune_ = now;
                lk.unlock();
                pruneOldRecordings();
                lk.lock();
            }
        }
    }
}

void RecordingService::pruneOldRecordings() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base(cfg_.baseDir);
    if (!fs::exists(base, ec)) return;

    // Snapshot the segments currently open for writing — never delete those.
    std::vector<fs::path> active;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& kv : recs_)
            if (!kv.second.currentPath.empty()) active.emplace_back(kv.second.currentPath);
    }
    auto isActive = [&](const fs::path& p) {
        for (auto& a : active) { std::error_code e; if (fs::equivalent(p, a, e)) return true; }
        return false;
    };

    struct Item { fs::path path; long long size; fs::file_time_type mtime; };
    std::vector<Item> items;
    long long total = 0;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        std::error_code e;
        if (!fs::is_regular_file(p, e)) continue;
        if (p.extension() != cfg_.extension) continue;   // only our segment files
        if (isActive(p)) continue;
        const auto sz = fs::file_size(p, e);          if (e) continue;
        const auto mt = fs::last_write_time(p, e);    if (e) continue;
        items.push_back({ p, (long long)sz, mt });
        total += (long long)sz;
    }

    int deleted = 0; long long freed = 0;
    auto tryRemove = [&](Item& it) {
        std::error_code e;
        if (fs::remove(it.path, e)) { ++deleted; freed += it.size; total -= it.size; it.size = 0; }
    };

    // 1) AGE — delete segments older than retentionDays.
    if (cfg_.retentionDays > 0) {
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24LL * cfg_.retentionDays);
        for (auto& it : items) if (it.size > 0 && it.mtime < cutoff) tryRemove(it);
    }
    // Oldest-first ordering for the two byte-budget passes below.
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.mtime < b.mtime; });

    // 2) SIZE CAP — if still over budget, delete OLDEST-first until under.
    if (cfg_.maxTotalBytes > 0) {
        for (auto& it : items) {
            if (total <= cfg_.maxTotalBytes) break;
            if (it.size > 0) tryRemove(it);
        }
    }
    // 3) FREE-SPACE FLOOR — hard safety net: if the disk is still tighter than minFreeBytes
    //    (e.g. other data filled it), keep deleting OLDEST-first until the floor is met or we
    //    run out of prunable segments. `space()` is re-read each step so we stop as soon as
    //    enough is free.
    if (cfg_.minFreeBytes > 0) {
        std::error_code se;
        for (auto& it : items) {
            const auto sp = fs::space(base, se);
            if (se || sp.available >= (std::uintmax_t)cfg_.minFreeBytes) break;
            if (it.size > 0) tryRemove(it);
        }
    }

    // 4) Best-effort: remove directories left empty by the deletions (deepest first).
    if (deleted > 0) {
        std::vector<fs::path> dirs;
        for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end(),
                  [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
        for (auto& d : dirs) { std::error_code e; fs::remove(d, e); }  // remove() only succeeds if empty
    }

    if (deleted > 0)
        LOGI() << "RecordingService: pruned " << deleted << " old segment(s), freed "
               << (freed / (1024 * 1024)) << " MB (retentionDays=" << cfg_.retentionDays
               << ", maxGB=" << (cfg_.maxTotalBytes / (1024LL * 1024 * 1024)) << ")";
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
    r.currentPath = segmentPath(r);

    // Preferred path: real H.264 via Media Foundation (h264_mf) driven through libavcodec directly
    // — small, browser-playable, and needs NO extra DLL. cv::VideoWriter can't select this encoder,
    // so we bypass it here and only fall back to the OpenCV chain (mp4v) if it's unavailable.
    r.h264 = std::make_unique<FfmpegH264Writer>();
    if (r.h264->open(r.currentPath, cfg_.fps, cfg_.frameSize, cfg_.crf)) {
        LOGI() << "RecordingService[" << gate << "]: opened " << r.currentPath << " [H.264 Media Foundation]";
        return;
    }
    r.h264.reset();   // h264_mf unavailable/failed -> OpenCV mp4v fallback below

    // OPENCV_FFMPEG_WRITER_OPTIONS carries ENCODER-SPECIFIC private options. libx264's
    // preset/crf are meaningless to (and rejected by) Media Foundation, so it is set only for the
    // software-x264 attempt and cleared otherwise.
    auto setOpts = [](const std::string& opts) {
#if defined(_WIN32)
        _putenv_s("OPENCV_FFMPEG_WRITER_OPTIONS", opts.c_str());   // "" clears it
#else
        if (opts.empty()) unsetenv("OPENCV_FFMPEG_WRITER_OPTIONS");
        else              setenv("OPENCV_FFMPEG_WRITER_OPTIONS", opts.c_str(), 1);
#endif
    };
    const std::string x264opts = "preset;" + cfg_.x264Preset + "|crf;" + std::to_string(cfg_.crf);

    // Open the smallest codec this build can actually produce, first-to-open wins:
    //   1. H.264 via ANY acceleration — on Windows this reaches the built-in Media Foundation
    //      encoder (h264_mf): small, browser-playable, and needs NO extra DLL (mfplat is linked).
    //   2. H.264 software — used if this FFMPEG has libx264/libopenh264 built in (LGPL vcpkg
    //      builds usually don't, so this is a no-op there but harmless).
    //   3. MPEG-4 (mp4v) — always present, ~3-5x larger; last resort so recording never fails.
    struct Attempt { const char* fourcc; int accel; std::string opts; const char* label; };
    const Attempt attempts[] = {
        { "avc1", cv::VIDEO_ACCELERATION_ANY,  std::string(), "H.264 (Media Foundation / HW)" },
        { "avc1", cv::VIDEO_ACCELERATION_NONE, x264opts,      "H.264 (software x264/openh264)" },
        { "mp4v", cv::VIDEO_ACCELERATION_NONE, std::string(), "MPEG-4 mp4v (fallback)" },
    };
    for (const Attempt& a : attempts) {
        std::string fc = a.fourcc; fc.resize(4, ' ');
        const int fourcc = cv::VideoWriter::fourcc(fc[0], fc[1], fc[2], fc[3]);
        const std::vector<int> params = { cv::VIDEOWRITER_PROP_HW_ACCELERATION, a.accel };
        setOpts(a.opts);
        r.writer.release();
        if (r.writer.open(r.currentPath, cv::CAP_FFMPEG, fourcc, cfg_.fps, cfg_.frameSize, params)
            && r.writer.isOpened()) {
            LOGI() << "RecordingService[" << gate << "]: opened " << r.currentPath
                   << " [" << a.label << "]";
            return;
        }
    }
    LOGE() << "RecordingService[" << gate << "]: failed to open " << r.currentPath
           << " (size=" << cfg_.frameSize.width << "x" << cfg_.frameSize.height
           << " fps=" << cfg_.fps << ")";
}

void RecordingService::closeSegment(const std::string& gate, Rec& r) {
    const bool wasOpen = (r.h264 && r.h264->isOpened()) || r.writer.isOpened();
    if (r.h264 && r.h264->isOpened()) r.h264->close();   // flush + write trailer
    if (r.writer.isOpened())          r.writer.release();
    r.h264.reset();
    if (wasOpen) {
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
    if (r.h264 && r.h264->isOpened()) r.h264->write(out);
    else                              r.writer.write(out);
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
    // "A segment is open" means EITHER the H.264/MF writer or the mp4v fallback writer is open.
    // Checking only r.writer (the old behavior) re-opened the file every frame in H.264 mode.
    auto segOpen = [&r] { return (r.h264 && r.h264->isOpened()) || r.writer.isOpened(); };
    if (!segOpen()) openSegment(gate, r);
    if (segOpen())  stampAndWrite(r, img);
}

} // namespace lpr
