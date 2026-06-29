#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <string>

namespace lpr {

class RoiMaskCalculator {
public:
    std::pair<cv::Mat, cv::Rect> compute(
        const std::string& key,
        const std::vector<cv::Point>& polygon,
        const cv::Size& imageSize
    ) const;

private:
    struct MaskData { cv::Mat mask; cv::Rect rect; };
    mutable std::unordered_map<std::string, MaskData> cache_;
    mutable std::mutex mutex_;
};

} // namespace lpr