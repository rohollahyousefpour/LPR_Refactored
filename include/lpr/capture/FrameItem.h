#pragma once
// FrameItem (was ImageData) - one captured frame flowing through the FrameQueue.
#include <opencv2/core.hpp>
#include <string>

namespace lpr {

struct FrameItem {
    cv::Mat     image;          // DETECTION frame: plate/vehicle recognition runs on this (mono in a pair)
    cv::Mat     evidenceImage;  // optional RGB evidence frame for the plate message's full_image;
                                // empty for a single camera -> full_image falls back to `image`
    long        timestamp = 0;
    std::string gate;
};

} // namespace lpr
