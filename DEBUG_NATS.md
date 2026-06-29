# Debugging the NATS send / command issue in VS Code

Goal: find out **why the server doesn't receive plates and the client doesn't receive commands**,
even though the client logs `PublishString -> OK`.

## 0. One-time setup
1. Open the `lpr` folder in VS Code (with the CMake Tools + C/C++ extensions).
2. Configure + build the debug-capable release exe:
   - Command Palette -> **CMake: Select Configure Preset** -> `full-vcpkg`
   - Run the task **Build lpr.exe (full-vcpkg)** (Terminal -> Run Build Task, or Ctrl+Shift+B).
   - This now produces `build/full-vcpkg/lpr.exe` **and** `lpr.pdb` (symbols for breakpoints).
3. Edit `.vscode/launch.json`: paste your real `AUTH_TOKEN`, and if your **original** client runs
   with `NATS_USER` / `NATS_PASS`, set them too. Fix the OpenVINO paths in `PATH` if your install differs.

## 1. The single most important breakpoint
Set a breakpoint in `src/net/NatsTransport.cpp` on the body of **`natsErrorCb`**:

```cpp
static void natsErrorCb(natsConnection*, natsSubscription* sub, natsStatus err, void*) {
    const char* subj = sub ? natsSubscription_GetSubject(sub) : nullptr;   // <-- breakpoint here
```

Press **F5** (the "Debug lpr (NATS, live server)" config). Let it run for ~30s with traffic.

- **If this breakpoint hits** -> the NATS **server is rejecting** something. Hover `err` and call
  `natsStatus_GetText(err)` in the Debug Console, and read `subj`. You'll see one of:
  - `Permissions Violation` on `messages.plates_data` / `socketio.*` -> your account can't publish there.
  - `Permissions Violation` on `command.1` -> your account can't subscribe there (so commands never arrive).
  - `Maximum Payload` -> the plate message is too big for the server's `max_payload`.
  This is a **server-side / account** problem, not the client. Fix = grant pub/sub on those subjects
  to your cert/user, or connect as the same account your working original uses (`NATS_USER`/`NATS_PASS`).
- **If it never hits** -> the server is NOT rejecting anything; the messages are accepted but there is
  no JetStream stream / consumer bound to `messages.plates_data` on this account (also server-side).

## 2. Confirm the publish actually leaves with NATS_OK
Breakpoint in `Impl::senderLoop` (same file) on:
```cpp
natsStatus s = natsConnection_PublishString(conn, pm.subject.c_str(), pm.payload.c_str());
```
Step over it and inspect `s` and then the `FlushTimeout` result. Both should be `NATS_OK (0)`.
Inspect `pm.subject` and `pm.payload.size()`. If `s == NATS_OK`, the message left the client cleanly.

## 3. Are commands arriving at all?
Breakpoint in **`natsMsgTrampoline`** (same file) on the first line.
While stopped at it is fine; then from your dashboard **trigger a "recording" or "streaming" command**.
- If the breakpoint **hits** with `subj == "command.1"` -> commands ARE arriving; step into the handler
  (`Application::onCommand`) to see dispatch.
- If it **never hits** when you trigger a command -> the server isn't delivering to `command.1` for this
  client (account permission on `command.*`, or the server is keyed to a different client id).

## 4. Definitive server-side check (no debugger needed)
If you have the `nats` CLI, from any machine with the same creds:
```
nats --server tls://185.81.99.23:4222 --tlscert client-cert.pem --tlskey client-key.pem --tlsca ca.pem sub "messages.plates_data"
```
Then run the client and make a plate.
- Plates appear in `nats sub` -> the client + server transport are fine; the problem is only your
  downstream consumer / JetStream binding.
- Nothing appears -> the publish is being dropped at the server (permissions) — matches breakpoint #1.

## What each outcome means
| Observation | Cause | Fix |
|---|---|---|
| `natsErrorCb` hits with Permissions Violation | account lacks pub/sub on that subject | server-side ACL, or use original's NATS_USER/NATS_PASS |
| `natsErrorCb` hits with Maximum Payload | plate message bigger than server max_payload | lower JPEG quality (full_image) or raise server max_payload |
| no error, `nats sub` sees plates | client/server fine | fix the downstream consumer / JetStream stream |
| no error, `nats sub` sees nothing | silent drop = no subscriber/stream on subject | create the stream/consumer on the server |
| `natsMsgTrampoline` never hits on a command | server not delivering command.1 | account perms on `command.*`, or wrong client id |
