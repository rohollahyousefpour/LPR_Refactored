#include "lpr/process/DirectionEstimator.h"

#include <algorithm>
#include <cmath>

namespace lpr {

// Averaging window at each end of the pass. It GROWS with the number of sightings
// (a quarter of them, floored at cfg_.window), so a long slow/stopping pass with MANY
// frames averages the trend over a wide baseline at each end — robust to per-frame
// box jitter instead of being read from just a couple of frames. Capped at half the
// history so the first and last windows never overlap.
static int trendWindow(int window, int size) {
    const int half = std::max(1, size / 2);
    return std::clamp(std::max(window, size / 4), 1, half);
}

double DirectionEstimator::avgFirst(const std::deque<double>& d) const {
    const int n = trendWindow((int)cfg_.window, (int)d.size());
    double s = 0; for (int i = 0; i < n; ++i) s += d[i];
    return s / n;
}

double DirectionEstimator::avgLast(const std::deque<double>& d) const {
    const int n = trendWindow((int)cfg_.window, (int)d.size());
    double s = 0; for (int i = 0; i < n; ++i) s += d[d.size() - 1 - i];
    return s / n;
}

int DirectionEstimator::current(const std::string& key) const {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? Unknown : it->second.reported;
}

int DirectionEstimator::sightingCount(const std::string& key) const {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? 0 : static_cast<int>(it->second.sizes.size());
}

bool DirectionEstimator::isLocked(const std::string& key) const {
    auto it = tracks_.find(key);
    return it != tracks_.end() && it->second.locked;
}

int DirectionEstimator::update(const std::string& key, double sizePx, double yCenterPx, long tsMs) {
    Track& t = tracks_[key];

    // Start a fresh pass if the plate has been gone a while, or a FINALIZED decision's
    // cooldown has elapsed (so the same plate driving back out later is judged afresh).
    // Cadence-aware "new pass" reset: a real gap must exceed the configured trackGapMs
    // AND be well beyond the recent inter-sighting interval — so a genuinely SLOW
    // detection cadence (interval > trackGapMs) does NOT reset the track on every frame
    // (which would leave it stuck at «unknown» forever). The cooldown reset is separate.
    bool reset = false;
    if (t.lastTs != 0) {
        const long gap = tsMs - t.lastTs;
        const long hardGap = 4 * (long)cfg_.trackGapMs;   // beyond ANY cadence -> new pass
        if (gap > hardGap) reset = true;
        else if (t.lastInterval > 0 && gap > 3 * t.lastInterval) reset = true;
        else t.lastInterval = gap;   // first gap (or in-cadence) -> learn it, don't reset
    }
    if (!reset && t.locked && tsMs - t.decidedTs > cfg_.cooldownMs) reset = true;
    if (reset) t = Track{};
    t.lastTs = tsMs;

    if (t.locked) return Unknown;                        // finalized this pass -> no repeat events

    if (sizePx > 0) {
        t.sizes.push_back(sizePx); t.ys.push_back(yCenterPx);
        // Bound memory for a vehicle idling in view with no gap reset. Drop the oldest;
        // the trend baseline stays wide (kMaxSamples/4 at each end via trendWindow).
        constexpr size_t kMaxSamples = 600;
        if (t.sizes.size() > kMaxSamples) { t.sizes.pop_front(); t.ys.pop_front(); }
    }
    if ((int)t.sizes.size() < cfg_.minSightings) return Unknown;

    const double s0 = avgFirst(t.sizes), s1 = avgLast(t.sizes);
    if (s0 <= 0) return Unknown;
    const double ratio = s1 / s0;

    // A Receding (exit) call must clear a HIGHER bar than Approaching (recedingBias >= 1):
    // the shrink ratio, the slow-path net delta and the peak offset are all scaled up, so
    // size-noise on a short pass at a one-directional gate can't fake an exit.
    const double recBias  = std::max(1.0, cfg_.recedingBias);
    const double recRatio = 1.0 / (cfg_.minGrowthRatio * recBias);   // <= this => receding

    int dir = Unknown;
    if (ratio >= cfg_.minGrowthRatio) dir = Approaching;             // plate grew -> approaching
    else if (ratio <= recRatio)       dir = Receding;               // plate shrank enough -> receding
    else if ((int)t.sizes.size() >= cfg_.trendMinSightings &&
             std::fabs(s1 - s0) >= cfg_.minTrendDeltaPx) {
        // Slow / stopping vehicle: the size barely changes so the ratio never crosses
        // minGrowthRatio, but with this many consistent sightings a small net size
        // change is a reliable trend. (A symmetric approach-then-leave nets ~0 px and
        // stays Unknown, so a car that merely pauses in view is not mislabeled.)
        if (s1 > s0) dir = Approaching;
        else if (std::fabs(s1 - s0) >= cfg_.minTrendDeltaPx * recBias) dir = Receding;
    }

    // Peak-position fallback: the first-vs-last averages can wash out a clear
    // trajectory (e.g. a fast road pass caught over a few frames where the last
    // window happens to dip). Decide instead from WHERE the LARGEST plate sits in
    // the pass — still growing => peak LATE => approaching; already shrinking =>
    // peak EARLY => receding. Uses ALL reads; guarded by a real size spread and a
    // clearly off-centre peak so a flat/noisy or symmetric pass stays Unknown.
    if (dir == Unknown && (int)t.sizes.size() >= cfg_.minSightings) {
        size_t maxIdx = 0;
        double mx = t.sizes[0], mn = t.sizes[0];
        for (size_t i = 1; i < t.sizes.size(); ++i) {
            if (t.sizes[i] > mx) { mx = t.sizes[i]; maxIdx = i; }
            if (t.sizes[i] < mn) mn = t.sizes[i];
        }
        // Only an INTERIOR peak (not the first or last reading): a peak still at the
        // END is a monotonic trend the ratio/net-delta already own — firing here would
        // just pre-empt them (and their slow-vehicle certainty). An interior peak means
        // the plate grew to its closest then the tail-off dropped, washing out the
        // first-vs-last averages — exactly the fast-pass case this rescues.
        const bool interior = maxIdx > 0 && maxIdx + 1 < t.sizes.size();
        if (interior && mn > 0 && mx / mn >= cfg_.peakMinSpread) {
            const double rel = (double)maxIdx / (double)(t.sizes.size() - 1);  // 0..1
            if      (rel >= 0.5 + cfg_.peakBias)           dir = Approaching;
            else if (rel <= 0.5 - cfg_.peakBias * recBias) dir = Receding;   // stricter early-peak -> exit
        }
    }

    // Extra guard for exits: a Receding call needs at least recedingMinSightings reads
    // (when configured above minSightings), so a very short noisy pass can't emit an exit.
    if (dir == Receding && cfg_.recedingMinSightings > cfg_.minSightings &&
        (int)t.sizes.size() < cfg_.recedingMinSightings) {
        dir = Unknown;
    }

    if (dir != Unknown && cfg_.requireYAgree) {
        const double dy = avgLast(t.ys) - avgFirst(t.ys);   // low camera: approaching drifts DOWN (+y)
        if (dir == Approaching && dy <  cfg_.minYShiftPx) dir = Unknown;
        if (dir == Receding  && dy > -cfg_.minYShiftPx) dir = Unknown;
    }

    if (dir == Unknown) return Unknown;                  // not enough signal yet -> keep watching

    // Certainty by agreement: count how many CONSECUTIVE images give the same direction.
    // A flip resets the streak, so the estimator keeps examining more images until several
    // in a row agree. It is finalized (locked) when the streak reaches confirmAgreeing, or
    // as a backstop once confirmSightings total reads accumulate (a persistently unstable
    // pass still ends). Until then it keeps refining, correcting the early guess.
    if (dir == t.pendingDir) ++t.agree; else { t.pendingDir = dir; t.agree = 1; }
    if (t.agree >= cfg_.confirmAgreeing) {
        // A confirmed streak wins and locks.
        t.locked = true; t.decidedTs = tsMs;
        if (dir != t.reported) { t.reported = dir; return dir; }
        return Unknown;
    }
    if ((int)t.sizes.size() >= cfg_.confirmSightings) {
        // Backstop cap: finalize on the already-reported STANDING decision if one exists —
        // a single late outlier that never formed a streak must NOT win. Only adopt the
        // current dir if nothing was ever reported this pass.
        t.locked = true; t.decidedTs = tsMs;
        if (t.reported != Unknown) return Unknown;
        t.reported = dir; return dir;
    }

    if (dir != t.reported) { t.reported = dir; return dir; }   // still refining -> correction
    return Unknown;                                            // unchanged this pass -> no event
}

} // namespace lpr
