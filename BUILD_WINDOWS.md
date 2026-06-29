# Building `lpr.exe` on Windows with Visual Studio

This is the end-to-end guide to compile the LPR client on Windows (x64) with Visual Studio + CMake
presets. The build produces a single executable, **`lpr.exe`** (CMake target `lpr_client`).

> The project is **x64 only**. Pylon, OpenVINO, ONNX Runtime and GStreamer are all 64-bit; the
> CMake script hard-errors on a 32-bit (x86) configuration.

---

## 1. What links into the build

| Component | How it's found | Required? |
|---|---|---|
| **OpenCV 4** (`ffmpeg` feature) | vcpkg manifest (`vcpkg.json`) | **Yes** (the full client needs it) |
| **nlohmann-json**, **eigen3** | vcpkg manifest | Yes (json falls back to the vendored header in `third_party/` if absent) |
| **Pylon / Basler SDK** | **NOT in vcpkg** (proprietary) - install Basler's pylon SDK separately; found via `PYLON_DEV_DIR` env (or `-DPYLON_ROOT=`) | Yes, for live Basler capture |
| **OpenVINO** | either the `openvino` **vcpkg feature** (builds from source), or a separately-installed runtime via `OpenVINO_DIR`; `-DWITH_OPENVINO=ON` | Yes, for the deep detector/OCR |
| **NATS (cnats)** | `nats` vcpkg feature; `-DWITH_NATS=ON` | Yes, for the backend transport |
| **GStreamer** | `gstreamer` vcpkg feature *or* a system SDK via `GSTREAMER_ROOT`; `-DWITH_GSTREAMER=ON` | Optional (RTSP backend) |
| **ONNX Runtime** | `-DONNXRUNTIME_ROOT=<sdk>` (`include/onnxruntime_cxx_api.h` + `lib/onnxruntime`) | Optional inference backend |
| **VLC (LibVLC)** | VideoLAN SDK; auto-skips if missing | Optional |

> **Only Basler is not installable via vcpkg.** OpenCV, OpenVINO, NATS, GStreamer (and json/eigen)
> can all be pulled by vcpkg at configure time (see the `full-vcpkg-all` preset below). Pylon is a
> proprietary Basler download; install it separately and CMake auto-detects it via `PYLON_DEV_DIR`.

Each backend **auto-skips** if its SDK isn't found (except where a preset explicitly forces it on, in
which case a missing SDK is a hard error — that's deliberate, so a "live" build can't silently lose
OpenVINO or NATS).

---

## 2. Prerequisites (install once)

1. **Visual Studio 2026** (or 2022) with these workloads/components:
   - *Desktop development with C++*
   - *C++ CMake tools for Windows* (gives the "Open Folder" CMake integration + Ninja)
   - The MSVC v143+ x64 toolset and a recent Windows SDK.
2. **vcpkg** — you already have it at `D:\full-vcpkg`. If setting up fresh:
   ```bat
   git clone https://github.com/microsoft/vcpkg D:\full-vcpkg
   D:\full-vcpkg\bootstrap-vcpkg.bat
   ```
   No need to `vcpkg install` anything by hand — the project uses **manifest mode** (`vcpkg.json`),
   so the toolchain installs OpenCV/json/eigen (and `cnats`/`gstreamer` when those features are on)
   automatically at configure time.
3. **Basler pylon SDK** (the "Development" components). The installer sets `PYLON_DEV_DIR`. Confirm:
   ```bat
   echo %PYLON_DEV_DIR%
   ```
   should print something like `C:\Program Files\Basler\pylon 8\Development`.
4. **OpenVINO runtime** (the archive/installer). It ships `setupvars.bat`, which sets `OpenVINO_DIR`.
5. **ONNX Runtime** (optional) — only if you want the ORT backend; note the folder for `ONNXRUNTIME_ROOT`.

---

## 3. One-time environment variables

The presets read three environment variables. Set them as **user environment variables** (so VS and
new terminals inherit them), or set them per-session in the terminal before configuring.

| Variable | Points to | Used by |
|---|---|---|
| `VCPKG_ROOT` | `D:\full-vcpkg` | every `*-vcpkg*` preset (toolchain file) |
| `OpenVINO_DIR` | `...\openvino\runtime\cmake` | OpenVINO presets (set automatically by `setupvars.bat`) |
| `PYLON_DEV_DIR` | Basler "Development" dir | Pylon discovery (set by the pylon installer) |

Set them permanently (run once, then open a **new** terminal):
```bat
setx VCPKG_ROOT      D:\full-vcpkg
setx OpenVINO_DIR    "C:\Program Files (x86)\Intel\openvino\runtime\cmake"
```
`PYLON_DEV_DIR` is set by the Basler installer; you normally don't need to touch it.

> If you prefer not to rely on `setupvars.bat`, point `OpenVINO_DIR` at the folder that contains
> `OpenVINOConfig.cmake` (usually `runtime\cmake`).

---

## 4. Pick a preset

| Preset | What it builds |
|---|---|
| `full-vcpkg` | full client, OpenCV-DNN only (no OpenVINO/NATS) — quickest sanity build |
| `full-vcpkg-ov` | full client + OpenVINO from a **separately-installed** runtime (`OpenVINO_DIR`) |
| `full-vcpkg-nats-gst` | full client + NATS + GStreamer |
| **`full-vcpkg-ov-nats-gst`** | live deployment: OpenVINO (separately-installed) + NATS + GStreamer |
| **`full-vcpkg-all`** | **everything via vcpkg: OpenVINO + NATS + GStreamer all pulled by vcpkg** (Basler still separate) |
| **`full-vcpkg-ov-gst`** | full-vcpkg **+ OpenVINO + GStreamer via vcpkg + Pylon** (auto-detected); **no NATS** - capture+inference sanity build |
| **`full-vcpkg-sdk`** | full-vcpkg **+ OpenVINO + GStreamer + Pylon from your INSTALLED SDK paths** (no source build) |
| `tests-only` | unit tests only, no OpenCV (CI / logic checks) |

Two ways to get OpenVINO:
- **`full-vcpkg-ov-nats-gst`** — you have the OpenVINO **runtime installed** (set `OpenVINO_DIR`, e.g. via `setupvars.bat`). Fast configure, no source build.
- **`full-vcpkg-all`** — vcpkg **builds OpenVINO from source** (CPU + IR frontend). No `OpenVINO_DIR` and nothing to pre-install, but the first configure is **slow** (OpenVINO + protobuf + tbb compile — tens of minutes to over an hour). One self-contained command, nothing to manage.

Either way **Basler is installed separately** (no vcpkg port); CMake finds it via `PYLON_DEV_DIR`.

### Adding OpenVINO + GStreamer + Pylon on top of `full-vcpkg`

`full-vcpkg` already has `WITH_PYLON`, `WITH_GSTREAMER` and `WITH_OPENVINO` set to **ON** - they only
"disappear" from a plain build because their SDKs aren't found, so CMake auto-skips them. Making them
active differs per library:

- **Pylon (Basler):** not a vcpkg package. Just install the Basler pylon SDK; CMake finds it through
  `PYLON_DEV_DIR`. Nothing to add to the preset - a plain `full-vcpkg` already compiles the Basler
  backend if the SDK is present.
- **GStreamer:** add the vcpkg feature so vcpkg installs it (`VCPKG_MANIFEST_FEATURES=...;gstreamer`).
- **OpenVINO:** add the vcpkg feature (`...;openvino`) - builds from source, slow first time - or use a
  separately-installed runtime via `OpenVINO_DIR`.

**Easiest (one preset):** use **`full-vcpkg-ov-gst`** - it sets `WITH_OPENVINO/GSTREAMER/PYLON=ON` and
`VCPKG_MANIFEST_FEATURES=openvino;gstreamer`. Pylon is auto-detected; NATS stays off.

```bat
:: CLI
cmake --preset       full-vcpkg-ov-gst
cmake --build --preset full-vcpkg-ov-gst
```

**Without a dedicated preset** (extend `full-vcpkg` with -D flags on the CLI):

```bat
cmake --preset full-vcpkg ^
  -DVCPKG_MANIFEST_FEATURES="openvino;gstreamer" ^
  -DWITH_OPENVINO=ON -DWITH_GSTREAMER=ON -DWITH_PYLON=ON
cmake --build --preset full-vcpkg
```

**In Visual Studio:** the `-D` route is awkward in the IDE, so prefer the preset. Open the folder,
pick **"Full + OpenVINO + GStreamer via vcpkg + Pylon (auto-detected); no NATS"** in the configuration
dropdown, and Build All. Make sure `PYLON_DEV_DIR` is set before launching VS (so the Basler SDK is on
the environment VS inherits). If you'd rather not switch presets, add the four cache variables above
via **Project -> CMake Settings**, or copy the preset block into a `CMakeUserPresets.json`.

**Confirm all three linked in** - watch the CMake configure output for these lines:
```
  + Pylon/Basler backend (pylon 8.x)
  + GStreamer backend (...)
  + detection backend: OpenVINO
```
If any shows the "- ... requested but not found / skipping" form instead, that library didn't resolve
(SDK/feature missing) and won't be in the binary.

### Pointing CMake at installed OpenVINO / GStreamer / Pylon (set the paths, then build)

If you already have these SDKs installed and do **not** want vcpkg to build them, point CMake at each
one. The variables are:

| SDK | CMake variable | Point it at | Env var alternative |
|---|---|---|---|
| OpenVINO | `OpenVINO_DIR` | the folder containing `OpenVINOConfig.cmake`, e.g. `<openvino>\runtime\cmake` | `OpenVINO_DIR` (set by `setupvars.bat`) |
| GStreamer | `GSTREAMER_ROOT` | the MSVC x86_64 root (contains `include\gstreamer-1.0`, `include\glib-2.0`, `lib\`), e.g. `C:\gstreamer\1.0\msvc_x86_64` | `GSTREAMER_1_0_ROOT_MSVC_X86_64` |
| Pylon | `PYLON_ROOT` | the Basler "Development" dir, e.g. `C:\Program Files\Basler\pylon 8\Development` | `PYLON_DEV_DIR` (set by installer) |

**CLI - explicit paths on `full-vcpkg` (most reliable):**
```bat
cmake --preset full-vcpkg ^
  -DWITH_OPENVINO=ON  -DOpenVINO_DIR="C:/Program Files (x86)/Intel/openvino/runtime/cmake" ^
  -DWITH_GSTREAMER=ON -DGSTREAMER_ROOT="C:/gstreamer/1.0/msvc_x86_64" ^
  -DWITH_PYLON=ON     -DPYLON_ROOT="C:/Program Files/Basler/pylon 8/Development"
cmake --build --preset full-vcpkg
```
(Use forward slashes, or escape backslashes. vcpkg still supplies OpenCV/json/eigen; OpenVINO and
GStreamer come from your installs, so there is **no slow source build**.)

**CLI - env vars + one preset:** set the three env vars (left table, "Env var alternative"), then:
```bat
cmake --preset       full-vcpkg-sdk
cmake --build --preset full-vcpkg-sdk
```
`full-vcpkg-sdk` forwards `OpenVINO_DIR`, `GSTREAMER_1_0_ROOT_MSVC_X86_64`, and `PYLON_DEV_DIR` from
the environment into the matching CMake variables.

**Visual Studio:** set the three env vars first (or run `setupvars.bat`) **before** launching VS so it
inherits them, then Open Folder -> pick **"Full + OpenVINO/GStreamer/Pylon from INSTALLED SDK paths
..."** -> Build All. To hard-code the paths instead of env vars, open **Project -> CMake Settings**
(or `CMakeUserPresets.json`) and set `OpenVINO_DIR`, `GSTREAMER_ROOT`, `PYLON_ROOT` directly.

After configure, confirm the three "+ ... backend" lines appear (see the box below). A `-` "not found"
line means that path was wrong.



---

## 5. Build it — Route A: Visual Studio "Open Folder" (GUI)

1. **File → Open → Folder…** and pick the project root (the folder with `CMakeLists.txt` and
   `CMakePresets.json`).
2. VS auto-detects the presets. In the toolbar **Configuration** dropdown choose
   **`full-vcpkg-ov-nats-gst`** (the display name is "Full + OpenVINO + NATS + GStreamer (live
   deployment)").
3. VS runs CMake configure automatically. Watch the **Output → CMake** pane — vcpkg will build
   OpenCV/cnats/etc the first time (this is slow, 20–40 min once; cached afterwards). Confirm the
   feature lines you expect appear, e.g.:
   ```
   + Pylon/Basler backend (pylon 8.x)
   + OpenVINO inference backend
   + NATS messaging transport
   ```
4. **Build → Build All** (or `Ctrl+Shift+B`).
5. The binary lands in the preset's build dir (see §7).

> If VS doesn't see the presets, make sure *C++ CMake tools for Windows* is installed, and that
> there's no stale `CMakeSettings.json` overriding presets.

---

## 6. Build it — Route B: Developer Command Prompt (CLI)

Open **"x64 Native Tools Command Prompt for VS 2026"** (this puts MSVC on `PATH`). Then:

```bat
cd /d E:\programs\Multi_Camera_Client\lpr

:: make sure the env vars from §3 are present in THIS shell
echo %VCPKG_ROOT%
echo %OpenVINO_DIR%

:: configure (first run also installs vcpkg deps; slow once)
cmake --preset full-vcpkg-ov-nats-gst

:: build Release
cmake --build --preset full-vcpkg-ov-nats-gst
```

That's it. The `--preset` form reads everything (toolchain, build type, features) from
`CMakePresets.json`, so there are no long `-D` flags to remember.

To build **only** the executable target:
```bat
cmake --build --preset full-vcpkg-ov-nats-gst --target lpr_client
```

To run the unit tests (logic only, no camera):
```bat
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only --output-on-failure
```

---

## 7. Where the output goes, and the DLLs next to it

The executable is written to:

```
<project>\build\full-vcpkg-ov-nats-gst\lpr.exe
```

(`binaryDir` is `${sourceDir}/build/${presetName}`; the target `lpr_client` has `OUTPUT_NAME lpr`.)

On Windows the build **auto-copies runtime DLLs next to `lpr.exe`** via post-build steps, so it runs
in place:
- `$<TARGET_RUNTIME_DLLS>` — OpenCV, cnats, json, etc. (vcpkg DLLs).
- The **OpenVINO** runtime bin directory **and TBB** (OpenVINO's own plugins aren't caught by the
  generic step, so they're copied explicitly).
- **GStreamer** plugins (`gstreamer-1.0\`) when the GStreamer feature is on.

**Pylon is the deliberate exception:** the build does **not** copy Pylon DLLs app-local, because
Pylon loads its transport-layer producers (`.cti`) from its **own installed runtime**. The Basler
runtime must therefore be installed on the machine and on `PATH` (the pylon installer does this). You
run the camera box with pylon installed — don't try to ship `lpr.exe` with copied Pylon DLLs, it will
fail to find its producers.

---

## 8. Running `lpr.exe`

The exe expects a few things **in its working directory**:
- TLS material next to the exe: `client-cert.pem`, `client-key.pem`, `ca.pem`.
- The model files under the path your settings specify (e.g. `training\detect_model_int8.xml`,
  `ocr_model_2.xml`, `car_model.xml`).
- The auth token in the environment (e.g. `AUTH_TOKEN`).

Because the DLLs are copied into the **build** folder, the simplest correct way to run is from that
folder, or copy the whole folder to your deploy location:

```bat
cd /d E:\programs\Multi_Camera_Client\lpr\build\full-vcpkg-ov-nats-gst
set AUTH_TOKEN=6f2f694c...d6
lpr.exe
```

> You have run **stale binaries** before by launching an old copy from `D:\full-vcpkg\`. After every
> rebuild, either run from the build folder above, or re-copy the freshly built `lpr.exe`
> **and** its sibling DLLs to wherever you launch it. Confirm you're on the new build by checking the
> startup banner / the new log lines you expect.

---

## 9. Troubleshooting

| Symptom at configure/build | Cause & fix |
|---|---|
| `error: 32-bit (x86) build is not supported` | You selected an x86 kit. Use the **x64** preset / "x64 Native Tools" prompt. |
| `CMAKE_TOOLCHAIN_FILE ... vcpkg.cmake not found` / vcpkg deps missing | `VCPKG_ROOT` not set in this shell. `echo %VCPKG_ROOT%`; set it (§3) and reopen the terminal. |
| `Pylon requested but SDK not found; skipping` | `PYLON_DEV_DIR` not set, or wrong. Pass `-DPYLON_ROOT="C:/Program Files/Basler/pylon 8/Development"` or fix the env var, then reconfigure. |
| OpenVINO not picked up | `OpenVINO_DIR` not set. Run the OpenVINO `setupvars.bat` in the shell first, or `setx OpenVINO_DIR ...\runtime\cmake`. |
| `WITH_NATS=ON but the cnats SDK was not found` | You're not using the NATS preset (which pulls `cnats` via vcpkg). Use `full-vcpkg-nats-gst` / `full-vcpkg-ov-nats-gst`, which set `VCPKG_MANIFEST_FEATURES=nats`. |
| `ONNX Runtime requested but not found` | Either pass `-DONNXRUNTIME_ROOT=<sdk>` or it's harmless (ORT is optional; OpenVINO is the live backend). |
| First configure is very slow | vcpkg is compiling OpenCV/cnats once. Subsequent configures are cached. |
| **At runtime:** `The code execution cannot proceed because XXX.dll was not found` | You launched the exe from a folder without its DLLs (e.g. old `D:\full-vcpkg\`). Run from the build folder, or copy the DLLs alongside it (§7). |
| **At runtime:** Pylon "no transport layer" / `.cti` errors | The Basler runtime isn't installed / not on `PATH` on this machine. Install pylon (its DLLs are intentionally not app-local). |
| Linker errors about `min`/`max` or `M_PI` | Already handled in-tree (`NOMINMAX`, `_USE_MATH_DEFINES`); if you see them you're likely building a TU outside this CMake — build via the preset. |

---

## 10. Quick reference

```bat
:: one-time
setx VCPKG_ROOT   D:\full-vcpkg
setx OpenVINO_DIR "C:\Program Files (x86)\Intel\openvino\runtime\cmake"

:: each build (x64 Native Tools Command Prompt)
cd /d E:\programs\Multi_Camera_Client\lpr
cmake --preset       full-vcpkg-ov-nats-gst
cmake --build --preset full-vcpkg-ov-nats-gst

:: ...or pull EVERYTHING (incl. OpenVINO) via vcpkg, no OpenVINO_DIR needed
:: (first configure is slow: OpenVINO builds from source). Basler still installed separately.
::   cmake --preset       full-vcpkg-all
::   cmake --build --preset full-vcpkg-all

:: run
cd /d E:\programs\Multi_Camera_Client\lpr\build\full-vcpkg-ov-nats-gst
lpr.exe
```

Output: `build\full-vcpkg-ov-nats-gst\lpr.exe` with all non-Pylon DLLs already beside it.
