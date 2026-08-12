#pragma once
// Pure decision for the live GigE frame-rate adaptation, factored out of the
// capture loop so it can be unit-tested / demoed without a camera.
//
// Rule (per evaluation window):
//   * loss >= downPct  and  cur > minFps        -> THROTTLE down: cur * downFactor (>= minFps)
//   * loss <= upPct    and  cur < ceil          -> after recoverMs of clean link,
//                                                   RECOVER up: cur * upFactor (<= ceil)
//   * otherwise                                 -> no change
#include <algorithm>

namespace lpr {

struct GigeAdaptCfg {
    double downPct   = 2.0;    // loss% at/above which to throttle down
    double upPct     = 0.2;    // loss% at/below which the link counts as clean
    double downF     = 0.75;   // throttle-down step multiplier
    double upF       = 1.15;   // recovery step multiplier
    double minFps    = 1.0;    // floor
    long long recoverMs = 15000;  // clean time before a recovery step
};

enum class GigeAdaptAction { None, Throttle, Recover };

struct GigeAdaptDecision {
    GigeAdaptAction action = GigeAdaptAction::None;
    double          newFps = 0.0;
};

// `cur` = current fps, `ceil` = connect-time cap, `lastStepMs` = when the last step
// was applied, `lossPct` = measured loss over the window, `nowMs` = steady clock ms.
inline GigeAdaptDecision gigeAdaptDecide(double cur, double ceil, long long lastStepMs,
                                         double lossPct, long long nowMs, const GigeAdaptCfg& c) {
    if (lossPct >= c.downPct && cur > c.minFps)
        return { GigeAdaptAction::Throttle, std::max(c.minFps, cur * c.downF) };
    if (lossPct <= c.upPct && cur < ceil && (nowMs - lastStepMs) >= c.recoverMs)
        return { GigeAdaptAction::Recover, std::min(ceil, cur * c.upF) };
    return { GigeAdaptAction::None, cur };
}

} // namespace lpr
