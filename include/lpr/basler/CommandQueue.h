#pragma once
//
// ICommand + CommandQueue  (Command pattern)
// ------------------------------------------
// A control request (set exposure, set gain, change trigger...) is modelled as
// a self-contained command object that knows its target camera serial and how
// to execute itself against a CameraRegistry. Commands are produced on the
// caller's thread (handleCommand) and consumed on the capture thread, so the
// queue is the thread-safe hand-off point.
//
// Why Command here:
//   The old handler map + cmdQueue<pair<string,json>> already separated
//   "describe the request" from "do it later on the right thread" -- that IS
//   the Command pattern. Formalising it gives each control a typed, testable
//   object and removes the per-camera double-execution bug (each command now
//   targets exactly one camera by serial).
//
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class CameraDevice;

// Resolves a serial to its device (returns nullptr if absent/null). Supplied by
// the facade so commands don't depend on the storage details.
using CameraResolver = std::function<CameraDevice*(const std::string&)>;

// Lets a command suspend a camera's per-frame auto-exposure loop for MANUAL
// control, and restore the camera's configured (NATS-loaded) settings afterwards.
// Implemented by the ConnectionSupervisor, which owns the per-serial profiles and
// installed strategies. May be null (e.g. in tests) -- commands then fall back to
// a direct device write.
// How to interpret a manual exposure/gain value:
//   Auto       -- legacy heuristic: value > 1.0 is absolute (microseconds / gain units),
//                 otherwise a 0..1 fraction of the range.
//   Absolute   -- the value is in device units (us / gain), honored up to the hardware max.
//   Normalized -- the value is a 0..1 fraction of the allowed range.
// The backend can send an explicit "unit" so exposure/gain are never ambiguous (e.g. an
// absolute gain below 1.0 dB is not mistaken for a 100%-scale fraction).
enum class ExposureUnit { Auto, Absolute, Normalized };

class IExposureController {
public:
    virtual ~IExposureController() = default;
    virtual void setManualExposure(const std::string& serial, double value,
                                   ExposureUnit unit = ExposureUnit::Auto) = 0;
    virtual void setManualGain(const std::string& serial, double value,
                               ExposureUnit unit = ExposureUnit::Auto) = 0;
    virtual void revertToSettings(const std::string& serial) = 0;               // back to auto
};

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void execute(const CameraResolver& resolve, IExposureController* ctrl) = 0;
    virtual const char* name() const = 0;
};

// Thread-safe FIFO of pending commands.
class CommandQueue {
public:
    void push(std::unique_ptr<ICommand> cmd);

    // Drain and execute every queued command. Call from the capture thread.
    void drain(const CameraResolver& resolve, IExposureController* ctrl = nullptr);

    bool empty() const;
    void clear();

private:
    mutable std::mutex      mtx_;
    std::deque<std::unique_ptr<ICommand>> q_;
};

// Factory: build a command from a (key, json) pair as received over the wire.
// Returns nullptr for an unknown key. Centralises parsing so the transport
// layer stays dumb.
std::unique_ptr<ICommand> makeCommand(const std::string& key, const json& value);
