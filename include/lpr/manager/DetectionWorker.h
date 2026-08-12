#pragma once
// DetectionWorker - the pipeline spine. It is the single clean replacement for the
// original Managment_Cameras::run_plate_detection + the four near-duplicate
// plate_detection_* variants. It owns no business rules itself; it pulls frames and
// hands them to its collaborators through seams, so each neighbour from the
// decomposition plugs in without DetectionWorker depending on its concrete type:
//
//   upstream  CameraManager ----> std::shared_ptr<FrameQueue>      (ctor)
//   detect    PlateRecognizer ---> IPlateRecognizer&               (ctor)
//   per-plate PlateProcessor /
//             ObjectTracker -----> PlateProcessor  (transform OR drop a plate)
//   per-frame RecordingService /
//             LiveViewService ---> FrameObserver   (runs on every frame)
//   output    PlateSender /
//             PlateQueue --------> PlateSink        (emit kept plates)
//
// The vehicle/tracking stages (VehicleDetector, ObjectTracker) slot in later either
// as a richer recognizer or inside the processor; nothing here needs to change.
#include "lpr/capture/FrameQueue.h"
#include "lpr/detect/IPlateRecognizer.h"
#include "lpr/detect/PlateItem.h"
#include "lpr/process/DirectionEstimator.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lpr {

class LiveOverlay;   // fwd: thread-safe hand-off so the live stream can show detection boxes

class DetectionWorker {
public:
    // Emit a kept plate downstream (PlateQueue today, PlateSender later).
    using PlateSink     = std::function<void(PlateItem&&)>;
    // Validate / dedup / canonicalise / attach track-id. Return nullopt to drop.
    // This is the PlateProcessor (and ObjectTracker) seam - it may MODIFY the plate.
    using PlateProcessor = std::function<std::optional<PlateResult>(PlateResult&&)>;
    // Observe every (non-empty) frame: RecordingService, LiveViewService, etc.
    using FrameObserver = std::function<void(const FrameItem&)>;
    // Convenience predicate seam, adapted onto a (drop-only) PlateProcessor.
    using PlateFilter   = std::function<bool(const PlateResult&)>;
    // Optional: returns the recognition polygon (full-frame px) for a gate, for debug drawing.
    using RoiPolygonProvider = std::function<std::vector<cv::Point>(const std::string& gate, int w, int h)>;
    // Optional: publish a diagnostic JSON (to messages.module_diag) on a notable
    // decision — a direction flip. Best-effort; wired to the NATS transport.
    using DiagSink = std::function<void(std::string)>;

    struct Config {
        int popTimeoutMs = 200;   // how often the loop re-checks running_
        bool showLive = false;    // imshow each processed frame (original 'show_live' debug preview)
        int  liveScale = 1;       // downscale factor for the preview window (original live_scale)
        bool annotateEvidence = true;  // stamp full_image with PC date/time + plate boxes
        bool directionEnable = true;   // infer enter/exit from plate growth (low plate-facing camera)
        DirectionEstimator::Config direction{};   // tuning for the enter/exit decision
        // Per-camera Entry_Exit polarity: true => a plate approaching the camera means ENTER
        // (Entry_Exit=0); false => approaching means EXIT (Entry_Exit=1). Null => treat as true.
        std::function<bool(const std::string& gate)> approachingIsEnter;
        // A quiet gap (ms) longer than this ends a vehicle pass; the same plate seen
        // again after it starts a NEW pass with a fresh passId. Mirrors the backend's
        // pass-correlation window intent (kept small so distinct passes never merge).
        long passGapMs = 5000;
        // Two consecutive reads at a gate whose plate texts are at least this
        // Jaro-Winkler similar are treated as the SAME pass (so OCR flicker between
        // the early announce and the settled correction keeps one passId, and the
        // backend can merge them instead of inserting a duplicate). A clearly
        // different plate (below this) starts a new pass.
        double passPlateSimilarity = 0.85;
    };

    DetectionWorker(std::shared_ptr<FrameQueue> input, IPlateRecognizer& recognizer, Config cfg);
    DetectionWorker(std::shared_ptr<FrameQueue> input, IPlateRecognizer& recognizer)
        : DetectionWorker(std::move(input), recognizer, Config{}) {}   // default config
    ~DetectionWorker();
    DetectionWorker(const DetectionWorker&) = delete;
    DetectionWorker& operator=(const DetectionWorker&) = delete;

    // Wire collaborators BEFORE start(). ---------------------------------------
    void setPlateSink(PlateSink sink)          { sink_ = std::move(sink); }
    void setPlateQueue(std::shared_ptr<PlateQueue> q);     // sink that pushes to q
    void setPlateProcessor(PlateProcessor proc) { processor_ = std::move(proc); }
    void setRoiPolygonProvider(RoiPolygonProvider p) { roiPolygon_ = std::move(p); }
    void setDiagSink(DiagSink d) { diag_ = std::move(d); }
    // Publish detection annotations here so the (continuously-fed) live stream can draw them.
    void setLiveOverlay(LiveOverlay* o) { overlay_ = o; }
    void setPlateFilter(PlateFilter f);                    // bool predicate -> processor
    void addFrameObserver(FrameObserver obs)   { observers_.push_back(std::move(obs)); }

    void start();
    void stop();
    bool isRunning() const { return running_; }

    std::uint64_t framesProcessed() const { return framesProcessed_; }
    std::uint64_t platesEmitted()   const { return platesEmitted_; }

private:
    void run();
    void process(FrameItem& item);

    // Direction delivery. All touched only on the worker thread.
    static long nowSteadyMs();
    int  physicalDirection(const std::string& gate, const std::string& text); // trend only -> 0/1/2
    std::string passIdFor(const std::string& gate, const std::string& text, long nowMs);
    void emitPlate(PlateItem&& out);          // sink + platesEmitted_ counter

    // Per-pass state (key = gate + ":" + plate text) for early-announce + correction:
    // a stable passId reused across the pass, whether the early event fired, and the
    // last physical direction sent (to avoid re-sending an unchanged direction).
    struct PassState { std::string passId; std::string text; long lastSeenMs = 0; long lastInterval = 0; bool emittedEarly = false; int lastSentDir = -1; };
    std::unordered_map<std::string, PassState> passes_;
    // Last enter/exit direction we LOGGED per gate, so the direction line can label
    // a first announce vs a later CORRECTION/flip (for debugging wrong enter/exit).
    std::unordered_map<std::string, int> lastLoggedDir_;

    std::shared_ptr<FrameQueue>  input_;
    IPlateRecognizer&            recognizer_;
    Config                       cfg_;
    PlateSink                    sink_;
    PlateProcessor               processor_;
    RoiPolygonProvider           roiPolygon_;
    DiagSink                     diag_;                // optional module-diag publisher
    LiveOverlay*                 overlay_ = nullptr;   // not owned
    DirectionEstimator           dir_;                 // enter/exit from plate growth
    std::vector<FrameObserver>   observers_;
    std::shared_ptr<PlateQueue>  plateQueue_;   // kept alive when used as the sink

    std::thread                  thread_;
    std::atomic<bool>            running_{false};
    std::atomic<std::uint64_t>   framesProcessed_{0};
    std::atomic<std::uint64_t>   platesEmitted_{0};
};

} // namespace lpr
