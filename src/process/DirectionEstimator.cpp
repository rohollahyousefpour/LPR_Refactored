#include "lpr/process/DirectionEstimator.h"

#include <algorithm>
#include <cmath>

namespace lpr {

double DirectionEstimator::avgFirst(const std::deque<double>& d) const {
    // Cap the window at half the history so the first/last windows never overlap (otherwise at
    // exactly minSightings the two averages would be identical and never cross the threshold).
    const int n = std::max(1, std::min((int)cfg_.window, (int)d.size() / 2));
    double s = 0; for (int i = 0; i < n; ++i) s += d[i];
    return s / n;
}

double DirectionEstimator::avgLast(const std::deque<double>& d) const {
    const int n = std::max(1, std::min((int)cfg_.window, (int)d.size() / 2));
    double s = 0; for (int i = 0; i < n; ++i) s += d[d.size() - 1 - i];
    return s / n;
}

int DirectionEstimator::current(const std::string& key) const {
    auto it = tracks_.find(key);
    return it == tracks_.end() ? Unknown : it->second.reported;
}

int DirectionEstimator::update(const std::string& key, double sizePx, double yCenterPx, long tsMs) {
    Track& t = tracks_[key];

    // Start a fresh pass if the plate has been gone a while, or a FINALIZED decision's
    // cooldown has elapsed (so the same plate driving back out later is judged afresh).
    if (t.lastTs != 0 && tsMs - t.lastTs > cfg_.trackGapMs)
        t = Track{};
    else if (t.locked && tsMs - t.decidedTs > cfg_.cooldownMs)
        t = Track{};
    t.lastTs = tsMs;

    if (t.locked) return Unknown;                        // finalized this pass -> no repeat events

    if (sizePx > 0) { t.sizes.push_back(sizePx); t.ys.push_back(yCenterPx); }
    if ((int)t.sizes.size() < cfg_.minSightings) return Unknown;

    const double s0 = avgFirst(t.sizes), s1 = avgLast(t.sizes);
    if (s0 <= 0) return Unknown;
    const double ratio = s1 / s0;

    int dir = Unknown;
    if (ratio >= cfg_.minGrowthRatio)            dir = Approaching;   // plate grew -> approaching
    else if (ratio <= 1.0 / cfg_.minGrowthRatio) dir = Receding;    // plate shrank -> receding
    else if ((int)t.sizes.size() >= cfg_.trendMinSightings &&
             std::fabs(s1 - s0) >= cfg_.minTrendDeltaPx) {
        // Slow / stopping vehicle: the size barely changes so the ratio never crosses
        // minGrowthRatio, but with this many consistent sightings a small net size
        // change is a reliable trend. (A symmetric approach-then-leave nets ~0 px and
        // stays Unknown, so a car that merely pauses in view is not mislabeled.)
        dir = (s1 > s0) ? Approaching : Receding;
    }

    if (dir != Unknown && cfg_.requireYAgree) {
        const double dy = avgLast(t.ys) - avgFirst(t.ys);   // low camera: approaching drifts DOWN (+y)
        if (dir == Approaching && dy <  cfg_.minYShiftPx) dir = Unknown;
        if (dir == Receding  && dy > -cfg_.minYShiftPx) dir = Unknown;
    }

    if (dir == Unknown) return Unknown;                  // not enough signal yet -> keep watching

    // Finalize once enough movement confirms the trend, so a late noisy frame can't
    // flip a settled call. Until then keep refining: a stronger or opposite trend from
    // MORE sightings can still correct the early guess.
    if ((int)t.sizes.size() >= cfg_.confirmSightings) { t.locked = true; t.decidedTs = tsMs; }

    if (dir != t.reported) { t.reported = dir; return dir; }   // first announce OR a correction
    return Unknown;                                            // unchanged this pass -> no event
}

} // namespace lpr
