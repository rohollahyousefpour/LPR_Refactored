# LPR — clean project, built module by module

A fresh, clean codebase grown one module at a time. Each step ports a module
from the legacy project, gives it clean names, wires it into CMake, and adds
tests that run with no heavy SDKs where possible.

## Layout
```
lpr/
├── CMakeLists.txt          top-level build
├── CMakePresets.json       "tests-only" (no OpenCV) and "full" (OpenCV/vcpkg)
├── cmake/                  find-modules (added as modules need them)
├── third_party/nlohmann/   vendored json single header (test fallback)
├── include/lpr/            public headers
│   ├── Log.h               real logger (Logger + LOG* macros), impl in src/log
│   └── config/             AppConfig, LprSettings, CameraSettings,
│                           CameraLineCache, RoiMaskCalculator, SettingsManager, JsonAbi
├── src/config/             implementations
└── tests/config/           unit tests
```

## Steps
- [x] **config** — `AppConfig` (local key=value file, boost-free), and the JSON
      settings stack: `LprSettings`, `CameraSettings`, `CameraLineCache`,
      `RoiMaskCalculator`, `SettingsManager` (renamed from `SettingsManager_`).
      All under `namespace lpr`; debug/ABI probes removed; logging via `lpr/Log.h`.
      Tests: `app_config`, `lpr_settings` (green, no OpenCV).
- [x] **logging** — `lpr/Log.h` is now a real, dependency-free `Logger` (replaces the
      Boost.Log `AppLogger`): same LOGT/D/I/W/E/F macros, level filter, timestamps,
      console + daily/size rotating file output, thread-safe. Lib `lpr_log`; the full
      config lib links it (settings files log through it). Test: `log` (green).
- [x] **core utils** — `Base64`, `JaroWinkler` (ported, namespaced), plus boost-free
      `Uuid` (RFC-4122 v4), `Time` (std::chrono), and `CapturePathBuilder` (was
      `PathAndUUIDGenerator`, rewritten on std::filesystem). Compiled into `lpr_core`.
      Tests: base64, jaro_winkler, uuid, capture_path (green). NOTE: `manage_time`
      (license/time-trial + `encp` crypto) deferred to a later **security** module.
- [x] **queue** — `lpr::BlockingQueue<T>` (header-only, STL-only): unifies the
      old `concurrent_queue` (bounded, drop-oldest) and `Send_Que_Data`
      (unbounded) into one template; adds `close()` for clean shutdown. Test:
      `blocking_queue` (green). The `FrameQueue`/`PlateQueue` aliases arrive with
      their item types: `FrameItem` (cv::Mat) at the capture step, `PlateItem`
      at the detection step.
- [~] **capture** — SDK-free parts DONE: `CameraKind`/`parseCameraKind` (test
      `camera_kind`), `CaptureSource` interface (OpenCV-free header, std::function
      callbacks instead of boost::signals2), `FrameItem` + `FrameQueue =
      BlockingQueue<FrameItem>`, and the `CameraSourceFactory` dispatch skeleton.
      Lib `lpr_capture` (auto-enabled when OpenCV is found). Test `capture_flow`
      exercises onFrame -> FrameItem -> FrameQueue end-to-end with a mock source.
      TODO: concrete sources -- `VideoFileCaptureSource` (OpenCV-only) next, then
      VLC / GStreamer / Pylon behind `LPR_WITH_*`.
- [ ] **detection / ocr** — `PlateRecognizer` strategies (OpenVINO, ...).
- [ ] **messaging** — `NatsTransport` + `ClientGateway`.
- [x] **orchestrator (camera side)** — `CameraWorker` (clean `detection`): owns one
      CaptureSource, motion-gates frames (faithful 3-frame absdiff/erode), forwards
      accepted frames to a sink, reconnects with exponential backoff; fixes the
      original detached+deleted thread leaks and centralizes source lifecycle.
      `CameraManager` (camera-owning half of `Managment_Cameras`): owns N workers,
      fans frames into one shared FrameQueue, start/stop all. Decoupled from
      messaging/detection via std::function callbacks. Lib `lpr_manager`. Test
      `camera_manager` covers reconnect + 2-camera fan-in with mock sources.
      NOTE: the detection/OCR/tracking/recording pipeline stays in a later module.

- [ ] **app** — `main()` wiring it all together.

## Build
```
cmake --preset tests-only && cmake --build --preset tests-only && ctest --preset tests-only
# full library (needs OpenCV; vcpkg toolchain optional):
cmake --preset full && cmake --build --preset full
```

## Notes / renames this step
- `Config`            -> `lpr::AppConfig`  (constructor param `adress` -> `configPath`; boost::split removed)
- `SettingsManager_`  -> `lpr::SettingsManager`  (file + class)
- `JsonConfig.h`      -> `lpr/config/JsonAbi.h`
- `AppLogger.h`       -> `lpr/Log.h` (seam)

- [x] **Basler full facade** (vendored) — the complete `Basler/` subsystem (22 files:
      Core/CameraContext/CameraDevice, CapturePipeline, ConnectionSupervisor,
      ExposureStrategies, GigEBandwidthCoordinator, SyncConfigurators, CommandQueue,
      StereoRectifySink) + `sunset` + `AutoRoadStereoRectifier`, brought into
      `include/lpr/basler` + `src/basler` as lib `lpr_basler`, guarded by `WITH_PYLON`
      (+ found SDK). Three compat shims map it onto the clean project unchanged:
      `virtual_cap_url.h` (legacy send_frame/send_cap -> CaptureSource emitFrame/Error),
      `AppLogger.h` (-> lpr/Log), `SettingsManager_.h` (-> lpr::SettingsManager).
      Verified: sunset + rectifier compile; the compat shim compiles+runs against
      CaptureSource. The Pylon facade itself compiles on a machine with the pylon SDK.
      NOTE: not wired into CameraSourceFactory (would make lpr_capture depend on
      lpr_basler); the full facade `BaslerCamera` is created from the app/manager layer.

- [x] **orchestrator + capture hardening (review bundle a-d + all flagged issues)**:
      (a) `CameraManager::buildFromSettings()` bridges to SettingsManager (camera ids
      + per-camera type/address/mono/delay/deviation); (b) backoff now RESETS after a
      stable session (was escalating forever) and `CameraWorker` distinguishes clean
      end-of-stream (stop, no reconnect) from error (reconnect) -- fixed a related
      start/stop lifecycle bug (join self-stopped supervisor) caught by a new
      `clean-end` test; (c) ROI-aware motion (runtime `setRoi`) + `handleCommand`
      passthrough + first-frame hook on `CameraWorker`; (d) GStreamer no longer self-
      reconnects (single-shot; CameraWorker owns reconnect uniformly). Source fixes:
      VLC now uses libVLC FORMAT callbacks (no 2nd probe instance) and emits from the
      display callback (event-driven, no 300ms poll); GStreamer clones frames before
      emit; Video opens on the worker thread (non-blocking) with clean EOF + optional
      loop. lpr_manager gated on LPR_BUILD_CONFIG_LIB. Tests: full 11/11, tests-debug
      10/10. VLC "RV24" (RGB) is now converted to OpenCV BGR in display() (revert to a plain
      copyTo if a camera ever shows inverted colors). The "full" preset now emits a
      clear, actionable error when OpenCV is missing instead of a cryptic CMake one.

- [x] **detection/OCR backbone (multi-backend, multi-platform)**: backend-agnostic
      `InferModel` interface (`include/lpr/detect/`) with three engines behind one API:
      `OpenCvDnnModel` (always; portable CPU incl. ARM/Raspberry Pi), `OpenVinoModel`
      (WITH_OPENVINO, Intel CPU/GPU/NPU), `TensorRtModel` (WITH_TENSORRT, NVIDIA/Jetson
      -- skeleton). `makeInferModel(Backend)` factory auto-selects TensorRT->OpenVINO->
      OpenCV-DNN among those compiled in (Auto on a Pi -> OpenCV-DNN). Shared, engine-
      independent post-processing: `ctcGreedyDecode` (CtcDecoder) + `PlateOcr` (crop ->
      blob -> infer -> CTC). Types: `PlateResult`, `PlateItem`, `PlateQueue`. CMake adds
      WITH_OPENVINO (find_package OpenVINO; pass -DOpenVINO_DIR=<pkg>/cmake) and
      WITH_TENSORRT (find nvinfer+CUDA), plus an ARM-target notice. Verified in-sandbox:
      OpenVINO 2026.2 backend compiles+links+runs (full 13/13); DNN-only "Raspberry Pi"
      config (WITH_OPENVINO/TENSORRT=OFF) compiles+passes; CTC decoder unit-tested
      (blank-first/last, repeat-collapse, all-blank). NEXT (needs model files to validate
      at runtime): EAST text-detection geometry decode + the full recognize() pipeline
      (FrameQueue -> detect -> OCR -> PlateQueue) + plate validation/tracking; flesh out
      TensorRtModel::infer for a concrete engine's I/O on a CUDA box.

- [x] **detection backends extended (ONNX Runtime + Hailo NPU)**: in response to "use the
      fastest runtime / NPU on Raspberry Pi / ONNX". Clarified that OpenCV-DNN is only the
      no-dependency fallback (and cannot drive an NPU). Added two backends behind the same
      `InferModel` interface: `OnnxRuntimeModel` (WITH_ONNXRUNTIME, default ON) -- one .onnx,
      many execution providers (CPU, XNNPACK for fast ARM CPU, CUDA, TensorRT, OpenVINO),
      requested-EP-with-CPU-fallback; and `HailoModel` (WITH_HAILO, off) -- Raspberry Pi
      AI HAT+ / Hailo-8/8L NPU via HailoRT + .hef (skeleton; proprietary SDK). Backend enum,
      toString/parseBackend, and factory updated; Auto priority TensorRT -> OpenVINO -> Hailo
      -> ONNX Runtime -> OpenCV-DNN. CMake: WITH_ONNXRUNTIME via find_library/find_path
      (-DONNXRUNTIME_ROOT=<sdk>; avoids the ORT tarball's broken CMake config), WITH_HAILO via
      find hailort; ARM message now points at Hailo/ORT first. Verified in-sandbox with ORT
      1.26: builds alongside OpenVINO, full 13/13, AND a real inference smoke (Relu model:
      output 1x3x4x4, negatives->0, max preserved) confirming blob->tensor->Run->cv::Mat.
      Per-device fastest: NVIDIA/Jetson=TensorRT, Intel=OpenVINO, Pi NPU=Hailo, generic/ARM
      CPU=ONNX Runtime(XNNPACK).

- [x] **TensorRT backend completed + compile-validated**: replaced the `infer()` stub with a
      full implementation using the modern name-based API (TensorRT 8.5+/10.x): deserialize
      engine -> execution context -> enumerate I/O tensors -> per-call setInputShape ->
      (re)allocate device buffers sized to resolved shapes -> setTensorAddress -> H2D ->
      enqueueV3 -> D2H -> n-dim CV_32F cv::Mat outputs (dynamic-shape aware, FP32 I/O).
      Installed TensorRT 10 + CUDA 12.6 *headers* in the sandbox and confirmed the backend
      COMPILES against them (no GPU here, so it can't be run/linked with the full runtime;
      that's the one backend still needing on-hardware run validation). CMake detects it via
      find_library(nvinfer)+find_path(NvInfer.h)+find_package(CUDAToolkit) and gracefully
      skips when absent (verified: WITH_TENSORRT=ON alongside OpenVINO+ONNX -> full 13/13).

- [x] **x86/x64 build guardrail**: a user's full-vcpkg build failed at link with 47 unresolved
      GStreamer/GLib externals + LNK4272 "machine type x64 conflicts with x86" -- root cause was
      an accidental 32-bit (x86) toolchain (x86 Native Tools prompt -> vcpkg pulled x86 OpenCV)
      while GStreamer/etc. are x64. Fix is to use the x64 Native Tools prompt + clean build dir.
      Added a fail-fast CMake guard: MSVC + 4-byte pointer -> FATAL_ERROR with exact remediation,
      so a 32-bit configure stops immediately instead of erroring deep in the linker.

- [x] **EAST plate detection + full recognize pipeline implemented** (the gap the user found:
      EAST was previously deferred; only the generic PlateOcr stage existed). Ported the
      original EastDetector faithfully -- its custom RBOX geometry decode, NHWC input,
      (pixel/127.5-1) normalization, config-driven distance scales (deep_plate_height/
      width_1/width_2), score+NMS thresholds, and fourPointsTransform crop -- but now running
      inference through the backend-agnostic InferModel, so detection works on OpenVINO/
      TensorRT/ONNX/DNN too. Files: include/lpr/detect/EastTextDetector.h + src (free function
      decodeEast() for testability), PlateRecognizer.{h,cpp} wiring detect -> crop -> PlateOcr
      -> PlateResult. Verified: decodeEast unit test (synthetic score/geometry -> box center
      (16,12) size 60x20); AND a real end-to-end smoke -- a 2-output ONNX model run through
      EastTextDetector::detect via ONNX Runtime produced the expected box + 60x20 crop. Full
      suite 14/14. REMAINING for real-model use: confirm THEIR model output names/order and
      NHWC-vs-NCHW after ONNX/engine conversion, OCR alphabet/blankIndex; port plate
      validation/tracking (check_plate + Jaro-Winkler dedup + AlprPlate history); wire
      PlateRecognizer into a FrameQueue->PlateQueue worker. All need their model files.
- [x] **DetectionWorker (pipeline spine) implemented** -- the first concrete piece of the
      Managment_Cameras decomposition. ONE clean worker replaces the original's
      run_plate_detection + the four near-duplicate plate_detection_* variants. It owns no
      business rules; it pulls FrameItems off a shared FrameQueue (via the new timed
      popFor()), and routes them to collaborators through seams designed so each neighbour
      from the decomposition plugs in WITHOUT DetectionWorker depending on its concrete type:
        * recognizer : IPlateRecognizer&     (PlateRecognizer today; a fake in tests)
        * processor  : PlateProcessor seam    std::function<optional<PlateResult>(PlateResult&&)>
                       -- can TRANSFORM or DROP a plate -> future PlateProcessor + ObjectTracker
                       (dedup/validate/canonicalise/attach track-id). setPlateFilter() adapts a
                       plain bool predicate onto it.
        * observers  : FrameObserver list     std::function<void(const FrameItem&)>, run per
                       non-empty frame -> future RecordingService + LiveViewService
        * sink       : PlateSink              std::function<void(PlateItem&&)> -> PlateQueue today
                       (setPlateQueue convenience) or future PlateSender.
      Lifecycle: start()/stop() own a worker thread; stop() is idempotent and safe on a
      shared, un-closed queue; the loop exits cleanly when the queue is closed AND drained.
      Counters framesProcessed()/platesEmitted(). Files: include/lpr/manager/DetectionWorker.h
      + src/manager/DetectionWorker.cpp; added to lpr_manager (links lpr_detect). Note: a GCC
      aggregate quirk made `Config cfg = {}` / `= Config{}` as a DEFAULT ARGUMENT illegal
      (nested struct's default member initializer can't be evaluated mid-class) -- fixed with a
      delegating convenience ctor instead. Test tests/manager/test_detection_worker.cpp drives
      all four seams: a FakeRecognizer emits 1 plate/non-empty frame; a processor that DROPS
      gate "skip" and REWRITES kept text ("OK-<gate>"); a frame observer counting non-empty
      frames; output to a PlateQueue. Pushes 3 valid + 1 empty + 1 skip and asserts
      framesProcessed==5, observed==4, platesEmitted==3, queued==3, text rewritten, clean stop.
      Full suite 15/15. NEXT in the decomposition: PlateProcessor + ObjectTracker (the processor
      seam's real implementation), then RecordingService + LiveViewService (the observer seam).

- [x] **vehicle-detect -> track -> plate pipeline (plate_detection_with_car)** -- the second
      slice of the Managment_Cameras decomposition, delivered as three clean, separately
      testable pieces plus wiring:
        * VehicleDetector (lpr_detect) -- backend-agnostic YOLOv5 port of the original
          vehicle_detection class. Runs through InferModel (so it works on OpenVINO/TensorRT/
          ONNX/OpenCV-DNN/Hailo, not just OpenVINO). The anchor decode (sigmoid, (2x)^2 box
          scaling, per-anchor class, NMS) is preserved and exposed as a free function
          decodeYolo() for unit testing. Behind an IVehicleDetector interface. NHWC in/out,
          config-driven (input size, numClasses, thresholds, anchors, masks, preprocessing).
          FLAGGED CHANGE: the original only decoded ONE output head (`for ou=2;ou<3`); this
          decodes ALL heads by default -- set cfg.headIndices={2} to reproduce exactly.
        * VehicleTracker (lpr_track) -- the original per-gate BYTETracker. ByteTrack is
          vendored verbatim under lpr/track/bytetrack/ (BYTETracker/STrack/kalmanFilter/lapjv/
          utils .cpp + headers; clean Eigen+OpenCV+STL, no Windows-isms) and hidden entirely
          behind a clean PIMPL VehicleTracker exposing lpr::TrackInput -> lpr::TrackedObject
          (stable trackId). ByteTrack's own types (STrack/Object/Eigen/byte_kalman) never leak
          into the rest of the codebase. Needs Eigen (find_package(Eigen3) + EIGEN3_INCLUDE_DIR
          fallback; eigen3 added to vcpkg.json). Eigen is confined to lpr_track -- detection
          alone stays Eigen-free.
        * VehicleAwarePlateRecognizer (lpr_track) -- IS an IPlateRecognizer (drops into the
          DetectionWorker seam). Composition via abstractions: IVehicleDetector& + IPlateRecognizer&
          (the EAST+OCR plate stage) + one VehicleTracker per gate. recognize() = detect vehicles
          -> per-gate tracker.update -> for each tracked vehicle crop the frame -> run the plate
          stage ON THE CROP -> offset plate boxes back to full-frame coords + attach trackId and
          vehicleBox. (Per-track "read once" dedup is intentionally left to the next step,
          PlateProcessor, via the worker's processor seam.) PlateResult gained trackId + vehicleBox.
      Targets: new lpr_track (links lpr_detect + Eigen + OpenCV); lpr_detect gained VehicleDetector;
      lpr_manager now links lpr_track. Tests (tests/track/): vehicle_decode (synthetic 1-cell head
      -> box (16,16,32,32) class 0 score 0.78), vehicle_tracker (moving box keeps stable id=1 over
      6 frames), vehicle_pipeline (fake detector+plate-stage: plate read on the 120x90 crop, center
      offset to (60,45), trackId=1). Full suite 18/18 green. NEXT: PlateProcessor (the processor
      seam's real impl: check_plate validation + Jaro-Winkler dedup + AlprPlate per-track history).

- [x] **PlateProcessor (the processor seam's real implementation)** -- detected plates now
      flow through a real validate/dedup/consensus stage before being sent, exactly as the
      user asked ("detected plates must send to plate process"). It is the clean synthesis of
      logic that was scattered across the original Managment_Cameras::process_recognized_plate,
      AlprPlate (per-track Jaro-Winkler dedup + majority vote + send-once) and the newer
      PlateProcessor.h (format validation + accuracy-weighted consensus). API: process(PlateResult)
      -> optional<PlateResult> (the consensus plate to send, or nullopt while gathering / invalid /
      already-sent), plus asProcessor() which binds it straight onto DetectionWorker::setPlateProcessor.
      Behaviour: (1) validate via checkPlate8 -- a faithful port of Alpr::check_plate (8 chars, [0,1]
      digits, [2] non-digit, [3..7] digits), configurable / nullable; (2) cluster reads per vehicle
      trackId (untracked plates cluster per-gate by Jaro-Winkler similarity, mirroring the old
      sentHistory dedup); (3) reject outlier misreads (similarity < threshold, like AlprPlate::update
      returning false); (4) accuracy-weighted consensus (weightedConsensus, port of PlateProcessor.h);
      (5) emit once per track unless the consensus text changes. PlateResult.trackId/vehicleBox carried
      through from the vehicle stage. New lib lpr_process (links lpr_core for JaroWinkler + lpr_detect +
      OpenCV); NOT linked by lpr_manager (the worker takes it as a std::function, so the Application
      wires it). Test tests/process/test_plate_processor.cpp: validation, vote-gating (minVotes), emit,
      send-once, outlier rejection, weighted consensus, permissive-validator, and asProcessor() binding.
      Full suite 19/19.
- [!] **IMPORTANT test-suite finding (NDEBUG strips asserts)**: the full / full-vcpkg presets compile
      with -O3 -DNDEBUG, so plain assert() is REMOVED -- meaning every existing test that relies on
      assert() (all of them except the new one) passes VACUOUSLY under those presets, including the
      user's Windows full-vcpkg runs. (tests-debug is a real Debug build, so asserts are live there.)
      Surfaced because the new test had process() calls inside assert(), which vanished under NDEBUG and
      left an empty optional to deref. Fix introduced: tests/lpr_check.hpp providing LPR_CHECK (stays
      live regardless of NDEBUG) + LPR_TEST_RESULT(); the new test uses it and is meaningful in Release.
      TODO (offered): migrate the other ~18 tests from assert() to LPR_CHECK so the suite actually
      verifies under full/full-vcpkg, or build tests with -UNDEBUG.

- [x] **services: RecordingService + LiveViewService + CommandRouter** (the "commands,
      recording, send live" parts of Managment_Cameras), each a clean class in a new
      lpr_services lib (links lpr_log + lpr_json + OpenCV):
        * RecordingService -- port of start_recording/stop_recording/(recording half of)
          handle_live_and_recording. A *frame observer* that records a gate to segmented
          video. Improvements: MULTI-GATE (original had a single global vidwrit/recording_gate
          so only one gate could record at a time); no per-frame std::async (original spawned a
          thread per written frame); duration expiry + 15-min segment rollover handled on the
          frame path (no detached sleeper threads); a segment-complete callback replaces the
          hard-coded handel_client->send_recording_async so messaging wires in later.
        * LiveViewService -- port of send_live_view/stop_streaming/(live half of)
          handle_live_and_recording. A frame observer that forwards 1-of-N frames for live
          gates to an injected sink. Improvements: expiry on the frame path (not a detached
          sleeper per request); injected sink (no network dependency); configurable frame-skip.
        * CommandRouter -- port of the command surface the original wired into Handle_Clients/
          NATS signals (live_signal->send_live_view, streaming_signal->start_recording,
          Lpr_Settting->get_setting, Camera_Config->set_camera_config). Transport-agnostic: the
          NATS/TCP layer just connects its signals to liveView/startRecording/getSetting/
          setCameraConfig. get_setting/set_camera_config (heavy app bootstrap in the original)
          are forwarded to injected handlers the Application supplies. JSON envelope parsing
          (messageBody.data.cameraId/duration) + the original duration clamps (live 2..1200).
      Both services plug into DetectionWorker::addFrameObserver (the seam built earlier).
      Test tests/services/test_services.cpp (uses LPR_CHECK so it's meaningful under NDEBUG):
      live enable/skip/expiry/sink, recording state machine + multi-gate + real segment file
      write (MJPG .avi) + segment-complete callback, command dispatch + injected handlers +
      duration clamp. Wrote a real 7KB segment. Full suite 20/20.
- [x] **plate-detection variants now all covered** (answer to "did you implement
      plate_detection_with_car and plate_detection"). The four original near-duplicate methods
      map onto DetectionWorker + a choice of recognizer + flags, not four methods:
        * plate_detection            -> DetectionWorker(recognizer = PlateRecognizer)  [plain]
        * plate_detection_track      -> same + PlateProcessor as the processor seam (per-track
                                        consensus/dedup gives the "tracked plate" behavior)
        * plate_detection_with_car   -> DetectionWorker(recognizer = VehicleAwarePlateRecognizer,
                                        useTracking=true)
        * plate_detection_withouttrack_car -> VehicleAwarePlateRecognizer with useTracking=FALSE
                                        (new flag added this step: detect+crop+plate per frame,
                                        no tracker, trackId=-1).
- [!] **AlprPlate is obsolete** (answer to "do we need AlprPlate more?"): we never ported it as
      a class; its responsibilities (per-track Jaro-Winkler dedup, majority vote, send-once,
      direction) are fully covered by PlateProcessor. Do not port it.
- REMAINING decomposition pieces: PlateSender (messaging: NATS/TCP send_plate + the
  Send_Que_Data thread; needs the user's NATS/server details), HeartbeatMonitor (Monitor_HeartRate),
  Application/main (wire CameraManager->DetectionWorker->PlateProcessor->sink + services + router).
  Recommended cleanup: migrate the other ~18 tests from assert() to LPR_CHECK so they verify under
  full/full-vcpkg (Release/NDEBUG).

- [x] **messaging: PlateSender + CameraStatusNotifier (lpr_net), transport behind IMessageTransport**
      (answer to "we have handle client and nats for send plates, and on camera disconnect send a
      message via NATS"):
        * IMessageTransport -- the seam hiding the broker. publish() (core) + publishDurable()
          (JetStream). Keeps cnats/boost out of the rest of the codebase.
        * NatsTransport (gated WITH_NATS) -- real NATS port of NatsClient's publish path: connect
          with optional mutual-TLS (cert/key/ca, as in the original), core publish via
          natsConnection_PublishString, durable via JetStream js_Publish. cnats kept in the .cpp via
          PIMPL. NOT compiled in this sandbox (no cnats SDK) -- same situation as TensorRT/Pylon; it
          was syntax-checked against a stub of the cnats API. Verify exact cnats signatures + enable
          with -DWITH_NATS=ON on the user's machine.
        * InMemoryTransport -- records published messages; the default + test transport.
        * PlateSender -- port of Handle_Clients::send_plates + the Send_Que_Data thread. It is the
          DOWNSTREAM SINK: worker.setPlateSink(sender.asSink()). Builds the "plates_data" message in
          the original wire format (messageId/messageType/messageBody{timestamp,camera_id,full_image,
          cars[{box,direction,plate{plate,is_valid,track_id,plate_image},ocr_accuracy,vehicle_class,
          vehicle_type,meta_data}]}). Async queue+thread (reuses BlockingQueue) so the detection
          thread never blocks on the network. Per-(gate:plate) send cooldown (configurable; default
          60s, 5-min prune) ports the original last_sent_map. Image encoding configurable: ByteArray
          (JSON array of JPEG bytes, matching the original backend) or Base64. buildMessage() is a
          pure, unit-tested function. Iran-specific plate parsing intentionally left as a future
          pluggable step (kept generic + a validator hook for plate.is_valid).
        * CameraStatusNotifier -- port of Handle_Clients::send_camera_connection. notify(camera,
          connected) publishes the "camera_connection" message ({camera_id, Connection}) to
          socketio.camera_connection, deduped on state change. asCallback() returns exactly
          CameraManager::setStatusCallback's type (void(gate,bool)), so a DISCONNECT from any
          CameraWorker is forwarded to the backend with one line of wiring:
          cameraManager.setStatusCallback(notifier.asCallback()).
      lpr_net links lpr_core (Base64/Uuid/Time) + lpr_detect (PlateItem) + lpr_json + OpenCV
      (imgcodecs). Test tests/net/test_messaging.cpp (LPR_CHECK, Release-safe): plates_data structure,
      async durable send + count, cooldown suppression, camera connect/disconnect messages +
      change-dedup. Full suite 21/21.
- REMAINING: HeartbeatMonitor (Monitor_HeartRate -> periodic heartbeat via IMessageTransport),
  Application/main wiring (CameraManager -> DetectionWorker -> PlateProcessor -> PlateSender, plus
  RecordingService/LiveViewService as frame observers, CommandRouter on the transport, and
  CameraStatusNotifier on the status callback). Cleanup: migrate older tests to LPR_CHECK.

- [x] **recording + live connected to NATS (MediaSender)** -- answer to "do you connect recording
      and live to NATS also?": previously RecordingService/LiveViewService only had injectable hooks
      (segment-complete callback, live sink) that nothing published. Now lpr_net has MediaSender,
      the port of Handle_Clients::send_live / send_recording:
        * sendLiveFrame(gate,img)        -> "live" message {timestamp,camera_id,live_image} on
                                            subject "socketio.live" (half-size, JPEG q20).
        * sendRecordingEvent(gate,path,end,frame) -> "recording" message {timestamp,camera_id,
                                            video_address,frame,end_recording} on subject
                                            "message.recording.<gate>" (JPEG q60).
        * liveSink() -> LiveViewService::setLiveSink  ; recordingCallback() ->
          RecordingService::setSegmentCompleteCallback. So the live frame path and segment-close
          path now actually reach the broker via IMessageTransport.
      Refactor done at the same time: the JPEG->JSON encoding that was duplicated inside PlateSender
      was extracted to a shared header lpr/net/ImageCodec.h (encodeJpeg + a single ImageEncoding
      enum reused by PlateSender and MediaSender; PlateSender keeps a `using ImageEncoding` alias so
      its API is unchanged). buildLiveMessage/buildRecordingMessage are pure + unit tested.
      test_messaging.cpp extended: live/recording builders, AND end-to-end wiring
      (LiveViewService.onFrame -> MediaSender -> transport on socketio.live; RecordingService
      start/onFrame/stop -> segment close -> MediaSender -> transport on message.recording.<gate>).
      Full suite 21/21.

- [x] **PRE-MAIN AUDIT (original vs clean, end-to-end follow) + bug fixes**. Traced the full data
      flow and compared against Managment_Cameras. Confirmed CORRECT: capture->motion gate->shared
      FrameQueue->DetectionWorker; recording/live as DetectionWorker frame-observers IS faithful
      (the original called handle_live_and_recording at lines 488/876, i.e. on post-motion frames
      pulled from gate_images, not on raw frames); per-gate trackers + per-track PlateProcessor keys
      handle the single interleaved queue correctly; CameraManager fan-in + status wiring correct.
      BUGS FOUND & FIXED:
        * CameraWorker reconnect status bug: on disconnect emitStatus(false) fired but connected_ was
          never reset, so a camera that reconnected NEVER re-emitted "connected" (backend would see the
          disconnect but not recovery). Fixed: reset connected_=false and firstFrame_=true on disconnect.
        * RecordingService::openSegment indexed cfg_.fourcc[0..3] blindly -> UB if a short fourcc string
          was configured. Fixed: pad/truncate to 4 chars.
        * PlateSender queue was bounded at 1024 (drops the OLDEST unsent plate under burst = lost plate
          data); the original Send_Que_Data was unbounded. Fixed: unbounded queue (note: unbounded grows
          if the broker is down).
      INCOMPLETE / GAPS still open (flagged, not yet built) -- to weigh before main:
        1. Detection ROI/mask NOT applied: the original cropped+masked each frame to the per-camera
           detection ROI (SettingsManager::getGeneralRoi + cropAndMask, which ARE ported) before
           recognition, and mapped boxes back. The clean recognizers run on the FULL frame. Capability
           exists in config; it just needs to be applied in the detection stage (crop -> recognize ->
           offset boxes). Biggest fidelity/perf gap.
        2. plate_detection_track is only APPROXIMATED: plain PlateRecognizer gives trackId=-1, so
           PlateProcessor clusters per-gate by text similarity rather than tracking plate positions.
           True plate-position tracking (running EAST boxes through the tracker) is not implemented.
        3. Recording duration watchdog: duration expiry is checked on onFrame; if a camera stops
           sending frames, a duration-limited recording never closes (original used a detached timer).
        4. Minor: recording observer records the FULL frame (original recorded the cropped image in the
           with_car path); buildFromSettings has a dangling MaxDeviation comment; clean-stream-end does
           not emit disconnect (intentional vs error disconnect).
      Suite remains 21/21 after fixes.

- [x] **All four audit gaps closed + bootstrap-flow support (subscribe/token)**:
        1. Detection ROI/mask: RoiCropRecognizer (lpr_detect) -- a DECORATOR IPlateRecognizer that
           crops each frame to the per-gate ROI (+ optional polygon mask), delegates to the inner
           recognizer, and offsets result boxes (and vehicleBox) back to full-frame coords. RoiProvider
           / MaskProvider are injected (wire to SettingsManager::getGeneralRoi + getCameraPoints +
           cropAndMask). Test: inner saw 100x80 crop, plate offset 5,5 -> 55,45; rect shifted.
        2. plate_detection_track: PlateTrackingRecognizer (lpr_track) -- DECORATOR that runs the inner
           plate recognizer, feeds the PLATE boxes through a per-gate VehicleTracker, and binds each
           plate to the best-IoU tracked box -> real stable trackId (no longer just text clustering).
           Test: drifting plate keeps trackId=1 across frames.
        3. Recording watchdog: RecordingService gained a watchdog thread (Config.watchdogMs, default
           500) + tick(); a duration-limited recording now closes (and fires the segment callback)
           even if the camera stops sending frames. Test confirms close without further frames.
        4. Minor: tidied the misleading MaxDeviation comment; recording-records-full-frame is left as
           a main wiring choice (the observer gets whatever frame is fed); clean-stream-end
           intentionally does not emit a disconnect (a finished file is not a camera fault).
      BOOTSTRAP-FLOW support (connect->token->get settings->load->create cameras->detect): extended
      IMessageTransport with subscribe(subject,handler) + MessageHandler; InMemoryTransport implements
      subscribe + a deliver() test helper; NatsTransport gained Config.token (natsOptions_SetToken) and
      subscribe() (natsConnection_Subscribe via a C trampoline that forwards subject+payload). All
      gated/ syntax-checked against an extended cnats stub. Verified the ingest path EXISTS:
      SettingsManager::loadAll(json) ingests a received settings messageBody; CameraManager
      .buildFromSettings + CameraSourceFactory create cameras by link type; recognizers load models from
      settings. So the whole flow is wireable in main with no missing capability -- only the Application
      glue remains. Full suite 23/23.
- FLOW CHECK (as the user described it): connect+token = NatsTransport(Config.token)+connect(); "send
  token + get settings" = either connection-token auth + subscribe to the settings subject(s) the server
  pushes (matches the original's NATS signals), or publish a register message then receive on a reply
  subject -- both are supported by publish()+subscribe(); if true request/reply is required, add a
  request() method (small). Received settings -> SettingsManager::loadAll -> load models (paths/backends
  from getLpr) -> CameraManager::buildFromSettings -> DetectionWorker(recognizer)+PlateSender start.
- REMAINING: HeartbeatMonitor; Application/main (the glue described above); migrate older tests to LPR_CHECK.

- [x] **Application + main (the lpr executable) -- end-to-end wiring of the bootstrap flow**.
      include/lpr/app/Application.h + src/app/Application.cpp + src/app/main.cpp; new lpr_app lib
      + `lpr` executable (links the whole stack). Implements exactly the described flow:
        start(): create transport (NatsTransport with token when --nats + WITH_NATS, else
                 InMemoryTransport) -> connect -> subscribe(settingsSubject) -> publish a register
                 message carrying the token.
        onSettings/bootstrapFromJson(): SettingsManager::loadAll -> buildRecognizerChain() ->
                 build queues/services -> wire -> start.
        buildRecognizerChain(): backend = parseBackend(getLpr "backend"); device = getLpr "device";
                 load PlateRecognizer(detector+ocr); then pick the variant by car_detection/track_plates
                 (1->vehicle+track, 2->vehicle no-track, track_plates 1->PlateTrackingRecognizer, else
                 plain), then wrap in RoiCropRecognizer (per-camera ROI+mask from SettingsManager).
        wiring: DetectionWorker(frames, topRecognizer) + PlateProcessor (processor seam) + PlateSender
                 (sink); RecordingService + LiveViewService as frame observers -> MediaSender ->
                 transport; CommandRouter on the live/recording/camera_config subjects; CameraStatusNotifier
                 on CameraManager status (set BEFORE buildFromSettings); CameraManager.buildFromSettings
                 creates sources by link type. Clean shutdown: stop() stops cameras->worker->sender then
                 resets the transport (closes NATS subs) before handlers are torn down (race-safe);
                 CameraWorker source-factory now wrapped in try/catch so a bad link type can't terminate.
      main.cpp: `lpr --nats <url> --token <t> [--settings-subject <s>]` (production) or
                `lpr --settings <file.json>` (offline). SIGINT/SIGTERM -> clean shutdown.
      BACKEND SELECTION (answer to the user's question): 2 levels -- (a) compile-time WITH_* decides
      which InferModel backends exist; (b) runtime the "backend" setting (parseBackend) picks one, or
      Auto picks the best compiled-in (TensorRt>OpenVino>Hailo>OnnxRuntime>OpenCvDnn). Model path +
      backend + device are read from settings per model; the model FILE FORMAT must match the backend
      (OpenVINO .xml/.bin, TensorRT .engine, ONNX/OpenCV .onnx).
      VERIFIED: builds; offline smoke run boots, starts DetectionWorker/PlateSender/CameraManager and
      shuts down cleanly; variant selection confirmed (car_detection=1 -> vehicle+track->plate,
      track_plates=1 -> plate+tracking). NOTE: LPR settings JSON is an ARRAY of {name,value} (under
      "settings"); cameras under "cameras_data". Full suite 23/23.
- REMAINING (optional): HeartbeatMonitor; settings re-bootstrap on live update; migrate older tests to LPR_CHECK.

- [x] **NATS connection from environment + model-type setting (default openvino)**:
        * main.cpp reads the connection from the environment (CLI overrides env), matching the
          original which used NATS_SERVER_URL + AUTH_TOKEN, plus the requested user/password:
            NATS_SERVER_URL -> url, AUTH_TOKEN -> token, NATS_USER -> user, NATS_PASSWORD -> password.
          useNats is enabled automatically when NATS_SERVER_URL is set (or --nats given).
        * NatsTransport::Config gained user+password; connect() applies natsOptions_SetToken and
          natsOptions_SetUserInfo (gated, syntax-checked against the cnats stub).
        * Backend/model-type: buildRecognizerChain reads the ALPR setting Options.keyBackend
          (default key name "model_type") via getLpr<string>, DEFAULT VALUE "openvino"
          (Options.defaultBackend), parsed by parseBackend -> Backend::OpenVino. Verified:
          with the setting absent, logs model_type='openvino' -> backend OpenVINO.
      Full suite 23/23.

- [x] **FINAL COMPLETENESS AUDIT (whole original, last pass) + HeartbeatMonitor built + tested**.
      Mapped every Managment_Cameras method/member and every non-vendored original file:
        ACTIVE PIPELINE -> all ported: ctor/main; alpr_loading->buildRecognizerChain; add_camera/
        adding_camera_task/add_imge->CameraManager+CameraWorker; run_plate_detection + the 4 variants
        ->DetectionWorker+recognizer chain (verified all 4 select correctly); detect_vehicles->
        VehicleDetector; recognize_plates->PlateRecognizer; process_tracker_car->VehicleAware;
        process_recognized_plate->PlateProcessor; handle_live_and_recording->Recording/LiveView observers;
        get_segmented_filename->RecordingService; send_data_to_server->PlateSender; start/stop_recording,
        send_live_view, stop_streaming->CommandRouter+services+MediaSender; get_setting/set_camera_config->
        CommandRouter+SettingsManager.loadAll; initializeHandleClient->Application transport+subs;
        check_point/isPointInsidePolygon->SettingsManager+RoiCropRecognizer mask; monitor_HeartRate->
        HeartbeatMonitor (BUILT this pass: alternates socketio.resources + socketio.heartbeat;
        cross-platform via injectable ResourceProvider; wired into Application, interval from
        getLpr("heartbeat_interval"), min 120). Collaborator files all mapped: Alpr/Alpr_vino->
        PlateRecognizer+OpenVinoModel; Detection_plate/Detection_Model_Tensor->TensorRtModel; EastDetector->
        EastTextDetector; Ocr_Model(_tensor)->PlateOcr; ctc_decode->CtcDecoder; vehicle_detection->
        VehicleDetector; bytetracker->VehicleTracker; AlprPlate->PlateProcessor; Config->SettingsManager;
        concurrent_queue->BlockingQueue; base64/jaroWinkler/PathAndUUIDGenerator->util; AppLogger->Logger;
        NatsClient->NatsTransport; Send_Que_Data->PlateSender; Handle_Clients->the 5 net modules;
        CameraSource/Factory/Basler/Gst/vlc/virtual_cap->capture sources; camera_managment->CameraWorker;
        VideoPathManager->RecordingService paths.
      NOT PORTED (verified not referenced by the active pipeline = dead/auxiliary, correctly excluded):
        EntranceModel, AutoRoadStereoRectifier, Multi_Track_Blobs, MySQLDatabase, OutboxDir, sunset,
        manage_time, functioncodes.
      NOT PORTED (referenced, but deliberate): full TCP client/server stack (TCP_Server, TCP_Client_Boost,
        Json_TCP_Sender, JSONSender, *_Client_Helper, client_managment, TcpClient) -> NATS is the path;
        TCP_Rele_sokect (hardware relay) -> out of scope; SystemResourceMonitor (real CPU/RAM/disk) ->
        replaced by HeartbeatMonitor's injectable ResourceProvider (stub returns zeros; plug platform code);
        encp (encryption) + manage_time (licensing) -> deferred from the start; IranPlateParser -> replaced
        by PlateSender's generic validator hook. Minor: original's "reload model if >1000 tracked vehicles"
        safety not ported.
      TEST RESULTS: full suite 23/23 PASS (config/util/log/capture/manager/detect/track/process/services/
        net). All four pipeline variants verified end-to-end via the lpr executable. model_type default
        'openvino' -> OpenVINO confirmed. Clean boot + shutdown of DetectionWorker/PlateSender/
        HeartbeatMonitor/CameraManager. Engagement is feature-complete vs the original's active pipeline.
- REMAINING (need the user's environment): real model files (detector/OCR/car + alphabet/blank/geometry),
  a live NATS server (build -DWITH_NATS=ON, verify exact cnats signatures + real subjects + settings keys),
  a platform ResourceProvider, and settings re-bootstrap on live update. Optional: migrate older tests to LPR_CHECK.

- [x] **Camera connect/disconnect messages, message-format doc, error handling, daily log + purge**:
        * Camera connection: verified CameraStatusNotifier publishes socketio.camera_connection with the
          EXACT original structure {messageId, messageType:"camera_connection", messageBody:{camera_id,
          Connection}}. Both edges fire -- disconnect (Connection:false) when the source errors, and
          reconnect (Connection:true) on the next frame (the connected_ reset fix) -- de-duplicated to
          state changes. Wired in Application via CameraManager::setStatusCallback.
        * Saved ALL outbound wire formats to MESSAGE_FORMATS.md: plates_data (messages.plates_data),
          live (socketio.live), recording (message.recording.<gate>), camera_connection
          (socketio.camera_connection), heartbeat (socketio.heartbeat), resources (socketio.resources),
          register (lpr.register), plus the subscribed subjects and the settings payload shape.
        * Error handling: DetectionWorker hot path now wraps frame-observers AND the
          recognize/process/sink path in try/catch -> LOGE (a bad frame/model error is logged and
          skipped, never crashes the detection thread). Combined with existing guards: CameraWorker
          (source factory + run), Application/main (settings parse), SettingsManager::loadAll, model
          load (LOGW/E), transport connect, and command-handler lambdas. Errors go to the log file.
        * Logging: Logger already rotated daily (new file when the date changes) + rolls at 10 MB;
          ADDED retention -- purgeOldLogs() deletes <app>_*.log files older than retentionDays (default
          30) at startup and on each day rollover, leaving other prefixes untouched. main() now calls
          Logger::init("lpr") (file logging on), with env overrides LOG_DIR + LOG_RETENTION_DAYS and a
          --log-console flag. Verified: a 40-day-old lpr log was purged, a different-prefix file was
          kept, and today's lpr_YYYY-MM-DD.log was created and written.
      Full suite 23/23.

- [x] **Mono+RGB dual-camera capture (was a real gap) + final messaging/error recheck**.
        FINDING: the dual-camera plumbing existed (CaptureSource::onFrame(frame,mono,t),
        FrameItem.monoImage, monoAddress, setMonoAddress) BUT no concrete source opened the second
        stream -- every source emitted cv::Mat() for mono, so monoImage was always empty. The original
        opened both in one source (virtual_cap_url::set_mono_adress -> send_frame(A,B)).
        FIX: new DualCaptureSource (lpr_capture) composes two CaptureSources (main + mono), runs the
        mono stream on its own thread, and pairs each main frame with the latest mono frame ->
        emitFrame(main, mono, t) (loose latest-mono pairing, like the original). CameraSourceFactory now
        builds a DualCaptureSource of the SAME link type when MonoCameraAddress is set (main=CameraAddress,
        mono=MonoCameraAddress), else a single source as before. Fixed a double-join race (run() solely
        owns the mono-thread join, not stop()). Test test_dual_capture: 20/20 main frames delivered
        paired with the mono frame.
        OPEN QUESTION for the user: we capture BOTH streams and carry both (item.image = CameraAddress,
        item.monoImage = MonoCameraAddress); detection + recording/live currently use item.image. If the
        original ran detection on one stream and recording/live on the other, say which and it's a 1-line
        wiring change (the data is already paired and available).
        MESSAGING recheck: all subjects/structures confirmed vs original (plates_data, socketio.live,
        message.recording.<gate>, socketio.camera_connection, socketio.heartbeat, socketio.resources,
        lpr.register) -- documented in MESSAGE_FORMATS.md; camera connect+disconnect both fire.
        ERROR HANDLING recheck: guarded paths = CameraWorker (source factory + run), DualCaptureSource
        (mono thread), DetectionWorker (observers + recognize/process/sink), Application/main (settings
        parse), loadAll, model load, transport connect, command-handler lambdas; all log via LOGE.
      Full suite 24/24.

- [x] **NATS protocol corrected to MATCH THE ORIGINAL (was wrong: push + SetToken)**.
        Read the original NatsClient. The real protocol is request/reply, not push:
          connect: mutual TLS always (SetSecure + LoadCATrustedCertificates + LoadCertificatesChain),
            SetExpectedHostname("ALPR"), SetNoEcho(true), SetMaxReconnect(-1), SetReconnectWait(2000),
            SetTimeout(5000), optional NATS_USER/NATS_PASS, JetStream context. Token is NOT a NATS
            connection token (no SetToken).
          authenticate: publish {token} to "authenticate" reply-to "response.token.<token>";
            reply {status:"success", client_id} -> clientId/lpr_id.
          commands: subscribe "command.<clientId>"; messageBody.data.command_type in
            recording|streaming|set_config|reset_camera|lpr_settings; reply on "response.<clientId>".
          settings: publish {client_id, request_type:"alpr_settings"} to "alpr.settings.request";
            reply "alpr.settings.response.<clientId>"; settings in messageBody.data -> loadAll.
        IMPLEMENTED: NatsTransport.connect() now sets the real TLS/host/echo/reconnect/timeout options
        and dropped SetToken; added publishWithReply (natsConnection_PublishRequestString) +
        IMessageTransport::publishWithReply seam. Application now runs the real handshake:
        authenticate() -> onAuthReply (parse client_id) -> subscribeToCommands("command.<id>") +
        requestSettings("alpr.settings.request" -> reply onSettings -> bootstrapFromJson(messageBody)).
        onCommand routes to CommandRouter and replies on response.<id>. Heartbeat lpr_id = clientId.
        Options now carry authenticateSubject + settingsRequestSubject + expectedHostname. Removed the
        old assumed lpr.settings/lpr.register/lpr.cmd.* subjects. Syntax-checked vs the extended cnats
        stub; offline path unchanged (38-frame single-cam run OK). Full suite 24/24.
        NOTE: env var for password is NATS_PASS in the original (we read NATS_PASSWORD AND should accept
        NATS_PASS) -- see follow-up.

- [x] **NATS auto-reconnect + initial-connect retry (matches the original)**.
        * Auto-reconnect: cnats already set MaxReconnect(-1)+ReconnectWait(2000); ADDED the connection
          callbacks the original used -- natsOptions_SetDisconnectedCB / SetReconnectedCB / SetClosedCB
          (log + note that cnats keeps retrying and restores subscriptions on reconnect). So a dropped
          NATS connection is retried indefinitely by the client library and subscriptions resume
          automatically; the app-level client_id from authenticate persists (the original did not
          re-authenticate on reconnect, and neither do we).
        * Initial connect retry: Application::start(std::atomic<bool>& running) now loops connect()
          every 5s until it succeeds or the user stops (Ctrl-C), mirroring Handle_Clients' retry loop
          (was: give up after one failed attempt). main() installs the signal handler before start()
          and passes the running flag so the retry is interruptible.
        Syntax-checked vs the extended cnats stub; offline path unchanged (38-frame run OK); suite 24/24.

- [x] **Config-file model paths wired in (was implemented but UNUSED) + reconnect/frame-send verified**.
        FINDING: the original reads MODEL/training-data paths from a local key=value Config file
        (path = CLI arg): car = conf->config_file + "/" + conf->car_file; Alpr_vino(*conf) reads
        plate_detection_file + ocr_file; only runtime toggles (car_detection, track_plates, widths,
        use_cpu/cuda) come from the NATS settings. AppConfig (the clean port of Config) already existed
        and parsed all keys, but it was NOT wired into the pipeline -- buildRecognizerChain read model
        paths from NATS settings instead. FIX: Application now loads AppConfig (Options.configPath / env
        LPR_CONFIG / --config) and buildRecognizerChain assembles model paths from it
        (config_file + "/" + {plate_detection_file,ocr_file,car_file}) exactly like the original, using
        deep_detect_prob as the EAST score threshold; falls back to the NATS settings keys when no
        config file is given. Verified: --config /tmp/config_file.txt -> 'model paths from config file
        (detector=/models/alpr/east.xml, ocr=.../ocr.xml, car=.../yolov5.xml)'.
        VERIFIED vs original: NATS reconnection -- cnats infinite auto-reconnect (MaxReconnect -1 /
        ReconnectWait 2000) + Disconnected/Reconnected/Closed callbacks + initial-connect retry loop
        (matches Handle_Clients). Camera reconnection -- CameraWorker reconnects (recreates source) and
        emits camera_connection false(on drop)/true(on recovery), matching the detection class's
        send_camera_connection(gate,false/true); difference: ours uses exponential backoff 1s->30s
        (tunable via MotionConfig) vs the original GStreamer's fixed 1s. Frame sending -- plates_data,
        socketio.live, message.recording.<gate>, socketio.camera_connection, socketio.heartbeat,
        socketio.resources all match the original (see MESSAGE_FORMATS.md). Full suite 24/24.

- [x] **OCR rewritten to match original Ocr_Model + heartbeat units + detector size from settings**.
        Runtime error was: OCR model input (shape=[?,48,184,3]) vs our tensor (1,3,24,94). Root cause:
        the original OpenVINO models are fed NHWC u8 (model normalizes internally via OpenVINO PPP:
        set_element_type(u8).set_layout("NHWC")), but our OpenVinoModel fed NCHW f32 (blobFromImage).
        FIXES:
        - InferModel: added InputFormat {NchwF32, NhwcU8} + setInputFormat() (no-op default).
        - OpenVinoModel: when NhwcU8, applies PPP at load (u8/NHWC, f32 outputs) and feeds the raw HWC
          u8 cv::Mat directly as an {1,H,W,C} u8 tensor (matches Ocr_Model::get_input_tensor).
        - PlateOcr now matches Ocr_Model exactly: inputSize 184x48 (SetInputDims(184,48)), NHWC u8,
          3-channel BGR (no cvtColor/swapRB), NO /255 (scale=1.0; model normalizes), charset
          " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" with leading space = CTC blank (index 0), and
          numClasses = alphabet.size() (charset already includes the blank; removed the wrong +1).
          NHWC/u8 used only on the OpenVINO backend; other engines fall back to NCHW/f32.
        - Heartbeat: settings heartbeat_interval is SECONDS -> intervalMs = sec*1000 (was treated as ms,
          giving interval=120ms; user's 20 -> 20000ms).
        - Detector input size + geometry now come from settings (deep_width x deep_height = 1280x736,
          deep_plate_*; deep_detect_prob -> scoreThresh), with the config file as fallback (was fixed
          640x320). getLpr<double> not instantiated -> used getLpr<float>.
        Build: full + OpenVINO PPP path compiles; 24/24 tests pass. (Inference correctness needs the
        user's hardware; no models/GPU here.)
        STILL OPEN: the original DETECTOR is Detection_plate (NHWC u8, input {<=21,H,W,3}, per-cell
        4-corner-point decode at input/4 resolution where prob>=deep_detect_prob), NOT EAST RBOX.
        Our detector is still EastTextDetector (NCHW f32 + RBOX decode). Detector boxes for
        detect_model_int8.xml will be wrong until Detection_plate is ported -> NEXT STEP.

- [x] **Verified both models from their IR XML; OCR charset fixed; detector confirmed = original EAST; video-open hardened**.
        Read shapes directly from detect_model_int8.xml + ocr_model_2.xml:
        - DETECTOR: input [-1,-1,-1,3] NHWC f32 (FakeQuantize int8), outputs [N,H/4,W/4,1] from
          East_Head/pred_score_map/Sigmoid + [N,H/4,W/4,5] from pred_geo_map -> it IS an EAST head
          (score + 5-ch RBOX). Original uses EastDetector (Alpr_vino.h line 74; Detection_plate is
          commented out line 73). Our EastTextDetector already matches EXACTLY: normalize
          convertTo(CV_32F, 1/127.5, -1.0), NHWC f32, geometry d0..d3 * deep_plate_height/width_1/
          width_2, size from deep_width x deep_height. So detector was already correct; only the
          input size (settings) needed wiring (done previous step: 1280x736).
        - OCR: input image=[-1,48,184,3] NHWC (matches 184x48), output [-1,-1,39] = 39 classes.
          multi_language=0 charset = " 0123456789abcdefghijlkmnoqstvwypuzxDS" (38) and CTC
          num_classes = len+1 = 39 with the BLANK as the LAST class (original: prob[num_classes-1];
          output index i -> alphabet[i]). Fixed PlateOcr: alphabet = the 38-char set, numClasses =
          alphabet.size()+1, blankLast=true (blank index = numClasses-1). Charset selected by
          multi_language in Application (1 -> 60-char mixed set). (Earlier interim fix had wrong
          charset/blank-first; corrected.)
        - VIDEO OPEN: vcpkg opencv4 had NO ffmpeg feature (only msmf/dshow) -> .avi/.mp4 decode
          unreliable. Added "features":["ffmpeg"] to opencv4 in vcpkg.json. VideoFileCaptureSource now
          checks file existence first (logs "file does not exist" vs "open failed - missing codec")
          and tries default then CAP_FFMPEG. (3 of the user's cameras simply pointed at a missing
          s2.avi; camera 1's s5.mp4 opened fine.)
        Build (full + OpenVINO) + 24/24 tests pass. Inference correctness still needs the user's HW.

- [x] **Basler facade WIRED into the capture path + bandwidth/trigger reviewed**.
        Was: factory created a minimal direct-grab BaslerCaptureSource (no trigger/exposure/gain/sync/
        bandwidth); the full facade (lpr_basler) compiled but was unreferenced.
        WIRING (avoids the lpr_capture<->lpr_basler cycle via a creator hook):
        - CameraSourceFactory::setBaslerCreator(Creator) registered by the app; Pylon branch uses the
          facade when registered, else falls back to the minimal grabber. The factory also lets the
          facade own the RGB+mono PAIR (setMonoAddress on one source) instead of wrapping two sources
          in a DualCaptureSource, because BaslerCamera pairs/sync's them internally.
        - lpr_basler: new BaslerFactory.cpp/h -> makeBaslerFacade() builds BaslerCamera (which IS-A
          lpr::CaptureSource via the virtual_cap_url shim).
        - lpr_app: Application ctor registers makeBaslerFacade under LPR_WITH_PYLON; CMake links
          lpr_basler into lpr_app and defines LPR_WITH_PYLON there (when pylon found).
        Now a 'pylon' camera is built by the facade, which on connect self-applies (from settings):
        trigger role (trigger_mode/continuous_exposure -> SoftwareTrigger / TriggeredSlave (Line1) /
        ExposureActiveOutput (Line2)), exposure strategy + limits, gain, GigE bandwidth (packet size +
        DeviceLinkThroughputLimit/GevSCPD + per-cam AcquisitionFrameRate via GigEBandwidthCoordinator),
        and reconnect. Verified bandwidth + trigger logic line-by-line = original (IDENTICAL).
        Build note: non-Pylon parts (factory hook + guarded registration) compile here; 24/24 tests
        pass. The Basler facade itself CANNOT be compiled in this sandbox (no Pylon SDK) -> must be
        built+validated on the user's Basler hardware.
        REMAINING (optional): runtime live control-command routing (NATS command -> facade
        handleCommand(key,json) for live Exposure Time/Gain/Trigger Mode changes). Initial config from
        settings already works; live changes currently need a settings re-bootstrap. Offered to wire.

- [x] **Model input sizes from settings: EAST (re-verified) + Vehicle detector (fixed)**.
        EAST detector: deep_width x deep_height from settings -> east.detectWidth/detectHeight already
        wired (config-file fallback); model input is dynamic [-1,-1,-1,3] so it accepts the settings
        size (rounded to a multiple of 32 as an EAST safety net; 1280x736 unchanged). Log:
        'detector input 1280x736'.
        Vehicle detector: WAS using the fixed default 640x480 because Application called
        vehicleDet_->load(carModel, backend, device) without a VehicleConfig. FIXED: now builds a
        VehicleConfig with width/height from settings deep_car_width/deep_car_height (user: 512x256),
        config-file fallback, and passes it to load(). Log: 'vehicle model input 512x256'.
        Build + 24/24 tests pass.

- [x] **GStreamer/VLC RTSP + any-codec + single/dual camera verified**.
        GStreamer: matches original (rtspsrc location/latency/protocols=4 TCP, pad-added; H264/H265 via
        hardware d3d11h264dec/d3d11h265dec, openh264/5 then avdec_* fallback - ours adds the avdec
        fallback the original had commented out). ADDED any-codec support: non-H264/H265 streams now go
        through a decodebin fallback (onDecodebinPad: decodebin -> videoconvert -> BGR capsfilter ->
        appsink), so MJPEG/VP8/VP9/MPEG-4/etc. work. Only the else-branch changed, so the H264/H265
        hardware path is untouched. Compile-verified with WITH_GSTREAMER=ON (GStreamer 1.24 in sandbox).
        VLC: for rtsp:// URLs now forces TCP transport (:rtsp-tcp) + :network-caching=300 (the original
        used UDP default; TCP avoids packet loss on busy links). VLC's codec coverage is inherently broad.
        (libvlc not in sandbox -> VLC change syntax-reviewed, not compiled.)
        Single vs dual (mono+RGB): VERIFIED. Single = main source only. Dual = DualCaptureSource runs the
        mono stream on its own thread, keeps the latest mono frame, pairs it with each main frame ->
        FrameItem{image, monoImage}; CameraManager reads MonoCameraAddress and the factory builds the
        dual source. Basler does the RGB+mono pair INTERNALLY via the facade (set_mono_adress + master/
        slave sync). Covered by test_dual_capture. 24/24 tests pass.
