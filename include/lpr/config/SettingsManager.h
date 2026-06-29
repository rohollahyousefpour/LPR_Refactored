#pragma once

#include "lpr/config/LprSettings.h"
#include "lpr/config/CameraSettings.h"
#include "lpr/config/CameraLineCache.h"
#include "lpr/config/RoiMaskCalculator.h"
#include "lpr/config/JsonAbi.h"
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>
#include <mutex>

namespace lpr {

class SettingsManager {
public:
    static SettingsManager& instance();

    bool isPointInsidePolygon(const cv::Point& point, std::string gate_id, int cols, int rows);

    void loadAll(const nlohmann::json& messageBody);

    template<typename T>
    std::optional<T> getLpr(const std::string& key) const;

    int get_num_camera();

    std::vector<int> getCameraIds() const;

    std::vector<cv::Point2f> getCameraPoints(int cameraId) const;
    std::vector<cv::Point> getCameraPoints(int cameraId, int width, int height) const;

    std::optional<std::reference_wrapper<const CameraSettings>> getCameraSettings(int cameraId) const;

    cv::Rect getGeneralRoi(int cameraId, int width, int height) const;
    cv::Mat cropAndMask(int cameraId, const cv::Mat& img) const;
    

    int getLinePointsCount(int cameraId) const;
    int getLineContainingPoint(int cameraId, const cv::Point& pt, int width, int height) const;
    cv::Rect getMaskROI(int cameraId, int width, int height) const;

    template<typename T>
    std::optional<T> getCameraSettingByIdAndKey(int cameraId, const std::string& key) const;

private:
    SettingsManager() = default;
    int num_cam = 0;

    // Helper to construct cache key
    static std::string makePtsKey(int cameraId, int width, int height) {
        return std::to_string(cameraId) + "_" + std::to_string(width) + "x" + std::to_string(height);
    }

    LprSettings lpr_;
    std::unordered_map<int, CameraSettings> cameras_;
    mutable CameraLineCache   lineCache_;
    mutable RoiMaskCalculator roiCalc_;
    // Cache scaled points and protect with mutex
    mutable std::unordered_map<std::string, std::vector<cv::Point>> cameraPointsCache_;
    mutable std::mutex cameraCacheMutex_;
    static  bool samePt(const cv::Point& a, const cv::Point& b) {
        return a.x == b.x && a.y == b.y;
    }

    void printPts(std::vector<cv::Point> pts, std::string name="pyu") const;

    static std::vector<cv::Point> sanitizePolygon(const std::vector<cv::Point>& in);
};

} // namespace lpr