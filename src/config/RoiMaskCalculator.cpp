#include "lpr/config/RoiMaskCalculator.h"
#include <opencv2/imgproc.hpp>
#include "lpr/Log.h"   // log via your Boost.Log logger

namespace lpr {

std::pair<cv::Mat, cv::Rect> RoiMaskCalculator::compute(
    const std::string& key,
    const std::vector<cv::Point>& polygon,
    const cv::Size& imageSize
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        //LOGT() << "[RoiMaskCalculator] cache hit key='" << key << "'";
        return { it->second.mask, it->second.rect };
    }

    /*LOGD() << "[RoiMaskCalculator] compute key='" << key << "'"
           << " polygonPts=" << polygon.size()
           << " imageSize=" << imageSize.width << "x" << imageSize.height;*/

    cv::Mat mask = cv::Mat::zeros(imageSize, CV_8UC1);
    if (!polygon.empty()) {
        std::vector<std::vector<cv::Point>> contours{ polygon };
        cv::fillPoly(mask, contours, cv::Scalar(255));
    }
    cv::Rect bbox = cv::Rect();
    if (!polygon.empty()) {
        cv::Rect rawBBox = cv::boundingRect(polygon);
        cv::Rect imageRect(0, 0, imageSize.width, imageSize.height);
        bbox = rawBBox & imageRect;  // Safe clamped bbox
    }
    cache_[key] = { mask, bbox };

   /* LOGD() << "[RoiMaskCalculator] stored key='" << key << "'"
           << " bbox=" << bbox.x << "," << bbox.y << " " << bbox.width << "x" << bbox.height;*/

    return { mask, bbox };
}

} // namespace lpr