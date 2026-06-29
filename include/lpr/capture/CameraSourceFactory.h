#pragma once
// CameraSourceFactory - creates a fully-configured CaptureSource from a config.
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <opencv2/core.hpp>          // cv::Rect (Pylon ROI)
#include "lpr/capture/CameraKind.h"
#include "lpr/capture/CaptureSource.h"

namespace lpr {

struct CameraSourceParams {
    std::string typeOfLink;   // "video" | "rtsp" | "gstreamer" | "pylon" | "grey"
    std::string address;
    std::string monoAddress;  // "" or "-1" => skip
    int         delayMs = 50;
    cv::Rect    roi;          // Pylon/Basler
    std::string gate;
};

struct CameraSourceError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class CameraSourceFactory {
public:
    // Never returns null; throws CameraSourceError on unknown/unported/disabled kinds.
    static std::unique_ptr<CaptureSource> create(const CameraSourceParams& p);

    // The full Basler facade lives in lpr_basler, which depends on lpr_capture; to avoid a
    // circular dependency the app registers a creator here (under WITH_PYLON). When set, Pylon
    // cameras are built by the facade (trigger/exposure/gain/sync/bandwidth all live); when unset,
    // a minimal direct-grab BaslerCaptureSource is used as a fallback.
    using Creator = std::function<std::unique_ptr<CaptureSource>(const CameraSourceParams&)>;
    static void setBaslerCreator(Creator c);
};

} // namespace lpr
