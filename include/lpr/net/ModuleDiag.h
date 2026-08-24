#pragma once
//
// ModuleDiag - a lightweight, process-global sink for the camera module's
// diagnostic events, published to `messages.module_diag`.
//
// WHY: deep capture layers (CapturePipeline, ConnectionSupervisor) detect the
// real camera faults -- a grab timeout, a device error, an "controlled by
// another application" open failure, a jumbo/packet-size fallback -- but until
// now those only reached the module's LOCAL log file. The backend already
// ingests `messages.module_diag` into its log store (with `level` + `camera_id`
// correlation), so forwarding a fault here makes it appear in the frontend's
// «لاگ‌های تردد/دوربین» tab AND lets the camera-health dashboard show the real
// reason next to a failed Grab test -- with NO backend change.
//
// The sink is set ONCE at startup by Application (after the transport is up).
// Callers are best-effort: if no sink is set, or if the same (serial, code)
// fired within the throttle window, the call is a cheap no-op. This keeps a
// per-frame failure loop from flooding the log.
//
#include <functional>
#include <string>

namespace lpr::diag {

// Receives a ready-to-publish JSON string (destined for messages.module_diag).
using Sink = std::function<void(std::string /*json*/)>;

// Install the publisher. Thread-safe; call once from Application after the
// transport exists. Passing an empty function disables forwarding.
void setSink(Sink s);

// Surface a per-camera fault/notice to the backend log store.
//   cameraId : the DB camera id (== module gate) for log correlation/filtering.
//   serial   : the physical camera serial (shown in the message; also the
//              throttle key together with `code`).
//   level    : "ERROR" | "WARNING" | "INFO".
//   code     : a STABLE short slug for throttling + the diag `event`
//              (e.g. "grab_fault", "open_failed", "jumbo_fallback",
//              "reconnected"). Keep it free of volatile counters.
//   reason   : human-readable message (may carry volatile detail).
void cameraFault(const std::string& cameraId,
                 const std::string& serial,
                 const char* level,
                 const std::string& code,
                 const std::string& reason);

} // namespace lpr::diag
