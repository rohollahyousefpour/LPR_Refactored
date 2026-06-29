#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include <optional>

namespace lpr {

class CameraLineCache {
public:
    void cacheLines(
        const std::string& camId,
        const std::unordered_map<int, std::vector<cv::Point2f>>& normLines,
        int width,
        int height
    ) const;

    std::optional<std::vector<cv::Point>> getLine(
        const std::string& camId,
        int lineIndex
    ) const;

private:
    mutable std::mutex mutex_;
    mutable std::unordered_map< std::string,
        std::unordered_map<int, std::vector<cv::Point>>
    > pixelLines_;
};

} // namespace lpr