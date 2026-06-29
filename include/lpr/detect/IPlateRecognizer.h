#pragma once
// IPlateRecognizer - the seam DetectionWorker depends on, so any recognizer (the
// real PlateRecognizer, or a test double) can be plugged in without coupling.
#include "lpr/detect/PlateResult.h"
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace lpr {

// One detected vehicle on the most recent recognize() call (source-frame coords once mapped back).
struct VehicleAnnotation {
    cv::Rect box;
    int      trackId = -1;
    int      classId = -1;
};

class IPlateRecognizer {
public:
    virtual ~IPlateRecognizer() = default;
    virtual std::vector<PlateResult> recognize(const cv::Mat& frame,
                                               const std::string& gate, long timestamp) = 0;

    // Optional: vehicles detected on the last recognize() call (vehicle mode only). Default: none.
    // Coordinates are in the space passed to recognize(); wrappers (RoiCrop) remap to source.
    virtual std::vector<VehicleAnnotation> lastVehicles() const { return {}; }
    // Cumulative count of distinct vehicles seen so far (via tracking); -1 if not counted.
    virtual int totalVehicleCount() const { return -1; }
};

} // namespace lpr
