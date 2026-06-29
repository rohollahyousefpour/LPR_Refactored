#pragma once
//
// ISyncConfigurator  (Strategy pattern)
// -------------------------------------
// Encapsulates HOW a camera's trigger + line nodes are programmed. The role is
// chosen by TRIGGER STATE, not by sensor type:
//
//   trigger OFF  ->  ExposureActiveOutputConfigurator  (config-2)
//   trigger ON   ->  TriggeredSlaveConfigurator        (config-1)
//
// The three deployment situations all reduce to those two configs:
//   1) lone RGB, free-running           -> trigger off -> config-2
//   2) pair master (mono), free-running -> trigger off -> config-2
//   3) pair slave (color), triggered    -> trigger on  -> config-1
//
// config-2 and config-1 are identical regardless of whether the camera is color
// or mono. Use chooseSyncConfigurator(triggerOn) to pick.
//
#include <memory>
#include "CameraDevice.h"

class ISyncConfigurator {
public:
    virtual ~ISyncConfigurator() = default;
    // Returns true only if every REQUIRED node for this role read back correctly.
    // false => the role could not be fully applied (e.g. a line/trigger node was
    // not writable or the camera coerced it); sync will not work.
    virtual bool configure(CameraDevice& dev) = 0;
    virtual const char* role() const = 0;
};

// config-2: trigger OFF, free-running, Line 2 = Output, source = Exposure Active.
// Used by a lone free-running RGB AND by the mono master in a pair. The Exposure
// Active pulse on Line 2 drives a slave's Line 1 (or an external light/strobe).
class ExposureActiveOutputConfigurator : public ISyncConfigurator {
public:
    bool configure(CameraDevice& dev) override;
    const char* role() const override { return "free-run/exposure-active (config-2)"; }
};

// config-1: trigger ON, Selector Frame Start, Source Line 1, Activation Rising.
// Used by the triggered slave (color) in a pair.
class TriggeredSlaveConfigurator : public ISyncConfigurator {
public:
    bool configure(CameraDevice& dev) override;
    const char* role() const override { return "triggered-slave (config-1)"; }
};

// Optional: software-triggered (on-demand capture). Not one of the 3 line cases.
class SoftwareTriggerConfigurator : public ISyncConfigurator {
public:
    bool configure(CameraDevice& dev) override;
    const char* role() const override { return "software"; }
};

// Plain free-run, "pylon default": trigger OFF and nothing else -- no master/slave
// wiring, no Exposure-Active strobe output. Each camera acquires continuously and
// (with camera-native auto) exposes itself. Used when triggers are turned off.
class FreeRunConfigurator : public ISyncConfigurator {
public:
    bool configure(CameraDevice& dev) override;
    const char* role() const override { return "free-run (no trigger)"; }
};

// THE selection rule. trigger_on drives everything.
inline std::shared_ptr<ISyncConfigurator> chooseSyncConfigurator(bool triggerOn) {
    if (triggerOn) return std::make_shared<TriggeredSlaveConfigurator>();
    return std::make_shared<ExposureActiveOutputConfigurator>();
}
