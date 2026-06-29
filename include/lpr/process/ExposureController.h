#pragma once
// ExposureController - the pure decision law for the continuous, brightness-driven exposure
// loop, factored out of the Basler I/O so it can be unit-tested without a camera.
//
// Model: image brightness is ~ proportional to exposure * gain. One brightness measurement
// gives the multiplicative correction directly (target / measured). The step is damped and
// clamped to avoid oscillation ("tumbling" between over- and under-exposure), and a deadband
// around the target leaves a good-enough image alone.
//
// Actuator priority (best for OCR): when too dark, raise EXPOSURE up to the motion-blur cap
// first (noise-free), then add GAIN for the rest. When too bright, drop GAIN first (less
// noise), then shorten EXPOSURE. Exposure is never lengthened in the bright branch, so it can
// only move toward sharper.
#include <algorithm>
#include <cmath>

namespace lpr {

struct ExposureParams {
    double target       = 100.0; // desired brightness statistic (0..255)
    double deadband     = 10.0;  // no change while |target - measured| < this
    double damping      = 0.6;   // 0..1: fraction of the correction applied per step
    double maxStepRatio = 2.0;   // clamp the per-step exposure multiplier to [1/r, r]
    double expMin       = 1.0;   // exposure lower bound (us)
    double expMax       = 15000; // exposure upper bound = motion-blur cap (us)
    double gMin         = 5.0;   // gain lower bound
    double gMax         = 25.0;  // gain upper bound (noise ceiling for OCR)
    double gainStep     = 3.0;   // gain increment per step
};

struct ExposureDecision {
    double exposure = 0;   // proposed exposure (us)
    double gain     = 0;   // proposed gain
    bool   changed  = false;
    bool   dark     = false;   // true = was too dark (raised), false = too bright (lowered)
};

// Decide the next exposure/gain from a single brightness measurement and the current values.
inline ExposureDecision decideExposure(double measured, double exposure, double gain,
                                       const ExposureParams& p) {
    ExposureDecision d{exposure, gain, false, false};
    if (std::fabs(p.target - measured) < p.deadband) return d;          // deadband: leave it

    double factor = p.target / std::max(measured, 1.0);
    factor = std::pow(factor, std::clamp(p.damping, 0.05, 1.0));        // damp
    factor = std::clamp(factor, 1.0 / p.maxStepRatio, p.maxStepRatio);  // step-limit
    d.dark = factor > 1.0;

    if (factor > 1.0) {
        // Too dark: exposure up to the cap first, then gain for the residual.
        const double wantE    = exposure * factor;
        const double appliedE = std::clamp(wantE, p.expMin, p.expMax);
        if (appliedE > exposure + 1.0) { d.exposure = appliedE; d.changed = true; }
        const double residual = wantE / std::max(d.exposure, 1.0);      // cap bound? -> add gain
        if (residual > 1.02 && gain < p.gMax) {
            d.gain = std::min(p.gMax, gain + p.gainStep); d.changed = true;
        }
    } else {
        // Too bright: gain down first, then shorten exposure.
        if (gain > p.gMin) {
            d.gain = std::max(p.gMin, gain - p.gainStep); d.changed = true;
        } else {
            const double wantE    = exposure * factor;
            const double appliedE = std::clamp(wantE, p.expMin, p.expMax);
            if (appliedE < exposure - 1.0) { d.exposure = appliedE; d.changed = true; }
        }
    }
    return d;
}

} // namespace lpr
