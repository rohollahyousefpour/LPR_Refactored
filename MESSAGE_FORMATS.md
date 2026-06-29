# LPR Outbound Message Formats

Every message the `lpr` client publishes to NATS, with its subject and JSON body. All messages
share the envelope `{ messageId, messageType, messageBody }` (`messageId` is a UUID v4). Images are
JPEG, either Base64 strings or raw byte arrays depending on `ImageEncoding` (default matches the
original's byte-array form). Timestamps are local time `YYYY-MM-DDTHH:MM:SS`.

These mirror the original `Handle_Clients` wire formats. Subjects are configurable; the defaults
below match the original deployment.

---

## 1. Plate data  —  subject `messages.plates_data`  (durable / JetStream)
Emitted by `PlateSender` once per accepted plate (per-gate:plate cooldown applies).

```json
{
  "messageId": "<uuid>",
  "messageType": "plates_data",
  "messageBody": {
    "timestamp": "2026-06-17T18:25:53",
    "camera_id": "<gate>",
    "full_image": "<jpeg base64|bytes>",      // omitted if image encoding disabled
    "cars": [
      {
        "box": { "left": 0, "top": 0, "right": 0, "bottom": 0 },
        "direction": 0,
        "plate": {
          "plate": "<text>",
          "is_valid": true,                     // from the injected validator (default true)
          "track_id": -1,                        // stable id when tracking, else -1
          "plate_image": "<jpeg base64|bytes>"
        },
        "ocr_accuracy": 0.0,                      // confidence 0..1
        "vehicle_class": { "class": null, "conf": 0 },
        "vehicle_type":  { "class": null, "conf": 0 },
        "meta_data": {}
      }
    ]
  }
}
```
Notes: `box` is the vehicle box when car detection is on, else the plate's bounding box.
`vehicle_class`/`vehicle_type` are placeholders (the original Iran-specific fields are left generic).

---

## 2. Live view  —  subject `socketio.live`
Emitted by `MediaSender` while a gate's live view is active (frame-skip via `sendEveryN`).

```json
{
  "messageId": "<uuid>",
  "messageType": "live",
  "messageBody": {
    "timestamp": "2026-06-17T18:25:53",
    "camera_id": "<gate>",
    "live_image": "<jpeg base64|bytes>"
  }
}
```

---

## 3. Recording  —  subject `message.recording.<gate>`
Emitted by `MediaSender` on each recording segment event (start/rollover/stop). `end_recording`
is `true` when a segment closes (including rollover) and `false` while recording continues.

```json
{
  "messageId": "<uuid>",
  "messageType": "recording",
  "messageBody": {
    "timestamp": "2026-06-17T18:25:53",
    "camera_id": "<gate>",
    "video_address": "<path or url of the segment file>",
    "frame": "<jpeg base64|bytes>",
    "end_recording": false
  }
}
```

---

## 4. Camera connection  —  subject `socketio.camera_connection`
Emitted by `CameraStatusNotifier`. Sent on **disconnect** (`Connection: false`) and again on
**reconnect** (`Connection: true`); de-duplicated so only state changes are published.

```json
{
  "messageId": "<uuid>",
  "messageType": "camera_connection",
  "messageBody": {
    "camera_id": "<gate>",
    "Connection": false
  }
}
```

---

## 5. Heartbeat  —  subject `socketio.heartbeat`
Emitted by `HeartbeatMonitor`, alternating with resources every `intervalMs`.

```json
{
  "messageId": "<uuid>",
  "messageType": "heartbeat",
  "lpr_id": "<client id>",
  "messageBody": { "info": "I am Live" }
}
```

---

## 6. Resources  —  subject `socketio.resources`
Emitted by `HeartbeatMonitor`. CPU/RAM/disk come from the injected `ResourceProvider`
(percentages formatted to 2 decimals).

```json
{
  "messageId": "<uuid>",
  "messageType": "resources",
  "messageBody": {
    "lpr_id": "<client id>",
    "CPU_USAGE": "12.50",
    "RAM_USAGE": "40.00",
    "Free_Space_Percentage": "80.00"
  }
}
```

---

## 7. Register  —  subject `lpr.register`  (published once on start)
Sent by `Application::start()` to announce the client and its token so the backend pushes settings.

```json
{
  "messageType": "register",
  "messageBody": { "token": "<auth token>" }
}
```

---

## Inbound / handshake — the REAL protocol (from the original NatsClient)
The client does **request/reply**, not push. Connection uses **mutual TLS** (`SetSecure` + CA + client
cert/key), `ExpectedHostname="ALPR"`, `NoEcho`, infinite reconnect, 5s timeout, optional
`NATS_USER`/`NATS_PASS`, and a JetStream context. Then:

1. **Authenticate** — publish `{ "token": "<token>" }` to `authenticate`, reply-to `response.token.<token>`.
   Server replies `{ "status": "success", "client_id": <id> }`; `client_id` becomes `lpr_id`.
2. **Commands** — subscribe `command.<clientId>`; each command's `messageBody.data.command_type` is one of
   `recording` (start recording), `streaming` (live view), `set_config`, `reset_camera`, `lpr_settings`;
   the client replies on `response.<clientId>` with `{status, command_type}`.
3. **Request settings** — publish `{ "client_id": "<id>", "request_type": "alpr_settings" }` to
   `alpr.settings.request`; reply on `alpr.settings.response.<clientId>`. Settings are in
   `messageBody.data` (the LPR `settings` array + `cameras_data`), loaded via `SettingsManager::loadAll`.

Settings `data` shape (LPR settings and each camera's settings are arrays of {name,value}):
```json
{
  "settings": [ { "name": "model_type", "value": "openvino" }, { "name": "car_detection", "value": 1 } ],
  "cameras_data": [
    { "camera_id": 1,
      "settings": [
        { "name": "type_of_link",      "value": "rtsp" },
        { "name": "CameraAddress",     "value": "rtsp://..." },
        { "name": "MonoCameraAddress", "value": "" },
        { "name": "CameraDelayTime",   "value": 10 }
      ] }
  ]
}
```
A camera with no `MonoCameraAddress` (or `""`/`"-1"`) runs as a single source; set it to a second
address to enable dual mono+RGB capture.
