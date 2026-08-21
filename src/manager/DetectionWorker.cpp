#include "lpr/manager/DetectionWorker.h"
#include "lpr/Log.h"
#include "lpr/manager/LiveOverlay.h"
#include "lpr/util/Time.h"
#include "lpr/util/Uuid.h"
#include "lpr/util/JaroWinkler.h"

#include <nlohmann/json.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cmath>
#include <map>

namespace lpr {

DetectionWorker::DetectionWorker(std::shared_ptr<FrameQueue> input, IPlateRecognizer& recognizer, Config cfg)
    : input_(std::move(input)), recognizer_(recognizer), cfg_(cfg) { dir_.setConfig(cfg_.direction); }

DetectionWorker::~DetectionWorker() { stop(); }

void DetectionWorker::setPlateQueue(std::shared_ptr<PlateQueue> q) {
    plateQueue_ = std::move(q);
    auto qref = plateQueue_;
    sink_ = [qref](PlateItem&& it) { if (qref) qref->push(std::move(it)); };
}

void DetectionWorker::setPlateFilter(PlateFilter f) {
    processor_ = [f = std::move(f)](PlateResult&& p) -> std::optional<PlateResult> {
        if (f && !f(p)) return std::nullopt;
        return std::optional<PlateResult>(std::move(p));
    };
}

void DetectionWorker::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();   // join a previously stopped run
    thread_ = std::thread(&DetectionWorker::run, this);
}

void DetectionWorker::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void DetectionWorker::run() {
    if (!input_) { LOGE() << "DetectionWorker: no input queue"; running_ = false; return; }
    LOGI() << "DetectionWorker: started";
    while (running_) {
        auto item = input_->popFor(std::chrono::milliseconds(cfg_.popTimeoutMs));
        if (!item) {
            if (input_->isClosed() && input_->empty()) break;   // drained + closed -> exit
            continue;                                           // timeout -> re-check running_
        }
        process(*item);
    }
    LOGI() << "DetectionWorker: stopped (frames=" << framesProcessed_
           << ", plates=" << platesEmitted_ << ")";
}

long DetectionWorker::nowSteadyMs() {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PHYSICAL size-trend only (no per-camera polarity): 0 unknown, 1 approaching
// (plate growing), 2 receding (shrinking). The backend maps this to entry/exit.
int DetectionWorker::physicalDirection(const std::string& gate, const std::string& /*text*/) {
    const int trend = dir_.current(gate);   // gate-keyed: accumulates all of the pass's movement
    if (trend == DirectionEstimator::Approaching) return 1;
    if (trend == DirectionEstimator::Receding)   return 2;
    return 0;
}

// A stable per-pass UUID for "the same vehicle across a few consecutive frames",
// keyed by GATE (not gate+text) so OCR flicker between the early announce and the
// settled correction keeps ONE id — otherwise a changed reading would mint a new
// passId and the backend would insert a duplicate instead of correcting in place.
// A new pass starts on a quiet gap OR when a clearly different plate appears (so
// two distinct vehicles are never merged). No vehicle-tracking model is needed.
std::string DetectionWorker::passIdFor(const std::string& gate, const std::string& text, long nowMs) {
    // Prune passes not seen for over 5 minutes so the map can't grow unbounded.
    for (auto it = passes_.begin(); it != passes_.end();) {
        if (nowMs - it->second.lastSeenMs > 5L * 60 * 1000) it = passes_.erase(it); else ++it;
    }
    PassState& st = passes_[gate];
    // Cadence-aware gap: a real "vehicle left" gap must exceed passGapMs AND be well
    // beyond the recent inter-sighting interval, so a SLOW detection cadence (interval
    // > passGapMs) doesn't mint a new passId every frame — which would stop the backend
    // from merging the early event with its correction.
    bool fresh = st.passId.empty();
    if (!fresh) {
        const long gap = nowMs - st.lastSeenMs;
        const long hardGap = 4 * cfg_.passGapMs;          // beyond ANY cadence -> new pass
        if (gap > hardGap) fresh = true;
        else if (st.lastInterval > 0 && gap > 3 * st.lastInterval) fresh = true;
        else st.lastInterval = gap;   // first gap (or in-cadence) -> learn it, don't reset
    }
    const bool differentPlate = !fresh && !st.text.empty() && !text.empty() &&
        jaroWinklerDistance(st.text, text) < cfg_.passPlateSimilarity;
    if (fresh || differentPlate) {
        st.passId = generateUuidV4();
        st.emittedEarly = false;
        st.lastSentDir = -1;
        st.lastInterval = 0;
    }
    if (!text.empty()) st.text = text;   // keep the reference; an empty (no-OCR) frame must
                                         // NOT erase it or the next differentPlate check fails
    st.lastSeenMs = nowMs;
    return st.passId;
}

void DetectionWorker::emitPlate(PlateItem&& out) {
    ++platesEmitted_;
    if (sink_) sink_(std::move(out));
}

void DetectionWorker::process(FrameItem& item) {
    ++framesProcessed_;
    if (item.image.empty()) return;

    // Periodic throughput so it's clear frames are reaching the queue (every 100 frames).
    if (framesProcessed_ % 100 == 0)
        LOGI() << "DetectionWorker: processed " << framesProcessed_ << " frames, "
               << platesEmitted_ << " plates so far";

    // Per-frame collaborators (recording, live view) see the frame first. Guard each so a
    // misbehaving observer can't take down the worker, and log it.
    for (auto& obs : observers_) {
        try { obs(item); }
        catch (const std::exception& e) { LOGE() << "DetectionWorker: frame observer threw: " << e.what(); }
        catch (...)                     { LOGE() << "DetectionWorker: frame observer threw (unknown)"; }
    }

    // Detect + OCR once; we reuse the results for both the debug preview and the sink.
    std::vector<PlateResult> results;
    try {
        results = recognizer_.recognize(item.image, item.gate, item.timestamp);
    } catch (const std::exception& e) {
        LOGE() << "DetectionWorker[" << item.gate << "]: recognition error: " << e.what();
    } catch (...) {
        LOGE() << "DetectionWorker[" << item.gate << "]: recognition error (unknown)";
    }

    // Hand the newest annotations to the live stream (drawn onto the continuously-fed live frames
    // by LiveOverlay::draw). Done for EVERY frame, independent of show_live, so the dashboard live
    // view shows vehicle boxes / plate boxes / count - not just the local preview window.
    const std::vector<VehicleAnnotation> vehicles = recognizer_.lastVehicles();
    const int vehicleCount = recognizer_.totalVehicleCount();
    if (overlay_) {
        std::vector<LiveOverlay::Plate> plates;
        plates.reserve(results.size());
        for (const PlateResult& r : results) plates.push_back({ r.box, r.text, r.plateImage });
        overlay_->publish(item.gate, vehicles, std::move(plates), vehicleCount);
    }
    LOGD() << "DetectionWorker[" << item.gate << "]: vehicles=" << vehicles.size()
           << " plates=" << results.size() << " count=" << vehicleCount;

    // Debug preview (original 'show_live'): draw the recognition polygon, each plate box and its
    // text on a copy of the frame, then display it in a per-gate window.
    if (cfg_.showLive) {
        try {
            cv::Mat canvas = item.image.clone();
            if (roiPolygon_) {
                std::vector<cv::Point> poly = roiPolygon_(item.gate, item.image.cols, item.image.rows);
                if (poly.size() >= 3)
                    cv::polylines(canvas, poly, true, cv::Scalar(255, 0, 0), 2);
            }

            // Vehicle-detection overlay: box every detected vehicle (cyan), label its track id,
            // show a thumbnail of the largest vehicle crop (top-right), and a running count.
            const cv::Rect frameRect(0, 0, canvas.cols, canvas.rows);
            const cv::Rect* biggest = nullptr;
            for (const VehicleAnnotation& v : vehicles) {
                cv::Rect b = v.box & frameRect;
                if (b.width < 2 || b.height < 2) continue;
                cv::rectangle(canvas, b, cv::Scalar(255, 200, 0), 2);   // cyan-ish vehicle box
                if (v.trackId >= 0) {
                    const std::string id = "#" + std::to_string(v.trackId);
                    cv::putText(canvas, id, {b.x, std::max(0, b.y) + 18},
                                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 4);
                    cv::putText(canvas, id, {b.x, std::max(0, b.y) + 18},
                                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 200, 0), 2);
                }
                if (!biggest || b.area() > biggest->area()) biggest = &v.box;
            }

            for (const PlateResult& r : results) {
                cv::Point2f pts[4]; r.box.points(pts);
                for (int i = 0; i < 4; ++i) cv::line(canvas, pts[i], pts[(i + 1) % 4], cv::Scalar(0, 255, 0), 2);
                cv::Point org(static_cast<int>(r.box.center.x - r.box.size.width / 2),
                              static_cast<int>(r.box.center.y - r.box.size.height / 2) - 6);
                cv::putText(canvas, r.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 0), 4);
                cv::putText(canvas, r.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
            }

            // Cropped-vehicle thumbnail (largest vehicle) pinned to the top-right corner.
            if (biggest) {
                cv::Rect b = *biggest & frameRect;
                if (b.width >= 2 && b.height >= 2) {
                    const int tw = 220;
                    const int th = std::max(1, b.height * tw / std::max(1, b.width));
                    cv::Mat thumb; cv::resize(canvas(b), thumb, cv::Size(tw, th));
                    cv::Rect dst(canvas.cols - tw - 10, 10, tw, th);
                    dst &= frameRect;
                    if (dst.width == tw && dst.height == th) {
                        thumb.copyTo(canvas(dst));
                        cv::rectangle(canvas, dst, cv::Scalar(255, 200, 0), 2);
                    }
                }
            }

            // Live vehicle count (top-left). totalVehicleCount() is -1 outside vehicle mode.

            if (vehicleCount >= 0) {
                const std::string label = "Vehicles: " + std::to_string(vehicleCount);
                cv::putText(canvas, label, {12, 30}, cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 0, 0), 5);
                cv::putText(canvas, label, {12, 30}, cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 255, 0), 2);
            }

            cv::Mat view = canvas;
            if (cfg_.liveScale > 1) cv::resize(canvas, view, canvas.size() / cfg_.liveScale);
            const std::string win = item.gate.empty() ? "live" : item.gate;
            cv::namedWindow(win, cv::WINDOW_NORMAL);   // resizable window, as in the original
            cv::imshow(win, view);
            cv::waitKey(1);
        } catch (const cv::Exception& e) { LOGW() << "DetectionWorker: show_live draw failed: " << e.what(); }
    }

    // Build the evidence/full image once for this frame: stamp the PC date/time (top-left) and
    // draw the detected plate boxes + text, so the operator's evidence frame shows what was read
    // and when. Shared by all plates kept from this frame. Toggle with 'evidence_overlay'.
    cv::Mat evidence = item.evidenceImage.empty() ? item.image : item.evidenceImage;
    if (cfg_.annotateEvidence && !results.empty() && !evidence.empty()) {
        cv::Mat canvas = evidence.clone();
        for (const PlateResult& r : results) {
            cv::Point2f pts[4]; r.box.points(pts);
            for (int i = 0; i < 4; ++i)
                cv::line(canvas, pts[i], pts[(i + 1) % 4], cv::Scalar(0, 255, 0), 2);
            cv::Point org(static_cast<int>(r.box.center.x - r.box.size.width / 2),
                          static_cast<int>(r.box.center.y - r.box.size.height / 2) - 6);
            cv::putText(canvas, r.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 0), 4);
            cv::putText(canvas, r.text, org, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        // PC date/time, top-left (white with a black outline), sized to the frame width.
        const std::string ts = formatLocalTime("%Y-%m-%d %H:%M:%S");
        const double tscale = std::max(1.0, canvas.cols / 1200.0);
        const int    tth    = std::max(2, (int)std::lround(tscale * 2));
        const int    tY     = (int)std::lround(tscale * 28);
        cv::putText(canvas, ts, {12, tY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(0, 0, 0), tth * 2);
        cv::putText(canvas, ts, {12, tY}, cv::FONT_HERSHEY_SIMPLEX, tscale, cv::Scalar(255, 255, 255), tth);

        // Inset each plate's cropped picture as a zoomed thumbnail down the top-right, so the
        // evidence frame shows a readable close-up of the plate, not just the box.
        int insetY = 12;
        const int insetH = 70, margin = 12;
        for (const PlateResult& r : results) {
            if (r.plateImage.empty() || insetY + insetH + 4 >= canvas.rows) continue;
            cv::Mat thumb = r.plateImage;
            if (thumb.channels() == 1) cv::cvtColor(thumb, thumb, cv::COLOR_GRAY2BGR);
            const int w = std::max(1, (int)std::lround(thumb.cols * (double)insetH / thumb.rows));
            cv::Mat resized; cv::resize(thumb, resized, cv::Size(w, insetH));
            const int x = std::max(0, canvas.cols - w - margin);
            cv::Rect dst(x, insetY, std::min(w, canvas.cols - x), insetH);
            resized(cv::Rect(0, 0, dst.width, dst.height)).copyTo(canvas(dst));
            cv::rectangle(canvas, dst, cv::Scalar(0, 255, 255), 2);
            insetY += insetH + 8;
        }
        evidence = canvas;
        LOGI() << "DetectionWorker[" << item.gate << "]: evidence annotated time='" << ts
               << "' plate_boxes=" << results.size();
    }

    // Infer enter/exit from how the plate's apparent size changes across sightings (low,
    // plate-facing camera). Feed every raw detection once per frame; tag emitted plates with
    // the committed direction. Uses a steady-clock ms timestamp so gap/cooldown are wall-time.
    const long nowMs = nowSteadyMs();
    if (cfg_.directionEnable && !results.empty()) {
        // Per-camera polarity: does a plate approaching the camera mean ENTER? (Entry_Exit=0)
        const bool approachEnter = cfg_.approachingIsEnter ? cfg_.approachingIsEnter(item.gate) : true;
        // One gate-keyed track per pass: feed the estimator ONCE per frame with the
        // LARGEST plate (the nearest, realest vehicle), so every sighting of the pass
        // — regardless of OCR flicker — accumulates in a single track. More movement
        // samples => a more accurate, refine-able enter/exit decision.
        const PlateResult* best = nullptr; double bestSz = 0.0;
        for (const PlateResult& r : results) {
            if (r.text.empty()) continue;
            const double sz = std::sqrt(std::max(0.0f, r.box.size.width * r.box.size.height));
            if (sz > bestSz) { bestSz = sz; best = &r; }
        }
        int event = DirectionEstimator::Unknown;
        if (best && bestSz > 0.0)
            event = dir_.update(item.gate, bestSz, best->box.center.y, nowMs); // non-Unknown on announce/correction
        const int standing = dir_.current(item.gate);
        int direction = 0;   // 0 unknown, 1 enter, 2 exit
        if (standing == DirectionEstimator::Approaching) direction = approachEnter ? 1 : 2;
        else if (standing == DirectionEstimator::Receding) direction = approachEnter ? 2 : 1;
        if (event != DirectionEstimator::Unknown && best) { // fire the enter/exit log on announce/correction
            // Label a first announce vs a later CORRECTION/flip, and include the
            // evidence that drove it (plate size, how many sightings accumulated,
            // whether the trend is locked) so a wrong enter/exit is debuggable.
            auto lit = lastLoggedDir_.find(item.gate);
            const bool isFirst = (lit == lastLoggedDir_.end());
            const bool isFlip  = (!isFirst && lit->second != direction);
            const char* kind = isFirst ? "announce" : (isFlip ? "CORRECTION(flip)" : "reaffirm");
            lastLoggedDir_[item.gate] = direction;
            const int sightings = dir_.sightingCount(item.gate);
            const bool locked = dir_.isLocked(item.gate);
            LOGI() << "DetectionWorker[" << item.gate << "]: " << best->text << " " << kind << " "
                   << (event == DirectionEstimator::Approaching ? "approaching" : "receding")
                   << " -> " << (direction == 1 ? "ENTER" : "EXIT")
                   << " size=" << (long)std::lround(bestSz)
                   << " sightings=" << sightings
                   << " locked=" << (locked ? 1 : 0)
                   << " (Entry_Exit polarity approach=" << (approachEnter ? "enter" : "exit") << ")";
            // Publish a diagnostic ONLY on a real direction flip (a corrected
            // enter/exit) — the notable case worth surfacing in the panel. Best-
            // effort: the diag sink core-publishes to messages.module_diag.
            if (diag_ && isFlip) {
                nlohmann::json j = {
                    {"kind", "direction"}, {"event", "flip"},
                    {"gate", item.gate}, {"plate", best->text},
                    {"direction", direction == 1 ? "ENTER" : "EXIT"},
                    {"trend", event == DirectionEstimator::Approaching ? "approaching" : "receding"},
                    {"size", (long)std::lround(bestSz)},
                    {"sightings", sightings}, {"locked", locked},
                    {"message", best->text + std::string(" جهت اصلاح شد -> ") + (direction == 1 ? "ENTER" : "EXIT")},
                };
                if (best->trackId >= 0) j["track_id"] = std::to_string(best->trackId);
                diag_(j.dump());
            }
        }
    }

    // Run each recognized plate through the processor (dedup/validate/tag) and sink the survivors.
    for (PlateResult& p : results) {
        const std::string ptext = p.text;             // keep the read before the processor consumes it
        std::optional<PlateResult> kept =
            processor_ ? processor_(std::move(p))
                       : std::optional<PlateResult>(std::move(p));
        if (!kept) {
            // The processor suppressed this read as a duplicate of an already-emitted pass.
            // That is EXACTLY the frame on which a race-emitted pass tends to get its SETTLED
            // direction: the early announce already went out (often «unknown»), and the OCR
            // then de-dups every later read -- so the direction CORRECTION, which used to live
            // only on the kept path below, never fired and the passage stayed «unknown». Emit
            // it here too, from the gate-keyed pass state, so the backend back-fills the row in
            // place (same passId). Guarded to the SAME plate so two interleaved vehicles are
            // never cross-corrected.
            if (!cfg_.directionEnable) continue;
            PassState& st = passes_[item.gate];
            const int phys = physicalDirection(item.gate, ptext);
            const bool samePass = st.text.empty() || ptext.empty() ||
                jaroWinklerDistance(st.text, ptext) >= cfg_.passPlateSimilarity;
            // (0) RELEASE A HELD EARLY ANNOUNCE. announce-hold stashes the first announce until
            //     the direction settles; because the processor de-dups every later read of the
            //     SAME pass, this de-dup path is the ONLY place a slow pass is re-seen -- so the
            //     held plate MUST be freed here or it is read but never recorded. Release the
            //     moment the direction settles, or when the hold window (announceHoldSightings,
            //     counted in REAL sightings which grow every frame) elapses -- announced as
            //     «unknown» rather than dropped. Same-plate guarded.
            if (st.pendingEarly && samePass) {
                const int sightings = dir_.sightingCount(item.gate);
                const bool settled  = (phys != 0);
                const bool expired  = (sightings - st.pendingSince) >= cfg_.direction.announceHoldSightings;
                if (settled || expired) {
                    st.lastSeenMs = nowMs;                 // this sighting keeps the pass alive
                    st.pendingItem.plate.direction = phys; // settled trend, or 0/unknown on expiry
                    st.emittedEarly = true;
                    st.lastSentDir  = phys;
                    st.pendingEarly = false;
                    emitPlate(std::move(st.pendingItem));  // the announce that was held, now freed
                }
                continue;
            }
            if (st.pendingEarly && !samePass) {
                // A different vehicle holds the gate before the held one settled: flush the held
                // plate as «unknown» so it is still recorded, then let this read proceed.
                st.pendingItem.plate.direction = 0;
                st.pendingEarly = false;
                emitPlate(std::move(st.pendingItem));
            }
            if (st.emittedEarly && samePass && !st.passId.empty() &&
                phys != 0 && phys != st.lastSentDir) {
                st.lastSeenMs = nowMs;                 // this sighting keeps the pass alive
                PlateItem corr;
                corr.image           = evidence;
                corr.gate            = item.gate;
                corr.timestamp       = item.timestamp;
                corr.plate.text      = ptext.empty() ? st.text : ptext;
                corr.plate.passId    = st.passId;      // SAME pass -> backend updates in place
                corr.plate.direction = phys;
                corr.forceSend       = true;           // bypass the per-plate cooldown
                st.lastSentDir       = phys;
                emitPlate(std::move(corr));
            }
            continue;                                  // processor dropped it (dup / invalid)
        }

        PlateItem out;
        // Detection ran on item.image (mono in a pair); the evidence/full_image is the RGB
        // companion frame when present, else the same detection frame (single camera).
        out.image     = evidence;
        out.plate     = std::move(*kept);
        out.gate      = item.gate;
        out.timestamp = item.timestamp;

        // Early-announce + correction, correlated by a stable per-pass id:
        //   * FIRST sighting of a pass  -> emit immediately so the owner is announced
        //     ASAP; direction is the best physical trend so far (often 0/unknown yet).
        //   * later sighting where the physical direction has settled/changed -> emit a
        //     CORRECTION (same passId, forceSend to bypass the cooldown); the backend
        //     updates the SAME passage row instead of creating a new one.
        //   * otherwise (already announced, direction unchanged) -> suppress, no flood.
        // A pass seen too few times never commits a direction, so it stays 0/unknown
        // and the backend records it as «نامشخص» (not a wrong exit). The module sends
        // the PHYSICAL trend only; the backend applies per-camera Entry/Exit polarity.
        out.plate.passId = passIdFor(item.gate, out.plate.text, nowMs);   // manages passes_[gate]
        const int phys = cfg_.directionEnable ? physicalDirection(item.gate, out.plate.text) : 0;
        PassState& st = passes_[item.gate];   // SAME gate-keyed pass state passIdFor maintains
        // Two vehicles interleaving at the gate: if a DIFFERENT vehicle's early announce is still
        // held (pendingEarly), it MUST be flushed before this one is stashed — otherwise the
        // announce-hold stash below (`st.pendingItem = out`) overwrites and silently loses it, so
        // that car is read but never recorded. Record it as «unknown» (its direction never
        // settled), mirroring the same guard on the de-dup path above.
        if (st.pendingEarly && !st.pendingItem.plate.text.empty() && !out.plate.text.empty() &&
            jaroWinklerDistance(st.pendingItem.plate.text, out.plate.text) < cfg_.passPlateSimilarity) {
            st.pendingItem.plate.direction = 0;   // never settled -> unknown, but still recorded
            st.pendingEarly = false;
            emitPlate(std::move(st.pendingItem));
        }
        if (!st.emittedEarly) {
            // Announce-hold: wait for the direction to SETTLE before the first announce, so
            // the first stored row already carries entry/exit instead of «unknown». Bounded
            // by announceHoldSightings kept reads so a genuinely short/undecidable pass is
            // still announced (as unknown) and never lost. 0 => announce immediately (legacy).
            const int hold = cfg_.directionEnable ? cfg_.direction.announceHoldSightings : 0;
            if (hold > 0 && phys == 0) {
                // Hold the first announce for the direction to settle -- but STASH the plate so a
                // later (de-duped) frame can still release it. The old code `continue`d and
                // dropped it, relying on a future KEPT read; the processor de-dups every later
                // read of the pass, so at low speed that read never comes and the plate was read
                // but never recorded. The de-dup path above frees this within the hold window.
                if (!st.pendingEarly)
                    st.pendingSince = cfg_.directionEnable ? dir_.sightingCount(item.gate) : 0;
                st.pendingEarly = true;
                st.pendingItem  = std::move(out);
                continue;
            }
            out.plate.direction = phys;
            st.emittedEarly = true;
            st.lastSentDir   = phys;
            st.pendingEarly  = false;
            emitPlate(std::move(out));                      // EARLY (now usually with a settled direction)
        } else if (phys != 0 && phys != st.lastSentDir) {
            out.plate.direction = phys;
            out.forceSend       = true;                     // CORRECTION (bypass cooldown)
            st.lastSentDir      = phys;
            emitPlate(std::move(out));
        }
        // else: already announced with the same direction -> nothing to send.
    }
}

} // namespace lpr
