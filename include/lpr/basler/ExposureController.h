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

// NOTE: intentionally in the GLOBAL namespace to match the Basler exposure-strategy code
// (IExposureStrategy / AutoExposureStrategy live in the global namespace), which includes
// this header and uses these names unqualified.

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
    bool   highlightPriority = false; // expose for the BRIGHTEST region (plate): only pull down to
                                      // keep it below clipping; never raise gain to chase a dark
                                      // background. Best for night/IR ALPR.
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

    // Hard motion-blur ceiling: exposure must NEVER exceed the cap, even when the scene is dark and
    // the loop would otherwise only want more light. Without this, a stale above-cap value (e.g.
    // maxExposure was lowered at runtime, or a longer exposure persisted from a previous run / from
    // manual control) is never reined in, because the "too dark" branch only ever raises exposure.
    // Pull it straight down to the cap; the brightness loop then uses gain for any shortfall.
    if (exposure > p.expMax + 1.0) {
        d.exposure = p.expMax; d.changed = true;
        return d;
    }

    if (std::fabs(p.target - measured) < p.deadband) return d;          // deadband: leave it

    double factor = p.target / std::max(measured, 1.0);
    factor = std::pow(factor, std::clamp(p.damping, 0.05, 1.0));        // damp
    factor = std::clamp(factor, 1.0 / p.maxStepRatio, p.maxStepRatio);  // step-limit
    d.dark = factor > 1.0;

    if (p.highlightPriority) {
        // Highlight-priority: keep the brightest region (the plate) just below clipping and keep
        // gain LOW. We never add gain to brighten a dark scene - at night an empty road is
        // *supposed* to be dark; only a real (IR-lit, retroreflective) plate should be bright, and
        // if it ever clips we shorten exposure to recover it. Exposure otherwise drifts toward the
        // motion cap so a plate that appears is captured with the most (still-sharp) light.
        if (factor < 1.0) {
            // Brightest region too bright -> pull DOWN: gain to min first, then exposure shorter.
            if (gain > p.gMin) { d.gain = std::max(p.gMin, gain - p.gainStep); d.changed = true; }
            else {
                const double appliedE = std::clamp(exposure * factor, p.expMin, p.expMax);
                if (appliedE < exposure - 1.0) { d.exposure = appliedE; d.changed = true; }
            }
        } else {
            // Headroom -> lengthen exposure toward the cap (brighter, still sharp) and bleed gain
            // down to its floor. Gain is NEVER raised in this mode.
            const double appliedE = std::clamp(exposure * factor, p.expMin, p.expMax);
            if (appliedE > exposure + 1.0) { d.exposure = appliedE; d.changed = true; }
            if (gain > p.gMin) { d.gain = std::max(p.gMin, gain - p.gainStep); d.changed = true; }
        }
        return d;
    }

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
