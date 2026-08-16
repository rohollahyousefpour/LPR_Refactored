#pragma once
// RecordingService - the clean port of Managment_Cameras::{start_recording, stop_recording,
// handle_live_and_recording}'s recording half. It records frames for a gate to segmented
// video files. It is a *frame observer* (plug into DetectionWorker::addFrameObserver or the
// camera frame stream), so it sees frames without the detection code knowing about it.
//
// Improvements over the original (which used a single global vidwrit / recording_gate):
//   * multi-gate: any number of gates can record at once (map keyed by gate)
//   * no per-frame std::async (the original spawned a thread per written frame); frames are
//     written synchronously on the observer's thread
//   * duration expiry is handled on the frame path (steady_clock), not a detached sleeper
//   * a segment-complete callback replaces the hard-coded handel_client->send_recording_async,
//     so the messaging layer can be wired later without this class depending on it
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace lpr {

class RecordingService {
public:
    struct Config {
        double      fps             = 4.0;
        cv::Size    frameSize       = {1280, 960};
        int         crf             = 30;        // libx264 quality: higher = smaller file
                                                 // (18 near-lossless, 23 default, 28-34 small)
        std::string x264Preset      = "medium";  // slower preset = smaller file at same CRF
        int         segmentSeconds  = 900;       // roll over to a new file every 15 min
        std::string fourcc          = "avc1";
        std::string extension       = ".mp4";
        bool        timestampOverlay = true;     // burn a centered datetime into each frame
        std::string baseDir         = "recordings";  // <baseDir>/<gate>/<timestamp>/CAM_<gate>_<ts>
        int         watchdogMs      = 500;        // close timed-out recordings even if frames stop (0 = off)
        // Disk retention: without it, segment files accumulate forever and fill the disk. Old
        // files are pruned by AGE first, then (if still over budget) OLDEST-first until the total
        // is back under the size cap. The currently-open segments are never touched. 0 disables
        // either rule. Application.cpp overrides these from settings (recording_retention_days /
        // recording_max_gb) so operators can tune retention without a rebuild.
        int         retentionDays    = 14;                       // delete segments older than this (0 = off)
        long long   maxTotalBytes    = 20LL * 1024 * 1024 * 1024; // cap total recordings size (0 = off)
        long long   minFreeBytes     = 5LL * 1024 * 1024 * 1024;  // hard floor: keep this much disk free by
                                                                  // deleting oldest segments (0 = off). A last
                                                                  // resort even if the size cap is generous.
        int         pruneIntervalSec = 3600;                     // how often the watchdog runs a prune pass
    };

    // Called when a segment file is finished (closed): (gate, filePath).
    using SegmentCallback = std::function<void(const std::string& gate, const std::string& path)>;

    explicit RecordingService(Config cfg);
    RecordingService() : RecordingService(Config{}) {}
    ~RecordingService();

    bool startRecording(const std::string& gate, int durationSeconds = 0);  // 0 = until stopped
    void stopRecording(const std::string& gate);
    bool isRecording(const std::string& gate) const;
    std::size_t activeCount() const;

    // Frame observer: writes the frame if `gate` is recording (handles rollover + expiry).
    void onFrame(const std::string& gate, const cv::Mat& img);

    // Close any recordings whose duration has elapsed (called by the watchdog, or manually).
    void tick();

    void setSegmentCompleteCallback(SegmentCallback cb) { onSegment_ = std::move(cb); }

private:
    struct Rec {
        cv::VideoWriter writer;
        std::string     baseName;       // path stem without segment suffix
        std::string     currentPath;    // currently-open segment file
        int             segmentIndex = 0;
        std::chrono::steady_clock::time_point segmentStart;
        std::chrono::steady_clock::time_point stopAt;
        bool            hasDuration = false;
    };

    std::string makeBaseName(const std::string& gate) const;
    std::string segmentPath(const Rec& r) const;
    void        openSegment(const std::string& gate, Rec& r);
    void        closeSegment(const std::string& gate, Rec& r);   // releases + fires callback
    void        stampAndWrite(Rec& r, const cv::Mat& img);
    void        closeExpiredLocked();                            // caller holds mtx_
    void        watchdogLoop();
    // Delete expired / over-budget segment files under baseDir (never the open ones).
    void        pruneOldRecordings();

    Config            cfg_;
    SegmentCallback   onSegment_;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, Rec> recs_;

    std::chrono::steady_clock::time_point lastPrune_{};          // throttles pruneOldRecordings()

    std::thread             watchdog_;
    std::atomic<bool>       watchdogRunning_{false};
    std::condition_variable watchdogCv_;
    std::mutex              watchdogMtx_;
};

} // namespace lpr
