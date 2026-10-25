#pragma once
// LogForwarder — on-demand live streaming of this module's log lines to the
// backend. The module always writes its log to a local .txt (via lpr::Logger);
// when the panel opens the live-log viewer, the backend sends a `log_stream`
// command, which flips this forwarder ON. While on, every line the logger writes
// is also published to NATS (`socketio.module_log`), which the backend fans out
// to the socket room. The backend re-sends `start` periodically (keep-alive); if
// those stop (viewer closed / backend down), the safety window lapses and
// streaming auto-stops — so the module never streams its log to nobody.
//
// Mirrors the free-function style of lpr::diag (ModuleDiag): a NATS publish sink
// is wired once by Application; the Logger's sink calls onLogLine for every line.
#include <functional>
#include <string>

#include "lpr/Log.h"   // lpr::Level

namespace lpr::logfwd {

// Publish (subject, payload) over NATS — wired by Application to the transport.
using Sink = std::function<void(const std::string& subject, const std::string& payload)>;

// Wire the NATS publish sink and this device's client id (its lpr_id). Call once,
// after the NATS handshake, before any `log_stream` command can arrive.
void configure(Sink sink, std::string clientId);

// Backend control (`log_stream` command). `start` carries a safety window
// (seconds): streaming auto-stops if no further `start` arrives within it.
void start(int durationSeconds);
void stop();

// Whether streaming is currently active (and unexpired) — cheap, lock-free.
bool active();

// Registered as the lpr::Logger sink: called for EVERY written line. Publishes
// the line when streaming is active; a no-op otherwise.
void onLogLine(lpr::Level level, const std::string& isoTime, const std::string& message);

} // namespace lpr::logfwd
