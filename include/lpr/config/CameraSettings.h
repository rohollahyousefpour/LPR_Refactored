#pragma once

#include "lpr/config/JsonAbi.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <opencv2/opencv.hpp>

namespace lpr {

class CameraSettings {
public:
    using json = nlohmann::json;

    explicit CameraSettings(int cameraId);

    void load(const json& cameraJson);

    std::optional<json> getAll() const;

    template <typename T>
    std::optional<T> get(const std::string& key) const;

    const std::unordered_map<int, std::vector<cv::Point2f>>& getAllLineNormals() const;
    std::vector<cv::Point2f> getPoints() const;

private:
    int cameraId_;
    json values_;
    std::unordered_map<int, std::vector<cv::Point2f>> linesNorm_;
    std::vector<cv::Point2f> pointsNorm_;
    mutable std::mutex mutex_;
};

} // namespace lpr