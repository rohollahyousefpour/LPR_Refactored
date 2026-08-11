#pragma once
// DirectionEstimator - decides whether a vehicle is entering or exiting using ONLY plate
// detections, suited to a low-mounted, plate-facing camera at low frame rate where the full
// vehicle is not visible (so vehicle-box tracking is unreliable).
//
// Cue: a plate that grows in apparent size across successive sightings is approaching the
// camera/gate (ENTERING); one that shrinks is receding (EXITING). Optionally the vertical
// motion must agree (on a low camera an approaching plate also drifts DOWN the frame).
//
// Robust to sudden reversals at low fps: the decision needs a sustained net trend over several
// sightings (first-vs-last averaged), so a brief back-and-forth does not fire; each plate
// commits a direction once, then a cooldown lets a genuinely new pass be judged afresh.
#include <deque>
#include <map>
#include <string>

namespace lpr {

class DirectionEstimator {
public:
    // Physical motion of the plate relative to the camera. Mapping to enter/exit is a
    // per-camera concern (the Entry_Exit setting), applied by the caller.
    enum Trend { Unknown = 0, Approaching = 1, Receding = 2 };

    struct Config {
        int    minSightings   = 3;      // need >= this many reads before deciding
        double minGrowthRatio = 1.15;   // last/first plate-size ratio that counts as a trend
        long   trackGapMs     = 3000;   // gap since last sighting that starts a fresh track
        long   cooldownMs     = 8000;   // after a decision, ignore the same plate this long
        bool   requireYAgree  = false;  // also require vertical motion to agree
        double minYShiftPx    = 8.0;    // vertical-motion threshold when requireYAgree
        int    window         = 3;      // average first/last N sizes to fight box jitter
        // Slow / stopping-vehicle path: a low-speed vehicle changes the plate size
        // only a little (the ratio never reaches minGrowthRatio), but produces MANY
        // consistent sightings — so once at least this many reads accumulate, a net
        // size change of at least minTrendDeltaPx pixels is trusted as the trend
        // (a symmetric approach-then-leave nets ~0 and is still left Unknown).
        int    trendMinSightings = 6;
        double minTrendDeltaPx   = 5.0;
        // The decision keeps REFINING until at least this many sightings accumulate,
        // then it is finalized (locked) so a late noisy frame can't flip a settled
        // call. Larger = uses MORE movement before committing = more accurate, at the
        // cost of a slightly later final answer (the early guess is still announced
        // immediately and corrected as more movement confirms it).
        int    confirmSightings  = 8;
    };

    DirectionEstimator() = default;
    explicit DirectionEstimator(Config cfg) : cfg_(cfg) {}
    void setConfig(Config cfg) { cfg_ = cfg; }

    // Feed one plate sighting; returns the committed direction ONLY on the frame the decision is
    // first made (the transition) - Unknown before that, and Unknown again on every later frame of
    // the same pass - so the caller fires an enter/exit event exactly once per pass (a new pass,
    // after trackGap/cooldown, can fire again). Use current() for the standing decision when
    // annotating the later frames. `key` is a stable identity for the vehicle (plate text,
    // optionally gate-scoped). `sizePx` is the plate's apparent size in pixels (e.g. sqrt(box
    // area)); `yCenterPx` its vertical centre; `tsMs` a monotonic millisecond timestamp.
    int update(const std::string& key, double sizePx, double yCenterPx, long tsMs);

    // Direction already committed for a key without adding a sighting (0 if none / reset).
    int current(const std::string& key) const;

private:
    struct Track {
        std::deque<double> sizes;
        std::deque<double> ys;
        long lastTs    = 0;
        int  reported  = Unknown;   // last direction emitted this pass (refined over sightings)
        long decidedTs = 0;         // when the pass was finalized (drives the cooldown reset)
        bool locked    = false;     // enough movement confirmed the trend -> no more changes
    };
    Config cfg_;
    std::map<std::string, Track> tracks_;

    double avgFirst(const std::deque<double>& d) const;
    double avgLast(const std::deque<double>& d) const;
};

} // namespace lpr
