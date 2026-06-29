# LPR Client — Build & Install (CMake)

Cross-platform CMake build for the LPR client. This guide covers a complete Windows
build (Visual Studio 2026 + vcpkg) with the OpenVINO, GStreamer, Pylon, and NATS
backends, plus a quick test-only build.

The executable is `lpr` (target `lpr_client`). On Windows the build automatically copies
the dependent DLLs next to `lpr.exe` (see [DLL bundling](#dll-bundling)).

---

## 1. Prerequisites

| Component | Needed for | Notes |
|---|---|---|
| **CMake ≥ 3.21** | everything | Bundled with VS2026, or install standalone. |
| **Visual Studio 2026** (Desktop C++ workload) | Windows build | MSVC toolset + Windows SDK. |
| **vcpkg** | OpenCV, nlohmann-json, eigen3, (optional cnats, gstreamer) | Set `VCPKG_ROOT` to the vcpkg checkout. |
| **OpenVINO Runtime** | `WITH_OPENVINO` | Set `OpenVINO_DIR` to the folder containing `OpenVINOConfig.cmake` (usually `<install>/runtime/cmake`). |
| **Basler pylon 8 SDK** | `WITH_PYLON` | Install on the build **and** target machine — Pylon is not app-local (see note). |
| **GStreamer** | `WITH_GSTREAMER` | Either the official GStreamer SDK on `PATH`, or the vcpkg `gstreamer` feature. |
| **cnats** | `WITH_NATS` | Easiest via the vcpkg `nats` feature; or point `-DNATS_INC`/`-DNATS_LIB` at a build. |
| **VC++ Redistributable** | running on a clean machine | Or build static-runtime; see [DLL bundling](#dll-bundling). |

Optional backends (off by default): `WITH_TENSORRT` (NVIDIA/Jetson), `WITH_HAILO`
(Raspberry Pi AI HAT+), `WITH_VLC` (RTSP via libVLC — the VideoLAN SDK is **not** in
vcpkg, install it separately), `WITH_ONNXRUNTIME` (on by default; cross-platform EPs).

### Environment variables

```bat
set VCPKG_ROOT=C:\src\vcpkg
set OpenVINO_DIR=C:\Program Files (x86)\Intel\openvino\runtime\cmake
```

(Use `setx` to persist them. `OpenVINO_DIR` is only needed for OpenVINO builds.)

---

## 2. Dependencies vcpkg installs for you

The `vcpkg.json` manifest pulls these automatically when you use a `*-vcpkg*` preset:

- Always: `opencv4` (with `ffmpeg`), `nlohmann-json`, `eigen3`
- Feature `gstreamer` → `gstreamer`
- Feature `nats` → `cnats`

Features are selected with `VCPKG_MANIFEST_FEATURES` (the presets below do this for you).
**OpenVINO, Pylon, and the official GStreamer SDK are installed separately** and detected
by CMake — they are not vcpkg packages.

---

## 3. Build presets

List them with `cmake --list-presets`.

| Preset | Toolchain | Backends enabled | Use for |
|---|---|---|---|
| `tests-only` | none | none (tests only) | Fast unit-test build, no SDKs. |
| `tests-debug` | none | none (tests, Debug) | Debugging the core libraries. |
| `full` | none (prebuilt deps) | auto-detect | Non-vcpkg: you provide OpenCV etc. via `-D*_DIR`. |
| `full-vcpkg` | vcpkg | OpenCV via vcpkg | Baseline production build. |
| `full-vcpkg-gst` | vcpkg | + GStreamer | RTSP via GStreamer. |
| `full-vcpkg-nats-gst` | vcpkg | + NATS + GStreamer | Live deployment without OpenVINO. |
| `full-vcpkg-ov` | vcpkg | + OpenVINO | Inference via OpenVINO. |
| `full-vcpkg-ov-nats-gst` | vcpkg | + OpenVINO + NATS + GStreamer | **Full production build.** |

All presets build to `build/<presetName>/`. `WITH_PYLON`, `WITH_OPENVINO`, etc. each
*auto-skip* if their SDK isn't found, so a missing optional SDK won't fail the build —
it just disables that backend (you'll see a `- <backend> ... skipping` status line).

---

## 4. Full Windows build (production)

From a *Developer Command Prompt for VS 2026* (so MSVC is on `PATH`), with `VCPKG_ROOT`
and `OpenVINO_DIR` set:

```bat
cd E:\programs\Multi_Camera_Client\lpr

:: Configure (vcpkg builds OpenCV + cnats on first run; this can take a while)
cmake --preset full-vcpkg-ov-nats-gst

:: Build Release
cmake --build --preset full-vcpkg-ov-nats-gst --config Release
```

The executable lands at `build\full-vcpkg-ov-nats-gst\Release\lpr.exe`, with its
dependent DLLs copied alongside it.

To **enable Pylon** (on by default; it auto-skips if the SDK isn't found) just have the
pylon 8 SDK installed before configuring — CMake's `find_package(pylon)` picks it up.

### Selecting backends manually

The presets are convenience bundles; you can override any option:

```bat
cmake --preset full-vcpkg ^
  -DWITH_OPENVINO=ON -DOpenVINO_DIR="%OpenVINO_DIR%" ^
  -DWITH_NATS=ON -DWITH_GSTREAMER=ON ^
  -DVCPKG_MANIFEST_FEATURES=nats;gstreamer ^
  -DWITH_PYLON=ON
cmake --build --preset full-vcpkg --config Release
```

> When enabling `WITH_NATS` or `WITH_GSTREAMER` via vcpkg, configure a **clean** build
> directory with the matching `VCPKG_MANIFEST_FEATURES` so vcpkg installs `cnats` /
> `gstreamer` first.

---

## 5. <a name="dll-bundling"></a>DLL bundling (Windows)

After a successful build the following are copied next to `lpr.exe` automatically:

- **All linked dependency DLLs** — OpenCV, ONNX Runtime, NATS, OpenVINO core, etc.
  (via CMake `$<TARGET_RUNTIME_DLLS>` + vcpkg's applocal step).
- **OpenVINO plugins + TBB** — `openvino_intel_cpu_plugin.dll`, `plugins.xml`, TBB.
  These are loaded *dynamically*, so they're copied explicitly from the OpenVINO runtime
  `bin` dir derived from `OpenVINO_DIR`. If your layout differs, override it:
  ```bat
  cmake --preset full-vcpkg-ov-nats-gst -DLPR_OPENVINO_RUNTIME_DIR="C:\path\to\openvino\runtime\bin\intel64"
  ```
- **GStreamer plugins** — bundled into `gstreamer-1.0\` next to the exe when GStreamer
  came from vcpkg. With the official GStreamer SDK, keep its `bin` on `PATH` instead.

**Not bundled, by design:**

- **Pylon/Basler** must stay **installed** on the target. The Pylon runtime loads GenTL
  transport-layer producers (`.cti`) and GenICam XML from its own install tree via its
  environment (`PYLON_ROOT` / `GENICAM_GENTL64_PATH`); copying its DLLs next to the exe
  breaks camera discovery. For a self-contained target, use Basler's official *pylon
  Deployment* redistributable.
- **VC++ runtime** — install the Visual C++ Redistributable on the target, or configure
  with a static runtime if you prefer a fully standalone exe.

---

## 6. Running

The client takes its file paths/model config via `--config`, and gets camera settings
either live over NATS or from an offline JSON file.

**Live (settings from NATS):**

```bat
lpr.exe --config D:\train\config_file.txt ^
        --nats nats://<host>:4222 --no-tls-first --tls-hostname ALPR ^
        --token <auth-token>
```

**Offline (settings from a JSON file, no broker):**

```bat
lpr.exe --config D:\train\config_file.txt --settings settings.json
```

Common flags: `--config`/env `LPR_CONFIG`, `--nats`/env `NATS_URL`, `--token`/env
`AUTH_TOKEN`, `--user` / `--password`, `--cert` / `--key` / `--ca` (TLS),
`--tls-first` / `--no-tls-first`, `--tls-hostname`, `--settings-subject`,
`--settings <file>` (offline), `--log-console`.

### TLS certificates and token (zero-config)

You don't need to pass the TLS files or token on every run:

- **Token** comes from the `AUTH_TOKEN` environment variable by default (`--token` overrides).
- **Mutual-TLS files** default to the exe's own folder. Drop `client-cert.pem`,
  `client-key.pem`, and `ca.pem` **next to `lpr.exe`** and they're picked up automatically,
  regardless of the working directory. Precedence per file: `--cert/--key/--ca` flag →
  `NATS_CERT_FILE`/`NATS_KEY_FILE`/`NATS_CA_FILE` env → the file beside the exe. A startup
  log line prints which TLS files resolved.

So a minimal production run is just:

```bat
set AUTH_TOKEN=<token>
lpr.exe --config D:\train\config_file.txt --nats nats://<host>:4222 --tls-hostname ALPR
```

(with the three `.pem` files sitting beside `lpr.exe`).

The `config_file.txt` holds the model paths and detection parameters (e.g.
`detect_model_int8.xml`, `ocr_model_2.xml`, `car_model.xml`, `deep_width`, `deep_height`,
`deep_detect_prob`, `ocr_prob`, `car_detection`, `show_live`, `debug`). Per-camera and
runtime settings (including the camera-control and bandwidth keys) come from NATS — see
**CAMERA_CONTROL_NATS_API.md** for the full settings reference.

---

## 7. Quick test build (no SDKs)

To build and run the unit tests without any camera/inference SDKs:

```bat
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only --output-on-failure
```

---

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `OpenVINOConfig.cmake not found` | Set `OpenVINO_DIR` to the folder that *contains* it (usually `<install>/runtime/cmake`). |
| `cnats SDK not found` (WITH_NATS) | Use a clean build dir with `-DVCPKG_MANIFEST_FEATURES=nats`, or pass `-DNATS_INC`/`-DNATS_LIB`. |
| RTSP cameras don't open | Ensure GStreamer plugins are present: vcpkg build bundles them next to the exe; official SDK must be on `PATH` (`rtspsrc`). |
| `lpr.exe` exits: missing `*.dll` | Confirm the post-build copy ran (rebuild). For OpenVINO plugins set `-DLPR_OPENVINO_RUNTIME_DIR`. For Pylon, install the SDK. |
| No Basler cameras found | Pylon SDK must be installed on the target (not app-local); check it's discoverable in pylon Viewer first. |
| `VCRUNTIME140.dll` missing on target | Install the VC++ Redistributable, or build with a static runtime. |

A backend that auto-skipped prints a `- <backend> requested but ... skipping` line at
configure time — check the configure output to confirm which backends are actually on.
