#include "lpr/basler/ExposureController.h"
#include "../lpr_check.hpp"

// ExposureController types live in the global namespace (to match the Basler code).

int main() {
    ExposureParams p;
    p.target = 120; p.deadband = 10; p.damping = 1.0; p.maxStepRatio = 4.0;
    p.expMin = 100; p.expMax = 10000; p.gMin = 5; p.gMax = 25; p.gainStep = 4;

    // Within deadband -> no change.
    {
        ExposureDecision d = decideExposure(118, 2000, 5, p);
        LPR_CHECK(!d.changed);
    }

    // Too dark, exposure has headroom -> raise EXPOSURE, not gain.
    {
        ExposureDecision d = decideExposure(60, 2000, 5, p);   // factor=2 -> want 4000us
        LPR_CHECK(d.changed && d.dark);
        LPR_CHECK(d.exposure > 2000 && d.exposure <= 10000);
        LPR_CHECK(d.gain == 5);                                // gain untouched, headroom in exposure
    }

    // Too dark and exposure pinned at the cap -> add GAIN for the residual.
    {
        ExposureDecision d = decideExposure(40, 9000, 5, p);   // want 27000us, capped at 10000
        LPR_CHECK(d.changed && d.dark);
        LPR_CHECK(d.exposure == 10000);                        // hit the motion-blur cap
        LPR_CHECK(d.gain > 5);                                  // residual handled by gain
    }

    // Too bright with gain above min -> drop GAIN first, leave exposure.
    {
        ExposureDecision d = decideExposure(240, 3000, 13, p);
        LPR_CHECK(d.changed && !d.dark);
        LPR_CHECK(d.gain < 13);
        LPR_CHECK(d.exposure == 3000);                         // exposure not shortened yet
    }

    // Too bright at min gain -> shorten EXPOSURE.
    {
        ExposureDecision d = decideExposure(240, 3000, 5, p);
        LPR_CHECK(d.changed && !d.dark);
        LPR_CHECK(d.gain == 5);
        LPR_CHECK(d.exposure < 3000 && d.exposure >= 100);
    }

    // Step-ratio clamp: a huge error can't move exposure more than maxStepRatio in one step.
    {
        ExposureParams q = p; q.maxStepRatio = 2.0; q.damping = 1.0;
        ExposureDecision d = decideExposure(10, 1000, 5, q);   // factor would be 12, clamp to 2x
        LPR_CHECK(d.exposure <= 1000 * 2.0 + 1e-6);
    }

    // Hard cap: exposure above maxExposure is pulled DOWN to the cap even when the scene is dark
    // (motion-blur ceiling must hold; this is the 5567us-vs-5000-cap night bug).
    {
        ExposureParams q = p; q.expMax = 5000; q.target = 100; q.gMax = 25;
        ExposureDecision d = decideExposure(24, 5567, 25, q);  // dark + above cap
        LPR_CHECK(d.changed && d.exposure <= 5000 + 1e-6);
    }

    // --- Highlight-priority mode (expose for the bright plate; never gain up a dark scene). ---
    {
        ExposureParams h;
        h.target = 220; h.deadband = 10; h.damping = 1.0; h.maxStepRatio = 4.0;
        h.expMin = 100; h.expMax = 4000; h.gMin = 5; h.gMax = 25; h.gainStep = 4;
        h.highlightPriority = true;

        // Dark scene, gain already high: must NOT raise gain further (the whole point); it bleeds
        // gain DOWN and lengthens exposure toward the cap instead.
        ExposureDecision d = decideExposure(42, 3000, 25, h);
        LPR_CHECK(d.changed);
        LPR_CHECK(d.gain <= 25);                       // never increased
        LPR_CHECK(d.gain < 25 || d.exposure > 3000);   // bled gain down and/or stretched exposure
        LPR_CHECK(d.exposure <= 4000);                 // respects the cap

        // Bright plate clipping, gain at floor -> shorten exposure to recover it.
        ExposureDecision e = decideExposure(245, 3000, 5, h);
        LPR_CHECK(e.changed && e.exposure < 3000 && e.gain == 5);

        // Bright plate clipping with gain above floor -> drop gain first.
        ExposureDecision f = decideExposure(245, 3000, 17, h);
        LPR_CHECK(f.changed && f.gain < 17);
    }

    return LPR_TEST_RESULT();
}
