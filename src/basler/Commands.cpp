#include "CommandQueue.h"
#include "CameraDevice.h"
#include "IExposureStrategy.h"
#include "ISyncConfigurator.h"
#include "AppLogger.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

// ---- CommandQueue ----

void CommandQueue::push(std::unique_ptr<ICommand> cmd) {
    if (!cmd) return;
    std::lock_guard<std::mutex> lk(mtx_);
    q_.push_back(std::move(cmd));
}

void CommandQueue::drain(const CameraResolver& resolve, IExposureController* ctrl) {
    std::deque<std::unique_ptr<ICommand>> local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        local.swap(q_);                 // take everything, release lock fast
    }
    for (auto& cmd : local) {
        try { cmd->execute(resolve, ctrl); }
        catch (const Pylon::GenericException& e) {
            AppLogger::LogPylonException(e.GetDescription(), std::string("[command] ") + cmd->name());
        }
        catch (const std::exception& e) {
            AppLogger::LogException(e, std::string("[command] ") + cmd->name());
        }
        catch (...) {
            AppLogger::LogUnknownException(std::string("[command] ") + cmd->name());
        }
    }
}

bool CommandQueue::empty() const { std::lock_guard<std::mutex> lk(mtx_); return q_.empty(); }
void CommandQueue::clear()       { std::lock_guard<std::mutex> lk(mtx_); q_.clear(); }

// ---- Concrete commands ----
// Each carries its target serial and value, resolves its own device, applies
// once. No command touches a camera other than the one it names.

namespace {

struct SetExposureCommand : ICommand {
    std::string serial; double value; ExposureUnit unit;
    SetExposureCommand(std::string s, double v, ExposureUnit u = ExposureUnit::Auto)
        : serial(std::move(s)), value(v), unit(u) {}
    void execute(const CameraResolver& resolve, IExposureController* ctrl) override {
        // Manual control: suspend the camera's auto loop and set the value. The
        // controller swaps in a hold strategy so per-frame auto can't overwrite it.
        if (ctrl) { ctrl->setManualExposure(serial, value, unit); return; }
        if (auto* d = resolve(serial)) ManualExposureStrategy::setNormalizedExposure(*d, value);
    }
    const char* name() const override { return "ExposureTime"; }
};

struct SetGainCommand : ICommand {
    std::string serial; double value; ExposureUnit unit;
    SetGainCommand(std::string s, double v, ExposureUnit u = ExposureUnit::Auto)
        : serial(std::move(s)), value(v), unit(u) {}
    void execute(const CameraResolver& resolve, IExposureController* ctrl) override {
        if (ctrl) { ctrl->setManualGain(serial, value, unit); return; }
        if (auto* d = resolve(serial)) ManualExposureStrategy::setNormalizedGain(*d, value);
    }
    const char* name() const override { return "Gain"; }
};

// Restore the camera's configured (NATS-loaded) exposure behaviour: re-enables
// camera-native continuous auto / re-applies fixed / resumes host auto.
struct RevertToSettingsCommand : ICommand {
    std::string serial;
    explicit RevertToSettingsCommand(std::string s) : serial(std::move(s)) {}
    void execute(const CameraResolver& /*resolve*/, IExposureController* ctrl) override {
        if (ctrl) ctrl->revertToSettings(serial);
    }
    const char* name() const override { return "UseSettings"; }
};

struct SetTriggerModeCommand : ICommand {
    std::string serial; bool on;
    SetTriggerModeCommand(std::string s, bool v) : serial(std::move(s)), on(v) {}
    void execute(const CameraResolver& resolve, IExposureController* /*ctrl*/) override {
        auto* d = resolve(serial); if (!d || !d->raw()) return;
        auto* c = d->raw();
        if (!c->TriggerMode.IsWritable()) {
            LOGW() << "[trigger][" << serial << "] TriggerMode not writable; ignored";
            return;
        }
        // Apply to frame start; the trigger SOURCE (Line1 hardware / Software) is configured
        // separately by the sync-role/config path. Log it so the command's effect is observable.
        if (c->TriggerSelector.IsWritable())
            c->TriggerSelector.SetValue(Basler_UniversalCameraParams::TriggerSelector_FrameStart);
        c->TriggerMode.SetValue(on ? Basler_UniversalCameraParams::TriggerMode_On
                                   : Basler_UniversalCameraParams::TriggerMode_Off);
        LOGI() << "[trigger][" << serial << "] TriggerMode set -> " << (on ? "On" : "Off");
    }
    const char* name() const override { return "TriggerMode"; }
};

// Generic "Set Parameter": write ANY Basler node by name. Type is inferred from
// the JSON value (bool/int/float/string -> boolean/integer/float/enum or string
// node), each attempt guarded so a wrong type / unsupported / read-only node is
// just logged and skipped. This is the catch-all for basic controls (gamma, white
// balance, black level, frame rate, ROI, pixel format, auto limits, ...).
struct SetParameterCommand : ICommand {
    std::string serial; std::string node; json val;
    SetParameterCommand(std::string s, std::string n, json v)
        : serial(std::move(s)), node(std::move(n)), val(std::move(v)) {}

    // Parse the JSON value as a number whether it arrived as 1.2 or "1.2".
    static bool asNumber(const json& v, double& out) {
        if (v.is_number()) { out = v.get<double>(); return true; }
        if (v.is_string()) { try { out = std::stod(v.get<std::string>()); return true; } catch (...) {} }
        return false;
    }

    void execute(const CameraResolver& resolve, IExposureController* /*ctrl*/) override {
        auto* d = resolve(serial); if (!d || !d->raw()) return;
        auto& nm = d->raw()->GetNodeMap();
        const char* nn = node.c_str();
        bool done = false, clamped = false;
        std::string applied;

        // Float node: clamp into [Min, Max].
        if (!done) { try {
            Pylon::CFloatParameter p(nm, nn);
            double v;
            if (p.IsWritable() && asNumber(val, v)) {
                const double lo = p.GetMin(), hi = p.GetMax();
                const double cv = std::clamp(v, lo, hi);
                clamped = (cv != v);
                p.SetValue(cv);
                applied = std::to_string(cv);
                if (clamped) LOGW() << "[param][" << serial << "] " << node
                                    << " value " << v << " clamped to [" << lo << ".." << hi << "]";
                done = true;
            }
        } catch (...) {} }

        // Integer node: clamp into [Min, Max] and align to increment.
        if (!done) { try {
            Pylon::CIntegerParameter p(nm, nn);
            double v;
            if (p.IsWritable() && asNumber(val, v)) {
                const int64_t lo = p.GetMin(), hi = p.GetMax(), inc = p.GetInc();
                int64_t iv = static_cast<int64_t>(std::llround(v));
                iv = std::clamp(iv, lo, hi);
                if (inc > 1) iv = lo + ((iv - lo) / inc) * inc;   // align to step
                clamped = (double(iv) != v);
                p.SetValue(iv);
                applied = std::to_string(iv);
                if (clamped) LOGW() << "[param][" << serial << "] " << node
                                    << " value " << v << " clamped/aligned to " << iv
                                    << " (range [" << lo << ".." << hi << "] step " << inc << ")";
                done = true;
            }
        } catch (...) {} }

        // Boolean node.
        if (!done && val.is_boolean()) { try {
            Pylon::CBooleanParameter p(nm, nn);
            if (p.IsWritable()) { p.SetValue(val.get<bool>()); applied = val.dump(); done = true; }
        } catch (...) {} }

        // Enumeration node: only valid entries are accepted.
        if (!done && val.is_string()) { try {
            Pylon::CEnumParameter p(nm, nn);
            if (p.IsWritable() && p.TrySetValue(val.get<std::string>().c_str())) {
                applied = val.get<std::string>(); done = true;
            }
        } catch (...) {} }

        // String node.
        if (!done && val.is_string()) { try {
            Pylon::CStringParameter p(nm, nn);
            if (p.IsWritable()) { p.SetValue(val.get<std::string>().c_str()); applied = val.get<std::string>(); done = true; }
        } catch (...) {} }

        if (done) LOGI() << "[param][" << serial << "] " << node << " <- " << applied
                         << (clamped ? " (clamped)" : "");
        else      LOGW() << "[param][" << serial << "] could not set '" << node
                         << "' to " << val.dump() << " (absent / read-only / wrong type / invalid enum)";
    }
    const char* name() const override { return "SetParameter"; }
};

struct ApplySyncRoleCommand : ICommand {
    std::string serial; std::shared_ptr<ISyncConfigurator> cfg;
    ApplySyncRoleCommand(std::string s, std::shared_ptr<ISyncConfigurator> c)
        : serial(std::move(s)), cfg(std::move(c)) {}
    void execute(const CameraResolver& resolve, IExposureController* /*ctrl*/) override {
        if (auto* d = resolve(serial)) cfg->configure(*d);
    }
    const char* name() const override { return "SyncRole"; }
};

} // namespace

// Coerce a JSON value to bool tolerantly. Backends send booleans variously as true/false, 1/0, or
// "on"/"true" strings; json::get<bool>() throws on anything non-boolean, which silently dropped
// 'Trigger Mode' commands whose value arrived as a number.
static bool jsonToBool(const json& v, bool dflt = false) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0.0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        return s == "1" || s == "true" || s == "on" || s == "yes";
    }
    return dflt;
}

// Map an explicit "unit" field to an ExposureUnit. Absent/unknown => Auto (the
// legacy value>1.0 heuristic), so old backends keep working unchanged.
static ExposureUnit parseUnit(const json& v) {
    std::string u = v.value("unit", std::string{});
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char ch) { return (char)std::tolower(ch); });
    if (u == "us" || u == "abs" || u == "absolute" || u == "microseconds") return ExposureUnit::Absolute;
    if (u == "norm" || u == "normalized" || u == "pct" || u == "fraction")  return ExposureUnit::Normalized;
    return ExposureUnit::Auto;
}

std::unique_ptr<ICommand> makeCommand(const std::string& key, const json& v) {
    const std::string serial = v.value("camera_serial", std::string{});
    if (serial.empty()) { LOGW() << "[command] missing camera_serial for " << key; return nullptr; }

    if (key == "Exposure Time") return std::make_unique<SetExposureCommand>(serial, v.at("value").get<double>(), parseUnit(v));
    if (key == "Gain")          return std::make_unique<SetGainCommand>(serial, v.at("value").get<double>(), parseUnit(v));
    if (key == "Trigger Mode")  return std::make_unique<SetTriggerModeCommand>(serial, jsonToBool(v.at("value")));

    // Return to the camera's configured (NATS-loaded) settings / auto control.
    if (key == "Use Settings" || key == "Reset")
        return std::make_unique<RevertToSettingsCommand>(serial);
    if (key == "Exposure Auto") {
        // true => back to auto/settings. (false is a no-op: send Exposure Time/Gain to go manual.)
        const bool toAuto = v.contains("value") ? jsonToBool(v.at("value"), true) : true;
        if (toAuto) return std::make_unique<RevertToSettingsCommand>(serial);
        LOGW() << "[command] 'Exposure Auto'=false ignored (use Exposure Time/Gain for manual)";
        return nullptr;
    }

    if (key == "Sync Role") {
        // Role is driven by trigger state, matching the connect path:
        //   "trigger-off" / "free"   -> config-2 (ExposureActiveOutput)
        //   "trigger-on"  / "slave"  -> config-1 (TriggeredSlave)
        //   "software"               -> software trigger
        const std::string role = v.at("value").get<std::string>();
        std::shared_ptr<ISyncConfigurator> cfg;
        if      (role == "trigger-off" || role == "free" || role == "master")
            cfg = std::make_shared<ExposureActiveOutputConfigurator>();   // config-2
        else if (role == "trigger-on" || role == "slave")
            cfg = std::make_shared<TriggeredSlaveConfigurator>();         // config-1
        else if (role == "software")
            cfg = std::make_shared<SoftwareTriggerConfigurator>();
        else { LOGW() << "[command] unknown sync role: " << role; return nullptr; }
        return std::make_unique<ApplySyncRoleCommand>(serial, cfg);
    }

    // Explicit generic form: {key:"Set Parameter", node:"Gamma", value:1.2}
    if (key == "Set Parameter" || key == "Parameter") {
        const std::string node = v.value("node", std::string{});
        if (node.empty() || !v.contains("value")) {
            LOGW() << "[command] 'Set Parameter' needs node + value"; return nullptr;
        }
        return std::make_unique<SetParameterCommand>(serial, node, v.at("value"));
    }

    // Fallback: an unrecognized key WITH a value is treated as a raw Basler node
    // name, e.g. {key:"Gamma", value:1.2} -> set node "Gamma". (No value => unknown.)
    if (v.contains("value"))
        return std::make_unique<SetParameterCommand>(serial, key, v.at("value"));

    LOGW() << "[command] unknown command key: " << key;
    return nullptr;
}
