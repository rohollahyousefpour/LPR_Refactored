#pragma once
//
// CameraContext
// -------------
// The shared, mutable state for ONE BaslerCamera instance (one camera or one
// RGB+Mono pair). The facade owns it; CapturePipeline and ConnectionSupervisor
// hold a reference to it. Bundling the state here keeps their constructors
// small and gives one clear answer to "who guards what".
//
// THREADING INVARIANT (important, relied upon for pointer safety):
//   * devices/exposure/syncRole are guarded by devMutex.
//   * Devices are INSERTED by reconnect workers (off the capture thread) and
//     REMOVED only on the capture thread (CapturePipeline / handleFault).
//   * Therefore raw CameraDevice* obtained on the capture thread stay valid for
//     that capture iteration -- nothing else destroys them concurrently.
//
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>

#include "CameraDevice.h"
#include "IExposureStrategy.h"
#include "ISyncConfigurator.h"
#include "CommandQueue.h"
#include "IFrameSink.h"

struct CameraContext {
    std::mutex devMutex;   // guards the three maps below

    std::map<std::string, std::unique_ptr<CameraDevice>>      devices;   // serial -> device
    std::map<std::string, std::unique_ptr<IExposureStrategy>> exposure;  // serial -> policy
    std::map<std::string, std::shared_ptr<ISyncConfigurator>> syncRole;  // serial -> role
    // Latest grabbed BGR frame per serial (guarded by devMutex). Lets the manual-control
    // live view fetch EACH sensor of a pair by its own serial -- the paired frame bus
    // (onFramePair) collapses the two Mats and loses which serial is which, so a same-kind
    // (e.g. color+color) pair could otherwise be shown/labelled swapped. Fetch-by-serial is
    // unambiguous. Kept small (one frame per camera) and only read on demand.
    std::map<std::string, cv::Mat>                            lastFrames; // serial -> latest BGR

    CommandQueue      commands;          // thread-safe on its own
    std::atomic<bool> live{false};

    int  delay       = 33;   // ms between capture cycles (set from set_adress)
    int  grabTimeoutMs = 1000; // MUST exceed a triggered slave's frame period
    int  expectedCount = 1;  // 1 = lone camera, 2 = RGB+Mono pair

    IFrameSink* sink = nullptr;  // non-owning; set by the facade after build

    // Non-owning; set by the facade to the supervisor. Lets manual commands
    // suspend/restore the per-camera auto-exposure loop. May be null.
    IExposureController* exposureController = nullptr;
};
