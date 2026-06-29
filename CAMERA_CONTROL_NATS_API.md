# Camera Control over NATS — Backend Integration Guide

How to control a Basler camera at runtime by publishing NATS commands to the ALPR
client. The camera commands use the **same envelope** as the existing `recording` /
`streaming` commands — only `commandType` and the data fields differ.

---

## 1. Transport

| | |
|---|---|
| **Publish commands to** | `command.<client_id>` |
| **Listen for acks on** | `response.<client_id>` |

`<client_id>` is the client's configured id (e.g. `1` → subject `command.1`).

The client replies on `response.<client_id>` with an **acknowledgement of receipt**:

```json
{ "status": "success", "command_type": "camera_command" }
```

This confirms the message was received and routed — it is **not** a per-camera
confirmation that a node was written. To confirm the camera actually changed, watch the
client log (see §7).

---

## 2. Envelope (same as recording / streaming)

All commands are nested under `messageBody.data`, exactly like the existing commands.

**Existing commands, for reference:**

```json
{ "messageBody": { "data": { "commandType": "streaming", "cameraId": 1, "duration": 30 } } }
```
```json
{ "messageBody": { "data": { "commandType": "recording", "cameraId": 1, "duration": 60 } } }
```

**Camera control uses the identical shape** with `commandType: "camera_command"`:

```json
{
  "messageBody": {
    "data": {
      "commandType":  "camera_command",
      "cameraId":     1,
      "cameraSerial": "40012345",
      "key":          "Exposure Time",
      "value":        0.4
    }
  }
}
```

| Field (under `messageBody.data`) | Required | Meaning |
|---|---|---|
| `commandType` | yes | Must be `"camera_command"`. |
| `cameraId` | yes | Which camera **entry** (the gate / camera id, same value as recording/streaming). |
| `cameraSerial` | yes | Which **physical** Basler within that entry — the mono or the RGB of a pair. |
| `key` | yes | The command (see §3) or a raw Basler node name (see §5). |
| `value` | depends | Required for most commands; omitted for `Use Settings`. Send the correct JSON type. |
| `node` | only for explicit Set Parameter | Basler node name (see §5). |

**Field-name tolerance.** The client matches keys at any nesting and accepts common
variants, but use the canonical names above to stay consistent with recording/streaming.
Accepted: `cameraId` ↔ `camera_id`/`gate`/`gate_id`/`id`; `cameraSerial` ↔
`camera_serial`/`serial`; `key` ↔ `parameter`/`name`/`setting`; `value` ↔ `val`;
`node` ↔ `feature`/`param_name`; `commandType` ↔ `command_type`.

**Two hard rules:**

1. **`cameraSerial` is mandatory.** Without it the command is dropped (the camera of a
   pair can't be disambiguated). Log: `[command] missing camera_serial`.
2. **`value` must be the correct JSON type** — number for exposure/gain, boolean for
   trigger, string for sync role / enum nodes. `"0.5"` (string) ≠ `0.5` (number).

---

## 3. Command catalog

| `key` | `value` type | Effect |
|---|---|---|
| `Exposure Time` | number `0.0`–`1.0` | **Manual** exposure (normalized). Suspends auto for this camera. |
| `Gain` | number `0.0`–`1.0` | **Manual** gain (normalized). Suspends auto for this camera. |
| `Trigger Mode` | boolean | Hardware trigger on (`true`) / off (`false`). |
| `Sync Role` | string | `"master"`/`"free"`, `"slave"`, or `"software"`. |
| `Use Settings` | *(none)* | **Revert** the camera to its NATS-loaded settings; resume auto. |
| `Set Parameter` | any (+ `node`) | Write any Basler node by name (see §5). |
| *any other key* | any | Treated as a raw Basler node name = `value` (see §5). |

### Exposure & gain — absolute *or* normalized

`value` is interpreted by magnitude:

- **`value > 1.0` → absolute.** `Exposure Time` is **microseconds** (e.g. `5000` = 5000 µs),
  `Gain` is the camera's gain units (dB or raw). This is the normal case — send the real value.
- **`0.0 < value ≤ 1.0` → normalized** fraction of the allowed range (`0.0` = min, `1.0` = max).

Either way, exposure is clamped to the motion-blur cap (`maxExposure` from settings) and gain
to the camera's range, so a value can't blur a plate or exceed the sensor limits.

A manual `Exposure Time` or `Gain` puts that camera into **manual hold**: the per-frame
auto-exposure loop is suspended so it can't overwrite the operator's value.

### Live view during manual control

When you send any manual adjustment for a camera (`Exposure Time`, `Gain`, or a parameter),
the client **automatically streams that camera's live view over NATS** so the operator can
see the effect — no separate `streaming` command needed. During manual control the client
sends a dedicated message type, **`live_manual_control`**, to subject `socketio.live`. It
carries **all sensors of the camera in one message** — both the mono and RGB of a pair, or
the single camera — each with its serial and the operator's current exposure/gain:

```json
{
  "messageId": "...",
  "messageType": "live_manual_control",
  "messageBody": {
    "timestamp": "...",
    "camera_id": 1,
    "cameras": [
      { "camera_serial": "40012345", "role": "rgb",  "live_image": "<jpeg>", "exposure_us": 5000, "gain": 6.0 },
      { "camera_serial": "40012346", "role": "mono", "live_image": "<jpeg>", "exposure_us": 1000 }
    ]
  }
}
```

- `role` is `"rgb"` / `"mono"` for a pair, or `"single"` for one camera (then `cameras` has one entry).
- `exposure_us` is the operator's last-set absolute exposure (omitted if only a normalized 0–1
  value was sent); `gain` is the last-set gain (omitted if not set). The fields appear once the
  operator has set them via `Exposure Time` / `Gain`.
- Serials come from settings: `CameraAddress` = RGB/primary, `MonoCameraAddress` = mono.
- The stream is throttled (~every other frame), refreshes on each manual command, and expires
  ~60 s after the last one; `Use Settings` stops it immediately and the gate returns to the
  normal `live` stream.

> Backend: branch on `messageType` — `"live"` is the normal single-image stream
> (`messageBody.camera_id` + `live_image`); `"live_manual_control"` is the manual-session
> stream (`messageBody.cameras[]`, each with `camera_serial`, `role`, `live_image`, and
> optional `exposure_us`/`gain`).

**Message size.** A manual message carries two images, so it defaults to compact settings:
`live_image` is **base64** (≈3× smaller than the normal `live` byte-array), half-resolution,
JPEG quality 20. Typical real frames are a few KB each; worst case (high-detail) ~30 KB/image.
Three settings tune it without recompiling:

| setting | default | effect |
|---|---|---|
| `manual_live_quality` | 20 | JPEG quality (lower = smaller) |
| `manual_live_scale` | 2 | downscale divisor: 1 = full, 2 = half, 4 = quarter |
| `manual_live_base64` | 1 | 1 = base64 string (small); 0 = JSON byte-array (matches normal `live`) |

So `live_image` in `live_manual_control` is a **base64 string** by default. If your backend
reuses the normal `live` decoder (byte-array), set `manual_live_base64 = 0` and it'll match.

### Trigger / sync

- `Trigger Mode` `true`/`false` toggles the hardware trigger live.
- `Sync Role`: `"master"`/`"free"` = free-run + Exposure-Active strobe on Line 2;
  `"slave"` = triggered on Line 1 (rising edge); `"software"` = software trigger.

---

## 4. Manual control → return to settings (the intended flow)

```
1. Camera runs on its NATS-loaded settings        (brightness auto-exposure, capped at maxExposure)
2. Operator adjusts to test/inspect:
     key="Exposure Time" value=0.6                 -> manual hold (auto suspended)
     key="Gain"          value=0.3                 -> manual hold
     key="Gamma"         value=1.2                 -> raw node set
3. Operator finishes:
     key="Use Settings"                            -> EVERYTHING restored to the load-time
                                                       NATS state; auto-exposure resumes
```

**`Use Settings` restores the full camera state**, not just exposure. At connect the
client snapshots the entire configured node map (gamma, white balance, black level,
frame rate, exposure, gain, trigger, …); `Use Settings` reloads that snapshot, so the
camera returns to exactly what NATS configured at load. `Reset`, and `Exposure Auto`
with `value:true`, do the same thing. A camera reconnect also returns it to load-time
settings automatically.

---

## 5. Generic parameter control (any node)

Set any other basic control by sending the **Basler node name**. Two equivalent forms:

**Raw key (simplest):** the `key` *is* the node name.

```json
{ "messageBody": { "data": {
  "commandType":"camera_command", "cameraId":1, "cameraSerial":"40012345",
  "key":"Gamma", "value":1.2 } } }
```

**Explicit:**

```json
{ "messageBody": { "data": {
  "commandType":"camera_command", "cameraId":1, "cameraSerial":"40012345",
  "key":"Set Parameter", "node":"BalanceWhiteAuto", "value":"Continuous" } } }
```

The client infers the node type from the camera and **clamps the value into that node's
valid range** (and aligns integers to the node's step), then writes it. So a value that's
slightly off or out of range is corrected and applied, not rejected — and the log notes
when it was clamped. An unknown name, a read-only node, an invalid enum string, or a wrong
type is logged and skipped (never a crash).

**Values are absolute, in each node's own units — not 0–1.** Every parameter has its own
range: `Gamma` ~0–4, `AutoTargetBrightness` 0–1, `AcquisitionFrameRate` 1–N, `BlackLevel`
0–N, etc. Send the real value in the node's units; the client clamps it to the node's
range. (Only `Exposure Time` / `Gain` accept the 0–1 *normalized* shorthand — see §3 — and
even there a value > 1 is treated as absolute.)

### Commonly useful nodes

| Control | Node name | Example value |
|---|---|---|
| Gamma | `Gamma` | `1.2` (float) |
| Black level | `BlackLevel` | `5.0` (float) |
| White balance mode | `BalanceWhiteAuto` | `"Continuous"` / `"Once"` / `"Off"` (string) |
| White balance ratio | `BalanceRatio` (+ `BalanceRatioSelector`) | `1.3` (float) |
| Frame rate | `AcquisitionFrameRate` | `25.0` (float) |
| Frame rate enable | `AcquisitionFrameRateEnable` | `true` (bool) |
| Auto target brightness | `AutoTargetBrightness` | `0.4` (float, 0–1) |
| Auto exposure upper limit | `AutoExposureTimeUpperLimit` | `2000.0` (µs, float) |
| Auto gain upper limit | `AutoGainUpperLimit` | `12.0` (float) |
| Light source preset | `LightSourcePreset` | `"Daylight5000K"` (string) |

> Node names must be the camera's exact SFNC names (case-sensitive); they vary slightly
> across models/firmware. If one is rejected, check the spelling in pylon Viewer.

**Geometry is locked while streaming:** `Width`, `Height`, `OffsetX/Y`, `PixelFormat`
cannot be changed by live commands — set them via the camera settings / `.pfs` at load.

---

## 6. Examples (copy-paste)

```jsonc
// Go manual: half exposure, low gain
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Exposure Time","value":0.5 } } }
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Gain","value":0.2 } } }

// Tweak a parameter during the manual session
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Gamma","value":1.1 } } }
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"AcquisitionFrameRate","value":20.0 } } }

// Trigger / sync
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Trigger Mode","value":false } } }
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Sync Role","value":"slave" } } }

// Finished testing -> back to the NATS-loaded settings (no value)
{ "messageBody": { "data": { "commandType":"camera_command","cameraId":1,"cameraSerial":"40012345","key":"Use Settings" } } }
```

---

## 7. Verifying / troubleshooting via the client log

Successful exposure command:

```
Application: -> camera_command id=1 key='Exposure Time'
CameraManager: command 'Exposure Time' -> camera 1
[exposure][40012345] MANUAL exposure norm=0.5 (cap=2000us, auto suspended)
```

Revert: `[exposure][40012345] reverted to load-time camera settings`
Raw parameter: `[param][40012345] Gamma <- 1.2`

| Symptom | Log line | Cause |
|---|---|---|
| Nothing happens | `[command] missing camera_serial for ...` | `cameraSerial` not sent. |
| Nothing happens | `CameraManager: command ... for unknown camera id '...'` | `cameraId` matches no camera. |
| Node not set | `[param][...] could not set 'X' ...` | Wrong node name, read-only (streaming-locked), or wrong type. |
| Command ignored | `Application: unknown command_type '...'` | `commandType` not `camera_command`. |

---

## 8. Quick reference

- Same envelope as recording/streaming: everything under `messageBody.data`.
- `commandType: "camera_command"`; always include `cameraId` **and** `cameraSerial`.
- `Exposure Time` / `Gain` are **0.0–1.0 normalized**; they put the camera in manual hold.
- `maxExposure` (NATS setting) is the exposure cap; auto-exposure follows scene brightness up to it.
- Any other `key` + `value` = set that Basler node by name.
- `Use Settings` (no value) returns the camera to the exact NATS-loaded config and resumes auto.
- These commands target Basler (pylon) cameras. IP/RTSP cameras ignore them.

---

## 8d. Vehicle direction (enter / exit) from plate size

For a low-mounted, plate-facing camera where the full vehicle isn't visible, direction is
inferred from the plate's apparent size across sightings: a plate that **grows** is approaching
(entering); one that **shrinks** is receding (exiting). The decision needs a sustained net trend
over several reads, so a brief reverse at low frame rate doesn't fire a false event, and each
plate commits once per pass (with a cooldown before it can be judged again).

The estimator decides the **physical** motion (approaching vs receding from the camera). The
per-camera **`Entry_Exit`** setting maps that to enter/exit:

- `Entry_Exit = 0` (entry-gate camera): a plate **approaching** the camera = **ENTER**, receding = EXIT.
- `Entry_Exit = 1` (exit-gate camera): a plate **approaching** the camera = **EXIT**, receding = ENTER.

The result is reported in the existing `plates_data` field `cars[].direction`:
`0` = unknown, `1` = entering, `2` = exiting.

Tuning (lpr-level settings):

| setting | type | default | meaning |
|---|---|---|---|
| `direction_enable` | int | 1 | 1 = infer enter/exit from plate growth; 0 = always report `0` |
| `direction_min_sightings` | int | 3 | plate reads required before a decision |
| `direction_min_growth` | float | 1.15 | last/first plate-size ratio that counts as a trend (>= for enter, <= 1/x for exit) |
| `direction_cooldown_sec` | int | 8 | after a decision, ignore the same plate this long (avoids re-counting a wiggle) |
| `direction_gap_sec` | int | 3 | gap since last sighting that starts a fresh pass |
| `direction_require_y` | int | 0 | 1 = also require vertical motion to agree (low camera: approaching plate drifts down) |

Per-decision log line (debug): `DetectionWorker[1]: direction 14m12366 -> ENTER`.

---

## 8c. On-demand screenshot command

Send a command (same envelope/nesting as the other commands — `commandType` nested under
`messageBody.data`, camera id tolerant to casing) to grab the current frame for a camera:

```json
{ "messageType":"command",
  "messageBody": { "data": { "commandType":"screenshot", "camera_id": 1 } } }
```

`commandType` accepts `screenshot`, `get_screenshot`, or `camera_screenshot`. The client
replies on subject `message.screenshot` with the most recent frame for that camera:

```json
{ "messageId":"...", "messageType":"screenshot",
  "messageBody": { "camera_id": 1, "timestamp":"2026-06-20T15:39:00",
                   "width": 2448, "height": 2048, "screenshot": "<jpeg>" } }
```

`screenshot` is JPEG (quality 35) in the same encoding as the `live`/`crud_image` streams
(byte-array by default). If no frame has been captured yet for that camera, the command is
logged and skipped (no reply). The returned frame is the raw current frame (no overlay).

---

## 8b. Camera start: `crud_image` (ROI polygon image)

When a camera delivers its **first frame**, the client publishes the full crude frame to
subject `message.crud` so the backend can let the operator draw the ROI polygon (plate
reading region):

```json
{ "messageId":"...", "messageType":"crud_image",
  "messageBody": { "camera_id": 1, "crud_image": "<jpeg>" } }
```

`crud_image` is JPEG quality 20 in the same encoding as the normal `live` stream
(byte-array by default). Sent once per camera at start (and again after a reconnect).
For Basler this is the full sensor frame (see `basler_sensor_aoi` below) — detection then
crops to the ROI in software, so `full_image` and `crud_image` both show the whole scene.

---

## 9. Settings reference (added in this rebuild)

These configuration keys were added for the camera-control, manual-live, and GigE-bandwidth
features. They are supplied through the normal NATS settings the client requests at startup
(see §1). Keys not listed here (`maxExposure`, `maxGain`, `minGain`, `continuous_exposure`,
`trigger_mode`, `CameraAddress`, `MonoCameraAddress`, `CamconfigFile`, `type_of_link`, …)
already existed and are unchanged.

### `lpr`-level settings (apply to the whole client)

| key | type | default | meaning |
|---|---|---|---|
| `manual_live_quality` | int | 20 | JPEG quality (1–100) of `live_manual_control` images |
| `manual_live_scale` | int | 2 | downscale divisor for manual-live images: 1 = full, 2 = half, 4 = quarter |
| `manual_live_base64` | int | 1 | 1 = `live_image` as base64 string (smaller); 0 = JSON byte-array (matches normal `live`) |
| `gige_link_mbytes_per_sec` | int | 90 | usable NIC bandwidth (MB/s); per-camera budget = this ÷ N cameras |
| `gige_bw_reserve_pct` | int | 18 | `GevSCBWR` bandwidth reserve % for packet resends (Basler recommends 10–30) |
| `gige_scpd_floor` | int | 2000 | single-camera inter-packet-delay floor, in timestamp ticks |
| `basler_sensor_aoi` | int | 0 | 0 = capture the full crude frame, crop ROI in software (full-scene `full_image`); 1 = hardware sensor AOI (smaller frames, less bandwidth, no full scene) |
| `evidence_overlay` | int | 1 | 1 = stamp `full_image` with the PC date/time (top-left) and draw detected plate boxes + text; 0 = clean unannotated frame |

### Per-camera settings (keyed by camera id)

| key | type | default | meaning |
|---|---|---|---|
| `camera_control` | string | `"pylon"` | `"pylon"` = camera-native continuous auto + free-run, triggers off; `"host"` = host auto-exposure + IR trigger sync |
| `mono_exposure_us` | int | 1000 | fixed exposure (µs) for the mono / IR plate camera |
| `mono_gain` | int | 5 | fixed gain for the mono / IR plate camera |

### Notes

- All of the above are read **once at startup**; changing them takes effect on the next run.
- `manual_live_*` apply only to the `live_manual_control` message (§4); the normal `live`
  stream keeps its own existing encoding/size.
- The manual-live stop timeout (~60 s after the last manual command) is currently a
  compile-time constant, not a setting.
- GigE bandwidth: after a run, sum each camera's logged `bandwidthAssigned` (`GevSCBWA`)
  across the cameras on one NIC and confirm the total stays below
  `gige_link_mbytes_per_sec × 1e6`. If incomplete-grab errors appear, lower
  `gige_link_mbytes_per_sec` (e.g. 80) to widen the inter-packet delay.


### Overexposed / washed-out plates (single camera)

License plates are **retroreflective**, so a single camera on whole-frame auto-exposure
over-exposes the plate: it meters the darker scene and drives exposure up until the bright
plate clips to white (unreadable). Clipped highlights cannot be recovered in software, so fix
it at capture:

- **`exposure_target`** (per-camera, 0..255, default 100): lower it to expose *for the plate*.
  Try **50** (or 40-70). The background goes darker, which is fine for OCR.
- **`maxExposure`** (per-camera µs, default 15000): lower the exposure cap (e.g. 4000-8000) so
  night/dark scenes can't push exposure long enough to blow out the plate.
- **`maxGain`** / **`minGain`**: keep gain modest; high gain also pushes highlights to clip.

If you have IR illumination, a short fixed exposure (manual exposure command, ~1000-2000 µs)
exposes only the IR-lit plate and is the most robust setup.


### Continuous brightness-driven exposure loop (host control)

For full control of exposure from image brightness (recommended with fixed, non-dimmable IR),
set the camera to **host** control mode: `camera_control = host`. The host runs a continuous
loop on every grabbed frame: it measures a robust brightness statistic over the metering region,
compares it to a target, and corrects **exposure first (up to the motion-blur cap), then gain** —
damped and step-limited with a deadband so it doesn't hunt. (In the default `pylon` mode the
camera's own on-sensor auto runs instead, which can't meter a custom percentile.)

The control law is unit-tested. Tunable per-camera settings (defaults preserve prior behaviour):

| setting | type | default | meaning |
|---|---|---|---|
| `exposure_target` | int | 100 | with percentile metering this is the level (0..255) the **plate's bright pixels** are held at. 100 is conservative (no clipping, plate ~mid). Raise toward 150-180 if plates look too dark; lower if any clipping remains |
| `exposure_percentile` | int | 85 | brightness statistic the loop controls. **85 meters the bright plate**, so a sunny day can't wash it out and night/cloudy stay optimized. 50 = median (meters the scene; reverts to the old wash-out behaviour) |
| `exposure_deadband` | int | 10 | leave the image alone while within this of target (anti-hunt) |
| `exposure_damping` | float | 0.6 | 0..1, fraction of the correction applied per step (lower = gentler) |
| `exposure_step_ratio` | float | 2.0 | max per-step exposure multiplier (clamps big jumps) |
| `exposure_ema` | float | 0.5 | brightness smoothing (lower = smoother/slower) |
| `exposure_interval_ms` | int | 1000 | minimum time between adjustments |
| `exposure_highlight_priority` | int (0/1) | 0 | 1 = expose for the brightest region (plate) and keep gain low; never gain-up a dark scene. Recommended for night/IR with `exposure_percentile=98`, `exposure_target=220` |
| `maxExposure` | int (µs) | 15000 | motion-blur cap: the loop raises exposure only up to here, then adds gain |
| `maxGain` / `minGain` | int | 25 / 5 | gain bounds (high gain adds OCR-hurting noise) |

Recommended starting point for a fixed-IR plate camera: `camera_control=host`,
`exposure_percentile=85`, `exposure_target=120`, `maxExposure=6000`, `maxGain=18`. Watch the
`[auto-exposure] … measured=… target=… -> exposure=…us gain=…` log line and adjust.


### Plate voting / de-duplication (how plates are sent)

Detected plates are not sent raw. Each read is grouped into a per-camera **pass** (consecutive
reads of the same plate, clustered by text similarity), and each pass is published to
`messages.plates_data` **exactly once**, using the **highest-confidence actual read** in that
pass. This prevents three problems seen in raw output: the same car sent several times with one
digit different, a synthesized "consensus" string the camera never actually read, and fast cars
being dropped. Priority is to **capture every plate**, so a single-frame car is still sent.

LPR-level settings (all optional; defaults shown):

| setting | type | default | meaning |
|---|---|---|---|
| `min_votes` | int | 1 | reads required before a pass may be sent. **1 = never drop a single-frame car.** Raise to 2 to require two frames (more robust, but the rare 1-frame car is missed). |
| `plate_similarity` | float | 0.90 | Jaro-Winkler threshold for treating two reads as the same plate. Lower = more aggressive merging of digit-wobble; higher = stricter. |
| `ocr_prob` | float | (sent) | minimum OCR confidence for a read to enter voting; reads below this are ignored. |
| `plate_pass_gap_ms` | int | 1500 | a quiet gap longer than this ends a car's pass. The same plate seen again after the gap is a **new** pass and is sent again; within the gap it stays one send. Lower it for very fast traffic (quicker re-capture); raise it if one slow car is being sent twice. |

Notes:
- A read that fails the plate format check (8 chars: 2 digits, 1 non-digit, 5 digits) is dropped as
  malformed (e.g. a 7-character mis-read), regardless of confidence.
- The sent text is always a string the OCR actually produced for that pass - never a blended value.
- Startup logs the active values: `Application: plate processor minVotes=… similarity=… minConf=… passGapMs=… (send-once-per-pass, best read)`.


#### Recommended for sunny/cloudy/night with one loop (no day-night clock)

Set `camera_control=host` and `exposure_percentile=85` (now the default). The loop then meters the
**bright plate** and holds it just below clipping, so it self-adapts:
- **Sunny:** the plate is the brightest thing in frame; metering it keeps exposure low -> not over-exposed.
- **Cloudy:** dimmer daylight is just a lower measurement; the loop raises exposure within `maxExposure`. No separate "cloudy" mode is needed.
- **Night (fixed IR):** the plate is the bright signal; the loop holds it optimally and `maxExposure` keeps the shutter short enough to freeze motion.

Tuning order: start `exposure_target=100`, `maxExposure=6000`, `maxGain=18`; if plates read too dark
raise `exposure_target` toward 150-180; if any wash-out remains lower it or lower `maxExposure`.
Adjustment is periodic (~`exposure_interval_ms`, default 1000 ms), not per-frame, because an exposure
write takes several frames to take effect - re-deciding every frame would oscillate.


#### Highlight-priority metering (best for night / fixed IR)

By default the loop drives the chosen percentile toward `exposure_target` symmetrically - if the
scene is dark it will add gain to reach the target. At night that means it pins exposure at
`maxExposure` and gain at max chasing an empty (correctly dark) road, which adds noise and motion
blur for no benefit. Set **`exposure_highlight_priority = 1`** to switch to highlight-priority:

- It exposes for the **brightest region** (the plate) and keeps **gain low** - it never raises gain
  to brighten a dark background.
- If the bright plate ever **clips**, it shortens exposure to recover it; otherwise exposure drifts
  up to `maxExposure` so an appearing plate gets the most (still-sharp) light.
- Use a high percentile and a near-clipping target: `exposure_percentile=98`, `exposure_target=220`.

This makes one loop handle sunny -> cloudy -> night without touching settings: bright scenes are
pulled down to stop wash-out, and dark scenes simply sit at the motion cap with low gain instead of
amplifying noise. If your IR is weak and night plates come out dark, raise `minGain` (highlight mode
will not do it for you, on purpose).


### Trigger modes & Reset behaviour

**Supported trigger configurations** (set via the `Sync Role` camera command, value is a string):
- `free` / `master` / `trigger-off` - free-run (TriggerMode off), master with ExposureActive strobe out.
- `slave` / `trigger-on` - hardware-triggered on **Line1**, rising edge, FrameStart. Requires a real pulse wired to Line1, or the camera waits and no frames arrive.
- `software` - software trigger. The capture loop now fires one software trigger per grab, so this mode produces frames (previously it armed but never fired).

The `Trigger Mode` camera command toggles TriggerMode on/off only (the source stays as configured). Its value is accepted as a bool, a number (1/0), or a string ("on"/"true") - numbers no longer cause the command to be dropped.

Not supported: input lines other than Line1, activations other than rising edge, and trigger selectors other than FrameStart.

**Reset / Use Settings** restores the camera to its load-time (NATS-configured) state. It now briefly stops grabbing while restoring, so acquisition-locked nodes - TriggerMode/Source and ROI/PixelFormat - are actually re-applied (they are read-only during a live grab, so previously a triggered camera would not return to free-run on Reset). The configured sync role is re-applied authoritatively and grabbing resumes; look for `reverted to load-time camera settings (grab stopped/restarted ...)`.
