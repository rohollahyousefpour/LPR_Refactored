#pragma once
//
// IExposureStrategy  (Strategy pattern)
// -------------------------------------
// Encapsulates HOW exposure/gain are decided for a frame. The CapturePipeline
// owns one of these PER CAMERA (keyed by serial) and calls apply() each frame.
//
// IMPORTANT: AutoExposureStrategy holds per-camera control state (throttle,
// smoothed brightness). It is NOT shareable across cameras -- construct a
// separate instance per serial, with that camera's own Limits.
//
#include <chrono>
#include <memory>
#include <opencv2/core.hpp>
#include "CameraDevice.h"
#include "ExposureController.h"   // pure control law (same dir => always resolves)


class IExposureStrategy {
public:
    virtual ~IExposureStrategy() = default;
    virtual void apply(CameraDevice& dev, const cv::Mat& bgr) = 0;
    virtual const char* name() const = 0;
};

// ---------------------------------------------------------------------------
// AutoExposureStrategy
//
// An exposure-priority brightness controller:
//   * Robust metering: a configurable ROI and a percentile (median by default)
//     instead of a plain frame mean, so headlights / bright sky don't drag it.
//   * EMA smoothing of the measured brightness to damp frame-to-frame hunting.
//   * Multiplicative, unit-agnostic control: it scales exposure toward the
//     target (works for raw-us and float-us nodes alike); gain steps are a
//     fraction of the operating range (works for raw counts and dB alike).
//   * Exposure-priority: when too dark it raises EXPOSURE first (no noise) and
//     only adds gain once exposure is clamped; when too bright it lowers GAIN
//     first (reduces noise) and only then exposure. This keeps gain as low as
//     possible and removes the oscillation at the gain<->exposure handoff.
//   * Read-back verification: after writing exposure it reads the actual value.
//     If the camera clamped it (e.g. a triggered slave whose exposure can't
//     exceed the trigger interval), the controller sees the real ceiling and
//     compensates with gain instead of believing a write that didn't take.
// ---------------------------------------------------------------------------
class AutoExposureStrategy : public IExposureStrategy {
public:
    struct Limits {
        // Motion-blur ceiling (us): the controller raises EXPOSURE only up to here, then adds
        // GAIN, so a moving plate stays sharp. TUNE to your site/vehicle speeds.
        int maxExposureUs = 15000;

        // Target median/percentile intensity (0..255) the controller drives measured brightness
        // toward. Purely brightness-driven now (no day/night clock).
        double target = 100.0;

        int minGain     = 5;
        int maxGain     = 25;             // gain ceiling before noise ruins OCR
        std::chrono::milliseconds minInterval{1000};

        // Metering
        cv::Rect roi{};            // empty => whole frame (plates may be anywhere)
        double   percentile = 50;  // 50 = median; robust to headlights/bright sky
        double   emaAlpha   = 0.5; // brightness smoothing (0..1, lower = smoother)

        // Control shape
        double damping      = 0.6; // 0..1; fraction of correction applied per step
        double maxStepRatio = 2.0; // clamp per-step exposure multiplier to [1/r, r]
        double gainStepFrac = 0.15;// gain step as fraction of (maxGain-minGain)
        double deadband     = 10.0;// no change while |target - measured| < this (anti-hunt)
        bool   highlightPriority = false; // expose for the brightest region (plate), keep gain low
    };

    explicit AutoExposureStrategy(Limits limits);

    void apply(CameraDevice& dev, const cv::Mat& bgr) override;
    const char* name() const override { return "auto"; }

private:
    bool throttled();

    Limits  lim_;
    std::chrono::steady_clock::time_point lastChange_{};
    std::chrono::steady_clock::time_point lastLog_{};   // throttles the status heartbeat
    double  measuredEma_ = -1.0;   // <0 => uninitialised
};

// ---------------------------------------------------------------------------
// ManualExposureStrategy: fixed value set by command; apply() is a no-op so the
// auto loop can never overwrite it. The exposure ceiling is now a parameter so
// auto and manual agree on bounds (was a hardcoded 50000).
// ---------------------------------------------------------------------------
class ManualExposureStrategy : public IExposureStrategy {
public:
    void apply(CameraDevice&, const cv::Mat&) override {}
    const char* name() const override { return "manual"; }

    static void setNormalizedExposure(CameraDevice& dev, double norm,
                                      double maxExposureUs = 50000.0);   // [0..1]
    static void setNormalizedGain(CameraDevice& dev, double norm);       // [0..1]
    // Absolute variants (used when the backend sends real values, e.g. microseconds).
    static void setExposureUs(CameraDevice& dev, double us,
                              double maxExposureUs = 50000.0);            // absolute microseconds
    static void setGainAbs(CameraDevice& dev, double gain);              // absolute gain units
};

// ---------------------------------------------------------------------------
// FixedExposureStrategy: the PLATE camera (mono in a pair, IR-illuminated).
// It is set ONCE to a low, motion-freezing exposure tuned to the retroreflected
// plate and then HELD -- apply() does nothing, so scene brightness never pushes
// its exposure up and reintroduces motion blur. This is the correct policy for
// a detector camera whose lighting is controlled (IR), not ambient.
// ---------------------------------------------------------------------------
class FixedExposureStrategy : public IExposureStrategy {
public:
    // exposureUs / gain are absolute node values. TUNE to your IR setup.
    FixedExposureStrategy(double exposureUs, double gain)
        : exposureUs_(exposureUs), gain_(gain) {}

    // Applied once when the camera connects (call from the connect path).
    void configure(CameraDevice& dev);

    void apply(CameraDevice&, const cv::Mat&) override {}  // never auto-adjust
    const char* name() const override { return "fixed"; }

private:
    double exposureUs_;
    double gain_;
    bool   applied_ = false;
};

// ---------------------------------------------------------------------------
// CameraAutoExposureStrategy: hand exposure + gain to the camera's ON-SENSOR
// auto ("same as pylon"). configure() enables ExposureAuto=Continuous and
// GainAuto=Continuous with a target brightness and hard limits (exposure upper =
// motion cap, gain upper = OCR-safe ceiling); apply() is a no-op because the
// camera firmware runs the loop every frame -- no host round-trips. Optional
// nodes are written by name via GenApi, so it tolerates SFNC/model differences.
// ---------------------------------------------------------------------------
class CameraAutoExposureStrategy : public IExposureStrategy {
public:
    explicit CameraAutoExposureStrategy(AutoExposureStrategy::Limits limits)
        : lim_(limits) {}

    // Applied once when the camera connects (call from the connect path).
    void configure(CameraDevice& dev);

    void apply(CameraDevice&, const cv::Mat&) override {}   // sensor does the work
    const char* name() const override { return "camera-auto"; }

private:
    AutoExposureStrategy::Limits lim_;
    bool applied_ = false;
};
