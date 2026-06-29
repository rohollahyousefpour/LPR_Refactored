# Building `lpr`

This is the full build reference for the clean LPR client. It covers every supported
situation, from a dependency-free unit-test build up to the full Windows pipeline with
OpenVINO + NATS + **GStreamer**. For *running* the binary and message formats, see
`RUNNING.md`, `MESSAGE_FORMATS.md`, and `DEBUG_NATS.md`.

---

## 1. What gets built

A set of static libraries (`lpr_core`, `lpr_log`, `lpr_config`, `lpr_capture`,
`lpr_detect`, `lpr_track`, `lpr_process`, `lpr_services`, `lpr_net`, `lpr_manager`,
`lpr_app`) and one executable, **`lpr`** (`lpr.exe` on Windows).

Most capabilities are optional and auto-detected: the build keeps going if an optional
SDK is missing and simply leaves that backend out. The configure log prints one line per
backend (`+ GStreamer backend`, `+ detection backend: OpenVINO`, …) so you can always see
what actually went into the binary.

---

## 2. Prerequisites

- A **64-bit** toolchain. The build hard-errors on a 32-bit (x86) compiler — every
  dependency (OpenCV, GStreamer, OpenVINO, ONNX Runtime) is 64-bit.
  - Windows: Visual Studio 2022/2026, "Desktop development with C++", **x64** prompt.
  - Linux: GCC ≥ 9 or Clang ≥ 12.
- **CMake ≥ 3.21** (presets v4).
- A C++17 compiler.
- **vcpkg** (recommended) for OpenCV / nlohmann-json / Eigen, with `VCPKG_ROOT` set.

---

## 3. Dependencies and how each is found

| Dependency | Role | Required? | How it's found | Option |
|---|---|---|---|---|
| OpenCV (`opencv4[ffmpeg]`) | imaging, video files, DNN | **Yes** for the client | vcpkg or system `find_package(OpenCV)` | — |
| nlohmann-json | settings/JSON | **Yes** | vcpkg | — |
| Eigen3 | ByteTrack Kalman maths | **Yes** (tracking) | vcpkg | — |
| OpenVINO | inference (Intel CPU/GPU) | optional | `find_package(OpenVINO)` — set `OpenVINO_DIR` | `WITH_OPENVINO` (ON) |
| ONNX Runtime | inference (portable) | optional | `find_path/library`, hint `ONNXRUNTIME_ROOT` | `WITH_ONNXRUNTIME` (ON) |
| OpenCV-DNN | inference (CPU/ARM fallback) | always | comes with OpenCV | — |
| TensorRT | inference (NVIDIA/Jetson) | optional | CUDA toolkit + nvinfer | `WITH_TENSORRT` (OFF) |
| Hailo | inference (RPi AI HAT+) | optional | `hailort` | `WITH_HAILO` (OFF) |
| **GStreamer** | **RTSP capture** | optional | pkg-config **or** `GSTREAMER_ROOT` | `WITH_GSTREAMER` (ON) |
| VLC (libVLC) | RTSP capture | optional | `find_package(LibVLC)` / `LIBVLC_SDK_DIR` | `WITH_VLC` (ON) |
| Pylon | Basler cameras | optional | `find_package(pylon)` | `WITH_PYLON` (ON) |
| cnats | NATS messaging transport | optional | `find_library(nats/cnats)` | `WITH_NATS` (OFF) |

"ON/OFF" is the default. ON means "use it if present, silently skip if not"; the NATS and
TensorRT/Hailo backends default OFF and must be turned on explicitly.

---

## 4. CMake options

```
LPR_BUILD_TESTS      ON    build unit tests (ctest)
LPR_BUILD_CONFIG_LIB ON    build the full library + lpr.exe (needs OpenCV); OFF = tests only
WITH_OPENVINO        ON    OpenVINO inference backend
WITH_ONNXRUNTIME     ON    ONNX Runtime inference backend
WITH_TENSORRT        OFF   TensorRT (NVIDIA/Jetson)
WITH_HAILO           OFF   Hailo NPU (Raspberry Pi AI HAT+)
WITH_GSTREAMER       ON    GStreamer RTSP capture backend
WITH_VLC             ON    VLC RTSP capture backend
WITH_PYLON           ON    Pylon/Basler capture backend
WITH_NATS            OFF   NATS messaging transport (cnats)
```

---

## 5. Presets

| Preset | Toolchain | Builds | Use |
|---|---|---|---|
| `tests-only` | system | tests only, no OpenCV | fast logic CI |
| `tests-debug` | system | tests, Debug | debugging the logic libs |
| `full` | system OpenCV | full lib + `lpr` | Linux dev box with system deps |
| `full-vcpkg` | vcpkg toolchain | full lib + `lpr` | **the main path** (Windows + Linux) |
| `full-vcpkg-gst` | vcpkg toolchain | full lib + `lpr`, GStreamer via vcpkg feature | RTSP capture, no separate SDK |
| `full-vcpkg-nats-gst` | vcpkg toolchain | full lib + `lpr`, NATS + GStreamer via vcpkg features | live server + RTSP, one command |
| `full-vcpkg-ov` | vcpkg toolchain | full lib + `lpr`, OpenVINO | Intel inference; reads `OpenVINO_DIR` from env |
| `full-vcpkg-ov-nats-gst` | vcpkg toolchain | full lib + `lpr`, OpenVINO + NATS + GStreamer | **live deployment**, one command |

The `*-ov*` presets pull `OpenVINO_DIR` from the **environment** (`$env{OpenVINO_DIR}`), which
OpenVINO's `setupvars.bat` / `setupvars.sh` sets. Run that script first, then the preset finds
OpenVINO automatically; if the variable is unset the build simply omits OpenVINO (no error) and
falls back to ONNX Runtime / OpenCV-DNN. The `*-gst` presets pull GStreamer through the vcpkg
manifest feature (heavy first build). On Windows you can instead use plain `full-vcpkg` with the
official MSVC SDK (F3 below) — it needs no vcpkg feature.

```bash
cmake --preset full-vcpkg          # configure
cmake --build --preset full-vcpkg  # build
ctest --preset tests-only          # run tests (or use the full build dir)
```

The `full-vcpkg` preset reads `CMAKE_TOOLCHAIN_FILE` from `$VCPKG_ROOT`, so set that env
var first. You can pass any of the options above with `-D` on the configure line.

---

## 6. Build situations

### A. Tests only (no OpenCV, no SDKs)
The logic libraries and their unit tests, nothing else. Good for CI.
```bash
cmake --preset tests-only && cmake --build --preset tests-only && ctest --preset tests-only
```

### B. Full build, system OpenCV (Linux)
```bash
sudo apt install libopencv-dev nlohmann-json3-dev libeigen3-dev
cmake --preset full && cmake --build --preset full
```

### C. Full build, vcpkg (Windows + Linux) — main path
```powershell
set VCPKG_ROOT=C:\vcpkg
cmake --preset full-vcpkg
cmake --build --preset full-vcpkg
```
vcpkg installs `opencv4[ffmpeg]`, `nlohmann-json`, `eigen3` from `vcpkg.json` automatically.

### D. + OpenVINO inference
Install the OpenVINO runtime, then either run its `setupvars` script (which sets the
`OpenVINO_DIR` environment variable) or point CMake at the folder that **contains
`OpenVINOConfig.cmake`** (usually `<openvino>/runtime/cmake`):
```powershell
:: option 1 - run setupvars once, then use the ready-made preset:
"C:\Program Files (x86)\Intel\openvino_2024\setupvars.bat"
cmake --preset full-vcpkg-ov && cmake --build --preset full-vcpkg-ov
:: option 2 - pass the path explicitly on any preset:
cmake --preset full-vcpkg -DOpenVINO_DIR="C:/Program Files (x86)/Intel/openvino_2024/runtime/cmake"
```
Configure log should show `+ detection backend: OpenVINO`. If `OpenVINO_DIR` is empty/unset
the build omits OpenVINO and falls back to ONNX Runtime / OpenCV-DNN (no hard error).

### E. + NATS messaging (live server)
cnats is **not** pulled by default. With vcpkg, enable the manifest feature on a **clean**
build dir so cnats installs *before* configure, then turn the backend on:
```powershell
cmake --preset full-vcpkg -DVCPKG_MANIFEST_FEATURES=nats -DWITH_NATS=ON
```
`src/net/NatsTransport.cpp` only compiles when `WITH_NATS=ON`; without it the client builds
but cannot reach a NATS server. (This is why CI/sandbox builds leave NATS off.)

### F. + GStreamer (RTSP capture)  ← this is the one you asked about
The GStreamer backend is `WITH_GSTREAMER=ON` by default; it only needs the GStreamer **dev**
libraries to be findable. There are three ways to provide them.

**F1 — Linux (system packages, pkg-config).** Easiest.
```bash
sudo apt install \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav
cmake --preset full && cmake --build --preset full
```

**F2 — Windows / cross-platform via vcpkg feature.** Pulls GStreamer through vcpkg (heavy,
but no manual SDK):
```powershell
cmake --preset full-vcpkg -DVCPKG_MANIFEST_FEATURES="gstreamer" -DWITH_GSTREAMER=ON
```
(Combine features with a semicolon, e.g. `-DVCPKG_MANIFEST_FEATURES="gstreamer;nats"`.)
Shortcut: the ready-made presets do exactly this —
```powershell
cmake --preset full-vcpkg-gst            && cmake --build --preset full-vcpkg-gst
cmake --preset full-vcpkg-nats-gst       && cmake --build --preset full-vcpkg-nats-gst
```

**F3 — Windows with the official GStreamer MSVC SDK (recommended on Windows).**
Install **both** the *runtime* and the *development* MSVC 64-bit packages from
gstreamer.freedesktop.org (default location `C:\gstreamer\1.0\msvc_x86_64`). The installer
sets the `GSTREAMER_1_0_ROOT_MSVC_X86_64` environment variable. CMake now uses that (or an
explicit `GSTREAMER_ROOT`) directly — **no pkg-config required**:
```powershell
:: usually nothing extra is needed; if the env var isn't set, pass the root yourself:
cmake --preset full-vcpkg -DGSTREAMER_ROOT="C:/gstreamer/1.0/msvc_x86_64"
```

**Verify it went in.** The configure output must contain one of:
```
  + GStreamer backend (1.24.2)
  + GStreamer backend (Windows SDK: C:/gstreamer/1.0/msvc_x86_64)
```
If instead you see `- GStreamer requested but not found; skipping`, the dev libs weren't
found — install them (F1/F2/F3) or set `GSTREAMER_ROOT` / `PKG_CONFIG_PATH`.

### G. + Pylon / Basler cameras
Install the Basler pylon SDK; CMake auto-detects it (`+ Pylon/Basler backend`). `WITH_PYLON`
is ON by default and skips cleanly when the SDK is absent.

### H. + VLC RTSP
Install the VideoLAN SDK (not in vcpkg) and set `LIBVLC_SDK_DIR`; `+ VLC backend`.

---

## 7. The full Windows build (vcpkg + OpenVINO + NATS + GStreamer)

This is the configuration the live deployment uses. From an **x64** Developer prompt:

```powershell
set VCPKG_ROOT=C:\vcpkg
:: 1) install the GStreamer MSVC runtime + devel packages (F3 above) — once.
:: 2) run OpenVINO setupvars so OpenVINO_DIR is in the environment — once per shell:
"C:\Program Files (x86)\Intel\openvino_2024\setupvars.bat"
:: 3) one command (configure installs the vcpkg features on a clean build dir):
cmake --preset full-vcpkg-ov-nats-gst
cmake --build --preset full-vcpkg-ov-nats-gst
```
Equivalent explicit form (if you prefer flags over the preset):
```powershell
cmake --preset full-vcpkg ^
  -DVCPKG_MANIFEST_FEATURES="nats" ^
  -DWITH_NATS=ON ^
  -DWITH_GSTREAMER=ON ^
  -DOpenVINO_DIR="C:/Program Files (x86)/Intel/openvino_2024/runtime/cmake"
cmake --build --preset full-vcpkg
```
Expected configure lines:
```
  + GStreamer backend (...)            <- F3 via GSTREAMER_1_0_ROOT_MSVC_X86_64
  + detection backend: OpenVINO
  + detection backend: ONNX Runtime
  + messaging: NATS transport (cnats found: ...)
```
Binary: `build/full-vcpkg/lpr.exe` (with a matching `.pdb` in Release for VS Code debugging).

---

## 8. Runtime requirements

The build links the libraries; at run time their **DLLs and plugins** must be reachable.

- **PATH** must include the OpenVINO `runtime/bin/intel64/Release`, the OpenCV bin (vcpkg
  `installed/x64-windows/bin`), the cnats bin, and the **GStreamer bin**
  (`C:\gstreamer\1.0\msvc_x86_64\bin`).
- **GStreamer plugins.** The RTSP backend needs these plugins available at run time:
  `rtspsrc`, `rtph264depay` / `rtph265depay`, `h264parse` / `h265parse`, `decodebin`,
  a decoder (`avdec_h264` from `gstreamer1.0-libav`, or a HW decoder), `videoconvert`,
  `appsink`. These ship in plugins-good / plugins-bad / libav. If GStreamer can't find its
  plugins, set `GST_PLUGIN_PATH` to `...\msvc_x86_64\lib\gstreamer-1.0`. Quick check:
  `gst-inspect-1.0 rtspsrc`.
- **Models** and the **config file** referenced by `--config` (detector / OCR / car model
  paths), plus a reachable NATS server for live messaging.

---

## 9. Using GStreamer at run time

A camera uses the GStreamer backend when its `type_of_link` is `"gstreamer"` and its
`CameraAddress` is an RTSP URL:

```
type_of_link = "gstreamer"
CameraAddress = "rtsp://user:pass@192.168.1.50:554/stream1"
```

The backend builds an RTSP pipeline internally
(`rtspsrc → rtp*depay → h26*parse → decode → videoconvert → appsink`), auto-selecting H.264
or H.265 from the stream and falling back to `decodebin` for other codecs; decoded frames
arrive as `cv::Mat` BGR, identical to the other capture backends. `type_of_link="video"`
(OpenCV) and `"rtsp"` (VLC) are unchanged and need no GStreamer.

---

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `x86/x64` fatal error at configure | You're in a 32-bit prompt — use the **x64** Developer Command Prompt. |
| `- GStreamer requested but not found` | Dev libs missing. Linux: install `libgstreamer1.0-dev` + plugins-base dev (F1). Windows: install the MSVC **devel** package and set `GSTREAMER_ROOT`, or use the vcpkg feature (F2). |
| Builds, but RTSP camera fails at run time with "no element rtspsrc / decoder" | GStreamer **plugins** not on the run-time path. Install plugins-good/bad/libav; set `GST_PLUGIN_PATH`; verify with `gst-inspect-1.0 rtspsrc`. |
| `OpenVINOConfig.cmake` not found | Point `-DOpenVINO_DIR` at the folder that *contains* it (`<ov>/runtime/cmake`). |
| Links but can't reach NATS | `WITH_NATS` was OFF, so `NatsTransport.cpp` wasn't compiled. Rebuild per §E. |
| cnats not found with vcpkg | Use a **clean** build dir and pass `-DVCPKG_MANIFEST_FEATURES=nats` so cnats installs before configure. |
| App starts but no DLL found | Add the OpenVINO / OpenCV / GStreamer / cnats `bin` folders to `PATH`. |

---

## 11. CI / sandbox note

Continuous builds run the Linux `full` preset with **NATS off** (so `NatsTransport.cpp`,
which needs the cnats SDK, isn't compiled) and GStreamer on via system packages. The
unit-test suite (`ctest`, 24 tests) is backend-independent and must stay green on every
change.
