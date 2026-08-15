cmake_minimum_required(VERSION 3.21)
project(lpr LANGUAGES CXX VERSION 0.1)

# Guard against an accidental 32-bit (x86) toolchain. The dependencies (OpenCV,
# GStreamer, OpenVINO, TensorRT, ONNX Runtime) are all 64-bit, so an x86 build
# fails later with confusing "machine type 'x64' conflicts with 'x86'" linker errors.
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "32-bit (x86) build detected, but this project is 64-bit.\n"
    "The plain 'Developer Command Prompt' and 'x86 Native Tools' target x86 -- that is\n"
    "why vcpkg pulled x86 OpenCV and the compiler is Hostx86/x86/cl.exe.\n"
    "Fix (pick one), then wipe and reconfigure:\n"
    "  A) Start menu -> 'x64 Native Tools Command Prompt for VS 2026', or\n"
    "  B) in this window run vcvars64.bat from <VS>/VC/Auxiliary/Build\n"
    "Verify:  echo %VSCMD_ARG_TGT_ARCH%   (must print x64, not x86)\n"
    "Then:    rmdir /s /q build  &  rmdir /s /q vcpkg_installed  &  cmake --preset full-vcpkg")
endif()

if(MSVC)
  # Pylon and other SDKs pull in <windows.h>, whose min/max MACROS otherwise break
  # OpenCV headers (error C2589 on std::numeric_limits<>::max). Define globally.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
  # Emit debug symbols (PDB) even in Release so VS Code (cppvsdbg) can set breakpoints in the
  # release build without switching to the Debug runtime (which would pull *d.dll deps).
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LPR_BUILD_TESTS      "Build unit tests"                         ON)
option(LPR_BUILD_CONFIG_LIB "Build the full config library (needs OpenCV)" ON)

find_package(Threads REQUIRED)

# ---- header-only utilities (BlockingQueue, ...) ----
add_library(lpr_util INTERFACE)
target_include_directories(lpr_util INTERFACE include)
target_compile_features(lpr_util INTERFACE cxx_std_17)
target_link_libraries(lpr_util INTERFACE Threads::Threads)

# ---- compiled core utilities (base64, jaro-winkler, capture paths) ----
add_library(lpr_core STATIC
    src/util/Base64.cpp
    src/util/JaroWinkler.cpp
    src/util/CapturePathBuilder.cpp)
target_link_libraries(lpr_core PUBLIC lpr_util)
target_include_directories(lpr_core PUBLIC include)

# ---- logging (dependency-free, replaces Boost.Log AppLogger) ----
add_library(lpr_log STATIC src/log/Log.cpp)
target_link_libraries(lpr_log PUBLIC lpr_util)

option(WITH_VLC "VLC RTSP backend (auto-skips if libvlc missing)" ON)
option(WITH_GSTREAMER "GStreamer backend (auto-skips if missing)" ON)
option(WITH_PYLON "Pylon/Basler backend (auto-skips if SDK missing)" ON)
option(WITH_OPENVINO "OpenVINO inference backend (auto-skips if not found)" ON)
option(WITH_TENSORRT "TensorRT inference backend (NVIDIA/Jetson; off by default)" OFF)
option(WITH_ONNXRUNTIME "ONNX Runtime backend (cross-platform; XNNPACK/CUDA/TRT/OpenVINO EPs)" ON)
option(WITH_HAILO "Hailo NPU backend (Raspberry Pi AI HAT+; off by default)" OFF)
option(WITH_NATS "NATS messaging transport (needs the cnats SDK; off by default)" OFF)

# ---- capture (auto-enabled when OpenCV is available) ----
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
  message(STATUS "OpenCV ${OpenCV_VERSION} found -> building lpr_capture")
  set(LPR_CAP_SRC  src/capture/CameraSourceFactory.cpp
                   src/capture/DualCaptureSource.cpp
                   src/capture/VideoFileCaptureSource.cpp)
  set(LPR_CAP_DEFS "")
  set(LPR_CAP_LIBS lpr_util lpr_log ${OpenCV_LIBS})
  set(LPR_CAP_INCS include ${OpenCV_INCLUDE_DIRS})

  find_package(PkgConfig QUIET)

  if(WITH_VLC)
    find_package(LibVLC QUIET)
    if(LIBVLC_FOUND)
      list(APPEND LPR_CAP_SRC  src/capture/VlcCaptureSource.cpp)
      list(APPEND LPR_CAP_DEFS LPR_WITH_VLC)
      list(APPEND LPR_CAP_LIBS LibVLC::LibVLC)
      message(STATUS "  + VLC backend")
    else()
      message(STATUS "  - VLC requested but libVLC not found; skipping (install VLC SDK / set LIBVLC_SDK_DIR)")
    endif()
  endif()

  if(WITH_GSTREAMER)
    set(_gst_target "")
    # An explicit SDK root (or the official installer's env var) wins over pkg-config: under the
    # vcpkg toolchain pkg-config would otherwise resolve to a vcpkg gstreamer build that may lack
    # plugins like rtspsrc. Point GSTREAMER_ROOT at the official MSVC SDK (e.g. the one the old
    # project used: D:/gstreamer/1.0/msvc_x86_64) to link the complete, plugin-rich runtime.
    if(NOT GSTREAMER_ROOT AND DEFINED ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64})
      set(GSTREAMER_ROOT "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}")
    endif()
    if(GSTREAMER_ROOT)
      find_path(GST_INC     gst/gst.h    HINTS "${GSTREAMER_ROOT}/include/gstreamer-1.0")
      find_path(GLIB_INC    glib.h       HINTS "${GSTREAMER_ROOT}/include/glib-2.0")
      find_path(GLIBCFG_INC glibconfig.h HINTS "${GSTREAMER_ROOT}/lib/glib-2.0/include")
      find_library(GST_LIB     gstreamer-1.0 HINTS "${GSTREAMER_ROOT}/lib")
      find_library(GSTAPP_LIB  gstapp-1.0    HINTS "${GSTREAMER_ROOT}/lib")
      find_library(GOBJECT_LIB gobject-2.0   HINTS "${GSTREAMER_ROOT}/lib")
      find_library(GLIB_LIB    glib-2.0      HINTS "${GSTREAMER_ROOT}/lib")
      if(GST_INC AND GLIB_INC AND GLIBCFG_INC AND GST_LIB AND GSTAPP_LIB AND GOBJECT_LIB AND GLIB_LIB)
        add_library(lpr_gst_sdk INTERFACE)
        target_include_directories(lpr_gst_sdk INTERFACE ${GST_INC} ${GLIB_INC} ${GLIBCFG_INC})
        target_link_libraries(lpr_gst_sdk INTERFACE ${GST_LIB} ${GSTAPP_LIB} ${GOBJECT_LIB} ${GLIB_LIB})
        set(_gst_target lpr_gst_sdk)
        message(STATUS "  + GStreamer backend (SDK: ${GSTREAMER_ROOT})")
      else()
        message(WARNING "GSTREAMER_ROOT='${GSTREAMER_ROOT}' set but headers/libs not found there; "
                        "expected include/gstreamer-1.0, include/glib-2.0, lib/glib-2.0/include, lib/*.lib")
      endif()
    endif()
    # Fallback: pkg-config (Linux, MSYS2, or the Windows SDK exposed via PKG_CONFIG_PATH).
    if(NOT _gst_target AND PKG_CONFIG_FOUND)
      pkg_check_modules(GST IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 glib-2.0 gobject-2.0)
      if(GST_FOUND)
        set(_gst_target PkgConfig::GST)
        message(STATUS "  + GStreamer backend (${GST_gstreamer-1.0_VERSION})")
      endif()
    endif()
    if(_gst_target)
      list(APPEND LPR_CAP_SRC  src/capture/GStreamerCaptureSource.cpp)
      list(APPEND LPR_CAP_DEFS LPR_WITH_GSTREAMER)
      list(APPEND LPR_CAP_LIBS ${_gst_target})
    else()
      message(STATUS "  - GStreamer requested but not found; skipping "
                     "(install GStreamer dev libs; on Windows set GSTREAMER_ROOT or PKG_CONFIG_PATH)")
    endif()
  endif()

  if(WITH_PYLON)
    # Help find_package locate the official Basler SDK's CMake package. The installer sets the
    # PYLON_DEV_DIR env var; PYLON_ROOT can also be passed explicitly (e.g.
    # "C:/Program Files/Basler/pylon 8/Development"). find_package(pylon) then provides the
    # pylon::pylon target with all the version-stamped C++ libs wired in.
    if(NOT PYLON_ROOT AND DEFINED ENV{PYLON_DEV_DIR})
      set(PYLON_ROOT "$ENV{PYLON_DEV_DIR}")
    endif()
    if(PYLON_ROOT)
      list(APPEND CMAKE_PREFIX_PATH
           "${PYLON_ROOT}" "${PYLON_ROOT}/CMake" "${PYLON_ROOT}/CMake/pylon"
           "${PYLON_ROOT}/lib/cmake/pylon")
    endif()
    find_package(pylon QUIET)
    if(pylon_FOUND)
      list(APPEND LPR_CAP_SRC  src/capture/BaslerCaptureSource.cpp)
      list(APPEND LPR_CAP_DEFS LPR_WITH_PYLON)
      list(APPEND LPR_CAP_LIBS pylon::pylon)
      message(STATUS "  + Pylon/Basler backend (pylon ${pylon_VERSION})")
    else()
      message(STATUS "  - Pylon requested but SDK not found; skipping "
                     "(set PYLON_ROOT or pylon_DIR to the Basler pylon 'Development' dir)")
    endif()
  endif()

  add_library(lpr_capture STATIC ${LPR_CAP_SRC})
  target_include_directories(lpr_capture PUBLIC ${LPR_CAP_INCS})
  target_link_libraries(lpr_capture PUBLIC ${LPR_CAP_LIBS})
  if(LPR_CAP_DEFS)
    target_compile_definitions(lpr_capture PUBLIC ${LPR_CAP_DEFS})
  endif()

  # ---- manager (orchestrator: CameraWorker + CameraManager) ----
  # Needs the full config lib (SettingsManager), so only when that is enabled.
  if(LPR_BUILD_CONFIG_LIB)
    add_library(lpr_manager STATIC
      src/manager/CameraWorker.cpp
      src/manager/CameraManager.cpp
      src/manager/DetectionWorker.cpp)
    target_link_libraries(lpr_manager PUBLIC lpr_capture lpr_log lpr_config lpr_json lpr_detect lpr_track lpr_process ${OpenCV_LIBS})
    target_include_directories(lpr_manager PUBLIC include ${OpenCV_INCLUDE_DIRS})
  endif()

  # ---- Basler full facade (vendored as-is, guarded by Pylon) ----
  if(WITH_PYLON AND pylon_FOUND)
    file(GLOB LPR_BASLER_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/basler/*.cpp)
    add_library(lpr_basler STATIC ${LPR_BASLER_SRC})
    target_include_directories(lpr_basler PUBLIC
      include ${CMAKE_CURRENT_SOURCE_DIR}/include/lpr/basler ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(lpr_basler PUBLIC
      lpr_capture lpr_log lpr_config lpr_json pylon::pylon ${OpenCV_LIBS})
    target_compile_definitions(lpr_basler PUBLIC LPR_WITH_PYLON)
    message(STATUS "  + Basler full facade (lpr_basler)")
  endif()

  # ---- detection / OCR (backend-agnostic: OpenVINO / TensorRT / OpenCV-DNN) ----
  set(LPR_DETECT_SRC
    src/detect/CtcDecoder.cpp
    src/detect/InferModel.cpp
    src/detect/OpenCvDnnModel.cpp        # portable CPU path (incl. ARM / Raspberry Pi)
    src/detect/PlateOcr.cpp
    src/detect/EastTextDetector.cpp
    src/detect/PlateRecognizer.cpp
    src/detect/VehicleDetector.cpp
    src/detect/RoiCropRecognizer.cpp)
  set(LPR_DETECT_DEFS "")
  set(LPR_DETECT_LIBS lpr_util lpr_log ${OpenCV_LIBS})

  if(WITH_OPENVINO)
    find_package(OpenVINO QUIET)
    if(OpenVINO_FOUND)
      list(APPEND LPR_DETECT_SRC  src/detect/OpenVinoModel.cpp)
      list(APPEND LPR_DETECT_DEFS LPR_WITH_OPENVINO)
      list(APPEND LPR_DETECT_LIBS openvino::runtime)
      message(STATUS "  + detection backend: OpenVINO")
    elseif(OpenVINO_DIR)
      # A non-empty path was given but no OpenVINOConfig.cmake was there -> fail loudly.
      # (An EMPTY OpenVINO_DIR, e.g. from an unset $env{OpenVINO_DIR} in a preset, is treated
      # as "no hint" and falls through to the soft "not found" below instead of erroring.)
      message(FATAL_ERROR
        "WITH_OPENVINO=ON and OpenVINO_DIR='${OpenVINO_DIR}' was given, but OpenVINOConfig.cmake "
        "was not found there.\n"
        "  Set OpenVINO_DIR to the folder that CONTAINS OpenVINOConfig.cmake, e.g.\n"
        "    <openvino>/runtime/cmake   (archive/installer)\n"
        "  Find it with:  dir /s /b \"C:\\Program Files (x86)\\Intel\\OpenVINOConfig.cmake\"")
    else()
      message(STATUS "  - OpenVINO requested but not found (set -DOpenVINO_DIR=<...>/runtime/cmake)")
    endif()
  endif()

  if(WITH_TENSORRT)
    find_library(NVINFER_LIB nvinfer)
    find_path(NVINFER_INC NvInfer.h)
    find_package(CUDAToolkit QUIET)
    if(NVINFER_LIB AND NVINFER_INC AND CUDAToolkit_FOUND)
      list(APPEND LPR_DETECT_SRC  src/detect/TensorRtModel.cpp)
      list(APPEND LPR_DETECT_DEFS LPR_WITH_TENSORRT)
      list(APPEND LPR_DETECT_LIBS ${NVINFER_LIB} CUDA::cudart)
      message(STATUS "  + detection backend: TensorRT")
    else()
      message(STATUS "  - TensorRT requested but TensorRT/CUDA not found")
    endif()
  endif()

  if(WITH_ONNXRUNTIME)
    # Detect via plain find_* (works with any ORT layout; set -DONNXRUNTIME_ROOT=<sdk>).
    find_path(ORT_INC onnxruntime_cxx_api.h
              HINTS ${ONNXRUNTIME_ROOT}/include /opt/onnxruntime/include
                    /usr/local/include/onnxruntime /usr/include/onnxruntime)
    find_library(ORT_LIB onnxruntime
              HINTS ${ONNXRUNTIME_ROOT}/lib /opt/onnxruntime/lib /usr/local/lib /usr/lib)
    if(ORT_INC AND ORT_LIB)
      list(APPEND LPR_DETECT_SRC  src/detect/OnnxRuntimeModel.cpp)
      list(APPEND LPR_DETECT_DEFS LPR_WITH_ONNXRUNTIME)
      list(APPEND LPR_DETECT_LIBS ${ORT_LIB})
      message(STATUS "  + detection backend: ONNX Runtime")
    elseif(DEFINED ONNXRUNTIME_ROOT)
      message(FATAL_ERROR
        "WITH_ONNXRUNTIME=ON and ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}' was given, but the SDK was not "
        "found there (need <root>/include/onnxruntime_cxx_api.h and <root>/lib/onnxruntime).")
    else()
      message(STATUS "  - ONNX Runtime requested but not found (set -DONNXRUNTIME_ROOT=<sdk>)")
    endif()
  endif()

  if(WITH_HAILO)
    find_library(HAILORT_LIB hailort)
    find_path(HAILORT_INC hailo/hailort.hpp)
    if(HAILORT_LIB AND HAILORT_INC)
      list(APPEND LPR_DETECT_SRC  src/detect/HailoModel.cpp)
      list(APPEND LPR_DETECT_DEFS LPR_WITH_HAILO)
      list(APPEND LPR_DETECT_LIBS ${HAILORT_LIB})
      message(STATUS "  + detection backend: Hailo NPU")
    else()
      message(STATUS "  - Hailo requested but HailoRT not found")
    endif()
  endif()

  add_library(lpr_detect STATIC ${LPR_DETECT_SRC})
  target_include_directories(lpr_detect PUBLIC include ${OpenCV_INCLUDE_DIRS})
  if(WITH_TENSORRT AND NVINFER_INC)
    target_include_directories(lpr_detect PUBLIC ${NVINFER_INC})
  endif()
  if(WITH_ONNXRUNTIME AND ORT_INC)
    target_include_directories(lpr_detect PUBLIC ${ORT_INC})
  endif()
  target_link_libraries(lpr_detect PUBLIC ${LPR_DETECT_LIBS})
  if(LPR_DETECT_DEFS)
    target_compile_definitions(lpr_detect PUBLIC ${LPR_DETECT_DEFS})
  endif()

  # ---- tracking (vendored ByteTrack behind a clean VehicleTracker; needs Eigen) ----
  # Eigen is confined to this target: detection alone stays Eigen-free.
  find_package(Eigen3 3.3 QUIET NO_MODULE)
  if(NOT Eigen3_FOUND)
    find_path(EIGEN3_INCLUDE_DIR NAMES Eigen/Dense PATH_SUFFIXES eigen3)
  endif()
  if(Eigen3_FOUND OR EIGEN3_INCLUDE_DIR)
    add_library(lpr_track STATIC
      src/track/bytetrack/kalmanFilter.cpp
      src/track/bytetrack/lapjv.cpp
      src/track/bytetrack/STrack.cpp
      src/track/bytetrack/BYTETracker.cpp
      src/track/bytetrack/utils.cpp
      src/track/VehicleTracker.cpp
      src/track/VehicleAwarePlateRecognizer.cpp
      src/track/PlateTrackingRecognizer.cpp)
    target_include_directories(lpr_track PUBLIC include ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(lpr_track PUBLIC lpr_util lpr_log lpr_detect ${OpenCV_LIBS})
    if(TARGET Eigen3::Eigen)
      target_link_libraries(lpr_track PUBLIC Eigen3::Eigen)
    else()
      target_include_directories(lpr_track PUBLIC ${EIGEN3_INCLUDE_DIR})
    endif()
    message(STATUS "  + tracking: ByteTrack + VehicleTracker (Eigen found)")
  else()
    message(FATAL_ERROR "lpr_track needs Eigen3. Install libeigen3-dev (Linux), add 'eigen3' "
                        "via vcpkg (Windows), or pass -DEIGEN3_INCLUDE_DIR=<path-to-eigen>.")
  endif()

  # ---- plate processing (validation + per-track consensus + send-once dedup) ----
  add_library(lpr_process STATIC src/process/PlateProcessor.cpp src/process/DirectionEstimator.cpp)
  target_include_directories(lpr_process PUBLIC include ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(lpr_process PUBLIC lpr_core lpr_detect lpr_json ${OpenCV_LIBS})
  message(STATUS "  + plate processing: PlateProcessor")

  # ---- services (recording, live view, command routing) ----
  add_library(lpr_services STATIC
    src/services/RecordingService.cpp
    src/services/LiveViewService.cpp
    src/services/CommandRouter.cpp)
  target_include_directories(lpr_services PUBLIC include ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(lpr_services PUBLIC lpr_log lpr_json ${OpenCV_LIBS})
  message(STATUS "  + services: Recording + LiveView + CommandRouter")

  # ---- messaging (plate sending + camera connect/disconnect; transport behind IMessageTransport) ----
  set(LPR_NET_SRC src/net/PlateSender.cpp src/net/CameraStatusNotifier.cpp src/net/MediaSender.cpp src/net/HeartbeatMonitor.cpp src/net/SystemResourceMonitor.cpp)
  set(LPR_NET_LIBS lpr_core lpr_detect lpr_json lpr_log ${OpenCV_LIBS})
  if(WITH_NATS)
    find_path(NATS_INC nats/nats.h)
    find_library(NATS_LIB NAMES nats nats_static cnats libnats libnats_static)
    if(NATS_INC AND NATS_LIB)
      list(APPEND LPR_NET_SRC src/net/NatsTransport.cpp)
      list(APPEND LPR_NET_LIBS ${NATS_LIB})
      message(STATUS "  + messaging: NATS transport (cnats found: ${NATS_LIB})")
    else()
      message(FATAL_ERROR
        "WITH_NATS=ON but the cnats SDK was not found (NATS_INC=${NATS_INC}, NATS_LIB=${NATS_LIB}).\n"
        "  - With vcpkg: configure a CLEAN build dir with -DVCPKG_MANIFEST_FEATURES=nats so cnats installs first.\n"
        "  - Or point CMake at a cnats build: -DNATS_INC=<.../include> -DNATS_LIB=<.../nats_static.lib>\n"
        "  - Or build offline-only by omitting -DWITH_NATS=ON.\n"
        "  (find_path/find_library results are cached; delete the build dir or the NATS_INC/NATS_LIB cache vars and reconfigure.)")
    endif()
  endif()
  add_library(lpr_net STATIC ${LPR_NET_SRC})
  target_include_directories(lpr_net PUBLIC include ${OpenCV_INCLUDE_DIRS})
  if(WITH_NATS AND NATS_INC)
    target_include_directories(lpr_net PUBLIC ${NATS_INC})
  endif()
  target_link_libraries(lpr_net PUBLIC ${LPR_NET_LIBS})
  message(STATUS "  + messaging: PlateSender + CameraStatusNotifier")

  # ---- application (top-level wiring + main entry point) ----
  add_library(lpr_app STATIC src/app/Application.cpp)
  target_include_directories(lpr_app PUBLIC include ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(lpr_app PUBLIC
    lpr_manager lpr_track lpr_process lpr_services lpr_net lpr_detect
    lpr_capture lpr_config lpr_core lpr_json ${OpenCV_LIBS})
  if(WITH_NATS AND NATS_INC)
    target_compile_definitions(lpr_app PUBLIC LPR_WITH_NATS)
  endif()
  if(WITH_PYLON AND pylon_FOUND)
    target_link_libraries(lpr_app PUBLIC lpr_basler)
    target_compile_definitions(lpr_app PUBLIC LPR_WITH_PYLON)
  endif()

  add_executable(lpr_client src/app/main.cpp)
  target_link_libraries(lpr_client PRIVATE lpr_app)
  set_target_properties(lpr_client PROPERTIES OUTPUT_NAME lpr)
  message(STATUS "  + application: lpr (executable)")

  # ---- Windows: bundle the vcpkg GStreamer *plugins* next to lpr.exe so RTSP cameras work
  #      without the user setting GST_PLUGIN_PATH. vcpkg's applocal step already copies the
  #      core DLLs, but NOT the plugins (rtspsrc, depay/parse, libav, videoconvert, appsink),
  #      which live in a separate plugin dir. The exe finds them via setupBundledPluginPath().
  #      Skipped when GStreamer came from the official SDK (it finds its own plugins). ----
  if(WIN32 AND WITH_GSTREAMER AND _gst_target)
    set(_vcpkg_inst "${VCPKG_INSTALLED_DIR}")
    if(NOT _vcpkg_inst)
      set(_vcpkg_inst "${CMAKE_BINARY_DIR}/vcpkg_installed")
    endif()
    set(_triplet "${VCPKG_TARGET_TRIPLET}")
    if(NOT _triplet)
      set(_triplet "x64-windows")
    endif()
    set(_gst_base "${_vcpkg_inst}/${_triplet}")
    set(_gst_sub "")
    if(EXISTS "${_gst_base}/plugins/gstreamer")
      set(_gst_sub "plugins/gstreamer")
    elseif(EXISTS "${_gst_base}/lib/gstreamer-1.0")
      set(_gst_sub "lib/gstreamer-1.0")
    endif()
    if(_gst_sub)
      add_custom_command(TARGET lpr_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_gst_base}/$<$<CONFIG:Debug>:debug/>${_gst_sub}"
                "$<TARGET_FILE_DIR:lpr_client>/gstreamer-1.0"
        COMMENT "Bundling GStreamer plugins next to lpr.exe")
      if(EXISTS "${_gst_base}/tools/gstreamer/gst-plugin-scanner.exe")
        add_custom_command(TARGET lpr_client POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${_gst_base}/$<$<CONFIG:Debug>:debug/>tools/gstreamer/gst-plugin-scanner.exe"
                  "$<TARGET_FILE_DIR:lpr_client>")
      endif()
      message(STATUS "  + GStreamer plugins bundled next to lpr.exe (from ${_gst_base}/${_gst_sub})")
    else()
      message(STATUS "  * GStreamer plugins not found under ${_gst_base}; "
                     "the vcpkg gstreamer port may lack rtspsrc - set GST_PLUGIN_PATH at runtime "
                     "or use the official GStreamer SDK")
    endif()
  endif()

  # ---- Windows: copy ALL dependent DLLs next to lpr.exe so it runs from the build
  #      folder without hunting for DLLs. $<TARGET_RUNTIME_DLLS> resolves the DLLs of
  #      every imported target the exe links (OpenCV, ONNX Runtime, NATS, OpenVINO core,
  #      …). It complements vcpkg's applocal step and is idempotent (copy_if_different).
  if(WIN32)
    add_custom_command(TARGET lpr_client POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_RUNTIME_DLLS:lpr_client> $<TARGET_FILE_DIR:lpr_client>
      COMMAND_EXPAND_LISTS
      COMMENT "Copying dependent DLLs next to lpr.exe")
  endif()

  # ---- Windows: OpenVINO plugins + TBB. OpenVINO loads its device plugins
  #      (openvino_intel_cpu_plugin.dll, …) and TBB *dynamically* via plugins.xml, so
  #      $<TARGET_RUNTIME_DLLS> does NOT catch them. Copy the runtime bin dir (DLLs +
  #      plugins.xml) and TBB next to the exe. Derived from OpenVINO_DIR (…/runtime/cmake);
  #      override with -DLPR_OPENVINO_RUNTIME_DIR=<…/runtime/bin/intel64> if your layout differs.
  if(WIN32 AND WITH_OPENVINO AND OpenVINO_FOUND)
    set(LPR_OPENVINO_RUNTIME_DIR "" CACHE PATH
        "OpenVINO runtime bin dir (contains openvino*.dll, *_plugin.dll, plugins.xml)")
    set(_ov_binroot "")
    if(LPR_OPENVINO_RUNTIME_DIR AND EXISTS "${LPR_OPENVINO_RUNTIME_DIR}")
      set(_ov_binroot "${LPR_OPENVINO_RUNTIME_DIR}")
    elseif(OpenVINO_DIR)
      get_filename_component(_ov_root "${OpenVINO_DIR}/../.." ABSOLUTE)
      if(EXISTS "${_ov_root}/runtime/bin/intel64")
        set(_ov_binroot "${_ov_root}/runtime/bin/intel64/$<IF:$<CONFIG:Debug>,Debug,Release>")
        set(_ov_tbb "${_ov_root}/runtime/3rdparty/tbb/bin")
      endif()
    endif()
    if(_ov_binroot)
      add_custom_command(TARGET lpr_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_ov_binroot}" "$<TARGET_FILE_DIR:lpr_client>"
        COMMENT "Bundling OpenVINO runtime + plugins next to lpr.exe")
      if(_ov_tbb AND EXISTS "${_ov_tbb}")
        add_custom_command(TARGET lpr_client POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_directory "${_ov_tbb}" "$<TARGET_FILE_DIR:lpr_client>")
      endif()
      message(STATUS "  + OpenVINO runtime+plugins will be bundled next to lpr.exe")
    else()
      message(STATUS "  * OpenVINO plugins NOT auto-bundled (layout not found); "
                     "keep the OpenVINO 'bin' dir on PATH or set -DLPR_OPENVINO_RUNTIME_DIR")
    endif()
  endif()

  # ---- Windows: bundle the COMPLETE Pylon runtime next to lpr.exe (self-contained GigE).
  #      IMPORTANT: $<TARGET_RUNTIME_DLLS> above copies ONLY the 3 directly-linked Pylon DLLs
  #      (PylonBase/PylonUtility/PylonGUI). The GenICam stack (GenApi/GCBase/NodeMapData/
  #      XmlParser/log4cpp) and the GenTL *.cti producers are loaded DYNAMICALLY, so they are
  #      NOT caught → a PARTIAL app-local Pylon that BREAKS GigE camera discovery (it also
  #      becomes sensitive to a foreign zlib like OpenCV's z.dll shadowing the GenICam one).
  #      Copying the WHOLE runtime makes the app self-consistent AND removes the need to install
  #      the full Pylon SDK on an air-gapped target (only the GigE filter driver is still needed).
  if(WIN32 AND WITH_PYLON AND pylon_FOUND)
    get_filename_component(_pylon_rt "${PYLON_ROOT}/../Runtime/x64" ABSOLUTE)
    if(EXISTS "${_pylon_rt}")
      add_custom_command(TARGET lpr_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_pylon_rt}" "$<TARGET_FILE_DIR:lpr_client>"
        COMMENT "Bundling the COMPLETE Pylon runtime next to lpr.exe (self-contained GigE)")
      message(STATUS "  + Pylon runtime bundled next to lpr.exe (from ${_pylon_rt})")
    else()
      message(STATUS "  * Pylon runtime dir not found at ${_pylon_rt}; the target then needs the "
                     "full Pylon runtime installed (GENICAM_GENTL64_PATH) for camera discovery")
    endif()
  endif()

  message(STATUS "  + detection backend: OpenCV-DNN (always; portable CPU/ARM)")

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm|armv7")
    message(STATUS "  * ARM target detected (${CMAKE_SYSTEM_PROCESSOR}): fastest path is the Pi AI HAT+ "
                   "NPU via WITH_HAILO, else ONNX Runtime (XNNPACK); OpenCV-DNN is the last-resort fallback")
  endif()
else()
  message(STATUS "OpenCV not found -> skipping lpr_capture (set a toolchain/vcpkg with opencv4)")
endif()

# ---- nlohmann::json: system package if present, else the vendored single header ----
find_package(nlohmann_json QUIET)
add_library(lpr_json INTERFACE)
if(nlohmann_json_FOUND)
  target_link_libraries(lpr_json INTERFACE nlohmann_json::nlohmann_json)
else()
  target_include_directories(lpr_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
endif()
# Pin the json ABI project-wide so every TU agrees on layout (see JsonAbi.h).
target_compile_definitions(lpr_json INTERFACE
  JSON_DIAGNOSTICS=0 JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON=0)

# ---- the config module (full library needs OpenCV for the camera/ROI classes) ----
if(LPR_BUILD_CONFIG_LIB)
  if(NOT OpenCV_FOUND)
    message(FATAL_ERROR
      "LPR_BUILD_CONFIG_LIB=ON needs OpenCV, but none was found.\n"
      "  * Easiest:  use the 'full-vcpkg' preset (vcpkg builds OpenCV for you), or\n"
      "  * Prebuilt: cmake --preset full -DOpenCV_DIR=C:/path/to/opencv/build , or\n"
      "  * SDK-free: cmake --preset tests-debug  (no OpenCV, skips capture/config).")
  endif()
  add_library(lpr_config
      src/config/AppConfig.cpp
      src/config/LprSettings.cpp
      src/config/CameraSettings.cpp
      src/config/CameraLineCache.cpp
      src/config/RoiMaskCalculator.cpp
      src/config/SettingsManager.cpp)
  target_include_directories(lpr_config PUBLIC include ${OpenCV_INCLUDE_DIRS})
  target_link_libraries(lpr_config PUBLIC lpr_json lpr_log ${OpenCV_LIBS})
endif()


if(LPR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/config)
  add_subdirectory(tests/util)
  add_subdirectory(tests/log)
  add_subdirectory(tests/capture)
  add_subdirectory(tests/manager)
  add_subdirectory(tests/detect)
  add_subdirectory(tests/track)
  add_subdirectory(tests/process)
  add_subdirectory(tests/services)
  add_subdirectory(tests/net)
endif()
