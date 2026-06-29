#include "lpr/process/DirectionEstimator.h"

#include <algorithm>

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
    return it == tracks_.end() ? Unknown : it->second.decided;
}

int DirectionEstimator::update(const std::string& key, double sizePx, double yCenterPx, long tsMs) {
    Track& t = tracks_[key];

    // Start a fresh pass if the plate has been gone a while, or a previous decision's cooldown
    // has elapsed (so the same plate driving back out later is judged independently).
    if (t.lastTs != 0 && tsMs - t.lastTs > cfg_.trackGapMs)
        t = Track{};
    else if (t.decided != Unknown && tsMs - t.decidedTs > cfg_.cooldownMs)
        t = Track{};
    t.lastTs = tsMs;

    if (t.decided != Unknown) return Unknown;            // already committed this pass -> no repeat event

    if (sizePx > 0) { t.sizes.push_back(sizePx); t.ys.push_back(yCenterPx); }
    if ((int)t.sizes.size() < cfg_.minSightings) return Unknown;

    const double s0 = avgFirst(t.sizes), s1 = avgLast(t.sizes);
    if (s0 <= 0) return Unknown;
    const double ratio = s1 / s0;

    int dir = Unknown;
    if (ratio >= cfg_.minGrowthRatio)            dir = Approaching;   // plate grew -> approaching
    else if (ratio <= 1.0 / cfg_.minGrowthRatio) dir = Receding;    // plate shrank -> receding

    if (dir != Unknown && cfg_.requireYAgree) {
        const double dy = avgLast(t.ys) - avgFirst(t.ys);   // low camera: approaching drifts DOWN (+y)
        if (dir == Approaching && dy <  cfg_.minYShiftPx) dir = Unknown;
        if (dir == Receding  && dy > -cfg_.minYShiftPx) dir = Unknown;
    }

    if (dir != Unknown) { t.decided = dir; t.decidedTs = tsMs; }
    return dir;
}

} // namespace lpr
