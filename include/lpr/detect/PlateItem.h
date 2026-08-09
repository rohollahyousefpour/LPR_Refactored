#pragma once
// PlateItem (was PlateData) - one recognized plate flowing through the PlateQueue.
#include "lpr/detect/PlateResult.h"
#include "lpr/util/BlockingQueue.h"
#include <opencv2/core.hpp>
#include <string>

namespace lpr {

struct PlateItem {
    cv::Mat     image;     // full/context frame
    PlateResult plate;
    std::string gate;
    long        timestamp = 0;
    // When true, PlateSender skips the per-plate cooldown for this item — used for a
    // direction-correction of an already-announced pass (same passId).
    bool        forceSend = false;
};

using PlateQueue = BlockingQueue<PlateItem>;

} // namespace lpr
