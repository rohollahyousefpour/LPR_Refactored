#include "lpr/process/DirectionEstimator.h"
#include "../lpr_check.hpp"

using lpr::DirectionEstimator;

int main() {
    DirectionEstimator::Config cfg;
    cfg.minSightings = 3; cfg.minGrowthRatio = 1.15; cfg.window = 1;
    cfg.trackGapMs = 3000; cfg.cooldownMs = 8000;
    cfg.confirmSightings = 3;   // most cases: finalize as soon as the trend is clear

    // Approaching plate (sizes grow) -> ENTER, committed once size trend is clear.
    {
        DirectionEstimator d(cfg);
        LPR_CHECK(d.update("g:ABC", 30, 100, 1000) == DirectionEstimator::Unknown);  // 1 sighting
        LPR_CHECK(d.update("g:ABC", 34, 110, 1100) == DirectionEstimator::Unknown);  // 2 sightings
        const int dir = d.update("g:ABC", 46, 130, 1200);                            // 3rd: 46/30=1.53
        LPR_CHECK(dir == DirectionEstimator::Approaching);
        // Fires exactly ONCE: later sightings in the same pass return Unknown (no repeat event),
        // while current() still reports the standing decision.
        LPR_CHECK(d.update("g:ABC", 60, 150, 1300) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:ABC", 75, 160, 1400) == DirectionEstimator::Unknown);
        LPR_CHECK(d.current("g:ABC") == DirectionEstimator::Approaching);
    }

    // A genuinely new pass (after the cooldown elapses) is allowed to fire again.
    {
        DirectionEstimator::Config cc = cfg; cc.cooldownMs = 1000; cc.trackGapMs = 100000;
        DirectionEstimator d(cc);
        d.update("g:RE", 30, 100, 1000);
        d.update("g:RE", 34, 110, 1100);
        LPR_CHECK(d.update("g:RE", 46, 130, 1200) == DirectionEstimator::Approaching); // first fire
        LPR_CHECK(d.update("g:RE", 60, 150, 1300) == DirectionEstimator::Unknown);     // same pass: silent
        // After cooldown the pass resets; a fresh growing run fires once more.
        d.update("g:RE", 30, 100, 3000);
        d.update("g:RE", 34, 110, 3100);
        LPR_CHECK(d.update("g:RE", 46, 130, 3200) == DirectionEstimator::Approaching); // new pass fires
    }

    // Receding plate (sizes shrink) -> EXIT.
    {
        DirectionEstimator d(cfg);
        d.update("g:XYZ", 60, 150, 1000);
        d.update("g:XYZ", 50, 140, 1100);
        LPR_CHECK(d.update("g:XYZ", 40, 120, 1200) == DirectionEstimator::Receding);   // 40/60=0.67
    }

    // Sudden reversal: grows a bit then returns to start -> net flat -> stays Unknown (no false fire).
    {
        DirectionEstimator d(cfg);
        d.update("g:REV", 30, 100, 1000);
        d.update("g:REV", 33, 105, 1100);
        LPR_CHECK(d.update("g:REV", 30, 100, 1200) == DirectionEstimator::Unknown);   // 30/30=1.0
    }

    // Gap longer than trackGapMs starts a fresh pass (old history discarded).
    {
        DirectionEstimator d(cfg);
        d.update("g:GAP", 30, 100, 1000);
        d.update("g:GAP", 60, 100, 1100);
        // big gap -> reset; a single new sighting can't decide yet
        LPR_CHECK(d.update("g:GAP", 40, 100, 9000) == DirectionEstimator::Unknown);
    }

    // requireYAgree: growth says ENTER but plate moved UP -> vetoed to Unknown.
    {
        DirectionEstimator::Config yc = cfg; yc.requireYAgree = true; yc.minYShiftPx = 8;
        DirectionEstimator d(yc);
        d.update("g:Y", 30, 200, 1000);
        d.update("g:Y", 34, 190, 1100);
        LPR_CHECK(d.update("g:Y", 46, 180, 1200) == DirectionEstimator::Unknown);     // grew but moved up
    }

    // With the DEFAULT smoothing window (3), the decision must still land at min_sightings (3),
    // not later — the first/last windows are capped to half the history so they don't overlap.
    {
        DirectionEstimator::Config dc;   // defaults: minSightings=3, window=3, growth=1.15
        DirectionEstimator d(dc);
        LPR_CHECK(d.update("g:DEF", 30, 100, 1000) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:DEF", 38, 110, 1100) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:DEF", 50, 120, 1200) == DirectionEstimator::Approaching); // 50/30=1.67
    }

    // ── Slow / stopping vehicle: size barely changes so the ratio never reaches
    // minGrowthRatio, but many consistent sightings + a small net delta still decide. ──
    {
        DirectionEstimator::Config sc = cfg;   // window=1, growth=1.15
        sc.trendMinSightings = 6; sc.minTrendDeltaPx = 5.0;
        // Slow approach: 50 -> 55 over 6 reads (ratio 1.10 < 1.15, net +5px).
        DirectionEstimator d(sc);
        LPR_CHECK(d.update("g:SLOW", 50, 100, 1000) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:SLOW", 51, 101, 1100) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:SLOW", 52, 102, 1200) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:SLOW", 53, 103, 1300) == DirectionEstimator::Unknown);
        LPR_CHECK(d.update("g:SLOW", 54, 104, 1400) == DirectionEstimator::Unknown);   // n=5, delta 4 < 5
        LPR_CHECK(d.update("g:SLOW", 55, 105, 1500) == DirectionEstimator::Approaching); // n=6, delta 5
    }
    {
        DirectionEstimator::Config sc = cfg;
        sc.trendMinSightings = 6; sc.minTrendDeltaPx = 5.0;
        // Slow recede: 56 -> 51 over 6 reads.
        DirectionEstimator d(sc);
        for (int i = 0; i < 5; ++i) d.update("g:SLOWR", 56 - i, 100, 1000 + i * 100);
        LPR_CHECK(d.update("g:SLOWR", 51, 100, 1500) == DirectionEstimator::Receding);
    }
    {
        DirectionEstimator::Config sc = cfg;
        sc.trendMinSightings = 6; sc.minTrendDeltaPx = 5.0;
        // Fully stopped: no size change at all -> stays Unknown (can't be inferred).
        DirectionEstimator d(sc);
        for (int i = 0; i < 8; ++i)
            LPR_CHECK(d.update("g:STOP", 50, 100, 1000 + i * 100) == DirectionEstimator::Unknown);
    }
    {
        DirectionEstimator::Config sc = cfg;
        sc.trendMinSightings = 6; sc.minTrendDeltaPx = 5.0;
        // Pause-in-view (symmetric hump): approaches then leaves the SAME way -> ~0 net
        // delta -> not mislabeled (stays Unknown).
        DirectionEstimator d(sc);
        int last = DirectionEstimator::Unknown;
        double seq[] = {50, 52, 54, 54, 52, 50};   // small swing, net 0, ratio stays < 1.15
        for (int i = 0; i < 6; ++i) last = d.update("g:HUMP", seq[i], 100, 1000 + i * 100);
        LPR_CHECK(last == DirectionEstimator::Unknown);
    }

    // ── Refine with MORE movement: an early guess is corrected as more sightings
    // accumulate, then finalized once confirmSightings reads confirm the trend. ──
    {
        DirectionEstimator::Config rc = cfg;
        rc.confirmSightings = 8; rc.trackGapMs = 100000; rc.cooldownMs = 100000;
        DirectionEstimator d(rc);
        // First few reads shrink -> an early Receding guess.
        d.update("g:REF", 60, 100, 1000);
        d.update("g:REF", 55, 100, 1100);
        LPR_CHECK(d.update("g:REF", 50, 100, 1200) == DirectionEstimator::Receding);
        LPR_CHECK(d.current("g:REF") == DirectionEstimator::Receding);
        // More movement reveals a net approach -> CORRECTION to Approaching (same pass,
        // not finalized yet because fewer than confirmSightings reads).
        d.update("g:REF", 60, 100, 1300);
        LPR_CHECK(d.update("g:REF", 70, 100, 1400) == DirectionEstimator::Approaching); // 70/60=1.17
        LPR_CHECK(d.current("g:REF") == DirectionEstimator::Approaching);
    }
    // Once finalized (>= confirmSightings), a late reversing frame can no longer flip it.
    {
        DirectionEstimator::Config rc = cfg;
        rc.confirmSightings = 4; rc.trackGapMs = 100000; rc.cooldownMs = 100000;
        DirectionEstimator d(rc);
        d.update("g:LOCK", 30, 100, 1000);
        d.update("g:LOCK", 40, 100, 1100);
        d.update("g:LOCK", 55, 100, 1200);                                   // Approaching (3 reads)
        LPR_CHECK(d.update("g:LOCK", 70, 100, 1300) == DirectionEstimator::Unknown); // 4th: same dir, now LOCKED
        // A later shrink is ignored — the pass is finalized.
        LPR_CHECK(d.update("g:LOCK", 20, 100, 1400) == DirectionEstimator::Unknown);
        LPR_CHECK(d.current("g:LOCK") == DirectionEstimator::Approaching);
    }

    return LPR_TEST_RESULT();
}
