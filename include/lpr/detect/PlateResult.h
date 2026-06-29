#pragma once
#include <opencv2/core.hpp>
#include <string>

namespace lpr {

struct PlateResult {
    std::string     text;            // recognized plate string
    float           confidence = 0.f;
    cv::RotatedRect box;             // location in the source frame
    cv::Mat         plateImage;      // cropped plate
    long            timestamp = 0;
    std::string     gate;
    int             trackId = -1;    // vehicle track id (when produced via tracking), else -1
    cv::Rect        vehicleBox;      // owning vehicle box in the source frame (if any)
    int             direction = 0;   // 0 unknown, 1 entering (plate growing), 2 exiting (shrinking)
};

} // namespace lpr
