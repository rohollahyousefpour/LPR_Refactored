// =============== DIAGNOSTIC VERSION - ULTRA VERBOSE ===============
// Replace src/SettingsManager_.cpp with this version
// It will show EXACTLY which line crashes

#include "lpr/config/JsonAbi.h"      // FIRST: pin json ABI macros before json.hpp is pulled in
#include "lpr/config/SettingsManager.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>            // probe
#include <thread>             // probe (std::this_thread::get_id)
#include <string>             // probe
#include "lpr/Log.h"        // probe: your Boost.Log logger

namespace lpr {

#ifndef PROBE_FLUSH
#define PROBE_FLUSH() boost::log::core::get()->flush()
#endif

SettingsManager& SettingsManager::instance() {
    static SettingsManager mgr;
    return mgr;
}

bool SettingsManager::isPointInsidePolygon(const cv::Point& point, std::string gate_id, int cols, int rows) {
    std::vector<cv::Point>  polygon = getCameraPoints(std::atoi(gate_id.c_str()), cols, rows);
    if (polygon.size() > 0) {
        double result = cv::pointPolygonTest(polygon, point, false);
        return (result >= 0);
    }
    return true;
}

// =============== ULTRA-VERBOSE DIAGNOSTIC VERSION ===============
void SettingsManager::loadAll(const nlohmann::json& messageBody) {
    // ===================== CRASH-DIAGNOSIS PROBE (added) =====================
    // This block runs BEFORE any other access. It logs via AppLogger and flushes
    // each marker so the last line before a hard crash survives. Compare the
    // "sizeof(json)" value here with the one logged on the NATS side: if they
    // differ, you have an ABI / JSON_DIAGNOSTICS mismatch and that is the crash.
    {
        std::ostringstream tid; tid << std::this_thread::get_id();
        LOGI() << "=== loadAll ENTRY ==="
               << " tid=" << tid.str()
               << " &messageBody=" << static_cast<const void*>(&messageBody)
               << " json_version=" << NLOHMANN_JSON_VERSION_MAJOR
               << "." << NLOHMANN_JSON_VERSION_MINOR
               << "." << NLOHMANN_JSON_VERSION_PATCH
               << " sizeof(json)=" << sizeof(nlohmann::json);
        PROBE_FLUSH();
        // ABI fingerprint for THIS file + addresses of the singleton members.
        // Compare sizeof(json)/sizeof(LprSettings)/sizeof(CameraSettings) with
        // the [ABI] lines from LprSettings.cpp, CameraSettings.cpp, NatsClient.cpp.
        // Any mismatch == inconsistent JSON_DIAGNOSTICS == the mutex crash.
        LOGI() << "[ABI][SettingsManager_.cpp]"
               << " JSON_DIAGNOSTICS=" << (int)JSON_DIAGNOSTICS
               << " sizeof(json)=" << sizeof(nlohmann::json)
               << " sizeof(LprSettings)=" << sizeof(LprSettings)
               << " sizeof(CameraSettings)=" << sizeof(CameraSettings)
               << " sizeof(SettingsManager)=" << sizeof(SettingsManager);
        PROBE_FLUSH();
        LOGI() << "[loadAll] this=" << static_cast<const void*>(this)
               << " &lpr_=" << static_cast<const void*>(&lpr_);
        PROBE_FLUSH();
#if JSON_DIAGNOSTICS
        LOGI() << "[loadAll] JSON_DIAGNOSTICS = 1 (ON) in this translation unit";
#else
        LOGI() << "[loadAll] JSON_DIAGNOSTICS = 0 (OFF) in this translation unit";
#endif
        PROBE_FLUSH();
        // If the json layout is wrong (ABI mismatch) or the reference is dangling
        // (use-after-free), the crash happens on this dump(). If the log shows the
        // ENTRY line above but NOT a "dump OK" line below, that is your answer.
        try {
            std::string d = messageBody.dump();
            LOGI() << "[loadAll] dump OK, bytes=" << d.size(); PROBE_FLUSH();
            LOGD() << "[loadAll] dump="
                   << (d.size() > 4000 ? d.substr(0, 4000) + " ...(truncated)" : d);
            PROBE_FLUSH();
        }
        catch (const std::exception& e) {
            LOGE() << "[loadAll] dump() THREW std::exception: " << e.what(); PROBE_FLUSH();
        }
        catch (...) {
            LOGE() << "[loadAll] dump() THREW a non-std exception"; PROBE_FLUSH();
        }
        LOGI() << "[loadAll] probe done -> entering normal logic"; PROBE_FLUSH();
    }
    // =================== END CRASH-DIAGNOSIS PROBE ===========================

    LOGI() << "=== [SETTINGS MANAGER] loadAll() STARTED ===";
    PROBE_FLUSH();
    
    try {
        LOGI() << "[STEP 1] Extracting root data...";
        PROBE_FLUSH();
        
        // Store the sub-object by VALUE, not by reference
        const nlohmann::json root = messageBody.contains("data")
            ? messageBody["data"]       // ← copy, not ref — safe
            : messageBody;

        // Then pass settings by value too, or ensure root lives long enough
        lpr_.load(root["settings"]);    // now root is a real object, not a dangling ref
        
        LOGI() << "[STEP 1] [OK] Root data extracted";
        PROBE_FLUSH();

        // Load LPR settings
        LOGI() << "[STEP 2] Checking for LPR settings...";
        PROBE_FLUSH();
        
        if (root.contains("settings")) {
            LOGI() << "[STEP 2] Found settings, loading...";
            PROBE_FLUSH();
            
            try {
                lpr_.load(root["settings"]);
                LOGI() << "[STEP 2] [OK] LPR settings loaded successfully";
                PROBE_FLUSH();
            }
            catch (const std::exception& e) {
                LOGE() << "[STEP 2] [FAIL] ERROR loading LPR settings: " << e.what();
                PROBE_FLUSH();
                // Continue anyway
            }
        } else {
            LOGW() << "[STEP 2] No 'settings' field found";
            PROBE_FLUSH();
        }

        // Clear cameras
        LOGI() << "[STEP 3] Clearing camera cache...";
        PROBE_FLUSH();
        
        {
            //std::lock_guard<std::mutex> lock(cameraCacheMutex_);
            cameraPointsCache_.clear();
        }
        cameras_.clear();
        num_cam = 0;
        
        LOGI() << "[STEP 3] [OK] Cache cleared";
        PROBE_FLUSH();

        // Load cameras
        LOGI() << "[STEP 4] Checking for camera_data array...";
        PROBE_FLUSH();
        
        if (root.contains("cameras_data")) {
            LOGI() << "[STEP 4] Found cameras_data, is_array: " << root["cameras_data"].is_array();
            PROBE_FLUSH();
            
            if (root["cameras_data"].is_array()) {
                int cam_count = 0;
                for (auto& camJson : root["cameras_data"]) {
                    cam_count++;
                    LOGI() << "[CAMERA " << cam_count << "] Processing camera...";
                    PROBE_FLUSH();
                    
                    try {
                        LOGI() << "[CAMERA " << cam_count << ".1] Checking camera_id...";
                        PROBE_FLUSH();
                        
                        if (!camJson.contains("camera_id")) {
                            LOGE() << "[CAMERA " << cam_count << ".1] [FAIL] No camera_id, skipping";
                            PROBE_FLUSH();
                            continue;
                        }
                        
                        LOGI() << "[CAMERA " << cam_count << ".2] Extracting camera_id...";
                        PROBE_FLUSH();
                        
                        int id = -1;
                        try {
                            id = camJson["camera_id"].get<int>();
                            LOGI() << "[CAMERA " << cam_count << ".2] [OK] Got camera ID: " << id;
                            PROBE_FLUSH();
                        }
                        catch (const std::exception& e) {
                            LOGE() << "[CAMERA " << cam_count << ".2] [FAIL] ERROR extracting camera_id: " << e.what();
                            PROBE_FLUSH();
                            continue;
                        }
                        
                        LOGI() << "[CAMERA " << cam_count << ".3] Creating camera object...";
                        PROBE_FLUSH();
                        
                        cameras_.emplace(
                            std::piecewise_construct,
                            std::forward_as_tuple(id),
                            std::forward_as_tuple(id)
                        );
                        
                        LOGI() << "[CAMERA " << cam_count << ".3] [OK] Camera object created";
                        PROBE_FLUSH();
                        
                        // Load camera settings
                        LOGI() << "[CAMERA " << cam_count << ".4] Loading camera settings...";
                        PROBE_FLUSH();
                        
                        try {
                            LOGD() << "[CAMERA " << cam_count << ".4.1] Dumping camera JSON: " << camJson.dump(2);
                            PROBE_FLUSH();
                            
                            cameras_.at(id).load(camJson);
                            
                            LOGI() << "[CAMERA " << cam_count << ".4] [OK] Camera settings loaded";
                            PROBE_FLUSH();
                            
                            num_cam++;
                        }
                        catch (const std::exception& e) {
                            LOGE() << "[CAMERA " << cam_count << ".4] [FAIL] ERROR loading camera " << id << " settings: " << e.what();
                            LOGE() << "[CAMERA " << cam_count << ".4] Exception type: " << typeid(e).name();
                            PROBE_FLUSH();
                            cameras_.erase(id);
                            continue;
                        }
                        
                        // Check mono camera
                        LOGI() << "[CAMERA " << cam_count << ".5] Checking for mono camera...";
                        PROBE_FLUSH();
                        
                        try {
                            auto monOpt = getCameraSettingByIdAndKey<std::string>(id, "MonoCameraAddress");
                            if (monOpt && !monOpt->empty()) {
                                num_cam++;
                                LOGI() << "[CAMERA " << cam_count << ".5] [OK] Has mono camera";
                                PROBE_FLUSH();
                            }
                        }
                        catch (const std::exception& e) {
                            LOGE() << "[CAMERA " << cam_count << ".5] [FAIL] ERROR checking mono: " << e.what();
                            PROBE_FLUSH();
                        }
                        
                        LOGI() << "[CAMERA " << cam_count << "] [OK] Camera processed successfully";
                        PROBE_FLUSH();
                    }
                    catch (const std::exception& e) {
                        LOGE() << "[CAMERA " << cam_count << "] [FAIL] ERROR: " << e.what();
                        LOGE() << "[CAMERA " << cam_count << "] Exception type: " << typeid(e).name();
                        PROBE_FLUSH();
                        continue;
                    }
                    catch (...) {
                        LOGE() << "[CAMERA " << cam_count << "] [FAIL] UNKNOWN ERROR";
                        PROBE_FLUSH();
                        continue;
                    }
                }
                
                LOGI() << "[STEP 4] [OK] All " << cam_count << " cameras processed";
                PROBE_FLUSH();
            }
        } else {
            LOGW() << "[STEP 4] No 'cameras_data' field";
            PROBE_FLUSH();
        }
        
        LOGI() << "=== [SETTINGS MANAGER] loadAll() COMPLETED SUCCESSFULLY ===";
        LOGI() << "Total cameras: " << cameras_.size();
        LOGI() << "Total count: " << num_cam;
        PROBE_FLUSH();
    }
    catch (const std::exception& e) {
        LOGE() << "!!! [CRITICAL] ERROR IN loadAll(): " << e.what();
        LOGE() << "!!! Exception type: " << typeid(e).name();
        PROBE_FLUSH();
        throw;
    }
    catch (...) {
        LOGE() << "!!! [CRITICAL] UNKNOWN ERROR IN loadAll()";
        PROBE_FLUSH();
        throw;
    }
}

template<typename T>
std::optional<T> SettingsManager::getLpr(const std::string& key) const {
    return lpr_.get<T>(key);
}

int SettingsManager::get_num_camera() {
    return num_cam;
}

std::vector<int> SettingsManager::getCameraIds() const {
    std::vector<int> ids;
    ids.reserve(cameras_.size());
    for (auto& kv : cameras_) ids.push_back(kv.first);
    return ids;
}

std::vector<cv::Point2f> SettingsManager::getCameraPoints(int cameraId) const {
    if (auto camOpt = getCameraSettings(cameraId))
        return camOpt->get().getPoints();
    return {};
}

std::vector<cv::Point> SettingsManager::getCameraPoints(int cameraId, int width, int height) const {
    std::string key = makePtsKey(cameraId, width, height);
    {
        std::lock_guard<std::mutex> lock(cameraCacheMutex_);
        auto it = cameraPointsCache_.find(key);
        if (it != cameraPointsCache_.end())
            return it->second;
    }
    auto normPts = getCameraPoints(cameraId);
    std::vector<cv::Point> pts_;
    pts_.reserve(normPts.size());
    for (const auto& p : normPts) {
        pts_.emplace_back(
            static_cast<int>(std::min((int)std::round(p.x * width), width)),
            static_cast<int>(std::min((int)std::round(p.y * height), height))
        );
    }

    std::vector<cv::Point> pts = sanitizePolygon(pts_);
    {
        std::lock_guard<std::mutex> lock(cameraCacheMutex_);
        if (pts.size() >= 3) {
            cameraPointsCache_[key] = pts;
        }
        else
            pts.clear();
    }

    return pts;
}

void SettingsManager::printPts(std::vector<cv::Point> pts, std::string name) const {
    std::cout << name << " (" << pts.size() << "):\n";
    for (size_t i = 0; i < pts.size(); ++i) {
        std::cout << "  [" << i << "] (" << pts[i].x << ", " << pts[i].y << ")\n";
    }
    std::cout.flush();
}

std::vector<cv::Point> SettingsManager::sanitizePolygon(const std::vector<cv::Point>& in) {
    std::vector<cv::Point> poly = in;
    if (poly.size() < 3) return {};

    if (!poly.empty() && samePt(poly.front(), poly.back()))
        poly.pop_back();

    poly.erase(std::unique(poly.begin(), poly.end(),
        [](const cv::Point& a, const cv::Point& b) { return samePt(a, b); }),
        poly.end());
    if (poly.size() < 3) return {};

    double signedArea = cv::contourArea(poly, /*oriented=*/true);
    if (signedArea < 0) std::reverse(poly.begin(), poly.end());

    return poly;
}

std::optional<std::reference_wrapper<const CameraSettings>>
SettingsManager::getCameraSettings(int cameraId) const {
    auto it = cameras_.find(cameraId);
    if (it != cameras_.end()) {
        // ---- PINPOINT DIAGNOSTIC: SettingsManager_.cpp's view of this object.
        // Compare with CameraSettings::get's line. &cam here should equal 'this'
        // there; sizeof/JSON_DIAGNOSTICS must match. Any difference == ABI bug. --
        /*LOGI() << "[getCameraSettings] (SettingsManager_.cpp view) id=" << cameraId
               << " &cam=" << static_cast<const void*>(&it->second)
               << " sizeof(CameraSettings)=" << sizeof(CameraSettings)
               << " sizeof(json)=" << sizeof(nlohmann::json)
               << " JSON_DIAGNOSTICS=" << (int)JSON_DIAGNOSTICS;*/
        PROBE_FLUSH();
        return std::cref(it->second);
    }
    return std::nullopt;
}

std::optional<cv::Rect2f> SettingsManager::aoiCropNorm(int cameraId, bool mono) const {
    // Mirrors BaslerCamera::computeAoiCrop: mono plate cam of a pair is ALWAYS cropped
    // (roi = ROI-polygon bbox, or a manual rect); the colour camera crops only when
    // rgb_crop_enable is on. Returns nullopt for "no crop / full frame".

    // Configuration mode: temporarily disable the crop so the camera reconnects at FULL sensor
    // and snapshots/crud show the whole scene (the operator draws ROI/crop on the full image).
    // The frontend sets this while the ROI/AOI editor is open and clears it on close.
    if (getCameraSettingByIdAndKey<int>(cameraId, "aoi_config_mode").value_or(0) != 0)
        return std::nullopt;

    bool want = false; std::string mode;
    if (mono) {
        want = true;
        mode = getCameraSettingByIdAndKey<std::string>(cameraId, "mono_crop_mode").value_or("roi");
    } else if (getCameraSettingByIdAndKey<int>(cameraId, "rgb_crop_enable").value_or(0) != 0) {
        want = true;
        mode = getCameraSettingByIdAndKey<std::string>(cameraId, "rgb_crop_mode").value_or("manual");
    }
    if (!want) return std::nullopt;

    double x = 0, y = 0, w = 0, h = 0;
    if (mode == "roi") {
        const auto pts = getCameraPoints(cameraId);          // normalized ROI polygon
        if (pts.size() >= 3) {
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            for (const auto& p : pts) {
                minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
            }
            x = minx; y = miny; w = maxx - minx; h = maxy - miny;
        }
    } else {   // "manual"
        const char* kx = mono ? "mono_crop_x" : "rgb_crop_x";
        const char* ky = mono ? "mono_crop_y" : "rgb_crop_y";
        const char* kw = mono ? "mono_crop_w" : "rgb_crop_w";
        const char* kh = mono ? "mono_crop_h" : "rgb_crop_h";
        x = getCameraSettingByIdAndKey<float>(cameraId, kx).value_or(0.0f);
        y = getCameraSettingByIdAndKey<float>(cameraId, ky).value_or(0.0f);
        w = getCameraSettingByIdAndKey<float>(cameraId, kw).value_or(0.0f);
        h = getCameraSettingByIdAndKey<float>(cameraId, kh).value_or(0.0f);
    }
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    if (w <= 0.0 || h <= 0.0) return std::nullopt;
    w = std::min(w, 1.0 - x);
    h = std::min(h, 1.0 - y);
    if (w <= 0.001 || h <= 0.001) return std::nullopt;

    // Safety margin: grow the crop around its centre by aoi_crop_margin percent so small camera
    // drift is tolerated (the plate region stays inside the transmitted frame instead of being
    // clipped away). Bigger margin = more drift tolerance, slightly less bandwidth saving.
    const int marginPct = getCameraSettingByIdAndKey<int>(cameraId, "aoi_crop_margin").value_or(0);
    if (marginPct > 0) {
        const double mp = std::clamp(marginPct, 0, 200) / 100.0;
        x = x - w * mp * 0.5;
        y = y - h * mp * 0.5;
        w = w * (1.0 + mp);
        h = h * (1.0 + mp);
        x = std::clamp(x, 0.0, 1.0);
        y = std::clamp(y, 0.0, 1.0);
        w = std::min(w, 1.0 - x);
        h = std::min(h, 1.0 - y);
    }
    return cv::Rect2f((float)x, (float)y, (float)w, (float)h);
}

std::optional<cv::Rect2f> SettingsManager::detectionCropNorm(int cameraId) const {
    // Only pylon cameras get a hardware AOI crop.
    if (getCameraSettingByIdAndKey<std::string>(cameraId, "type_of_link").value_or("") != "pylon")
        return std::nullopt;
    const std::string mono = getCameraSettingByIdAndKey<std::string>(cameraId, "MonoCameraAddress").value_or("");
    const bool hasMono = !mono.empty() && mono != "-1";
    // Detection runs on the mono of a pair; on the lone camera otherwise.
    return aoiCropNorm(cameraId, hasMono);
}

cv::Rect SettingsManager::getGeneralRoi(int cameraId, int width, int height) const {
    std::vector<cv::Point> pts=getCameraPoints(cameraId, width, height);
    std::string ptsKey = makePtsKey(cameraId, width, height);

    if(pts.size()==0) {
        auto camOpt = getCameraSettings(cameraId);
        if (!camOpt) return {};
        const auto& cam = camOpt->get();
        auto normLines = cam.getAllLineNormals();
        lineCache_.cacheLines(std::to_string(cameraId), normLines, width, height);
        for (auto& [idx, _] : normLines) {
            if (auto pixOpt = lineCache_.getLine(std::to_string(cameraId), idx)) {
                pts.insert(pts.end(), pixOpt->begin(), pixOpt->end());
            }
        }
    }
    if (ptsKey.find('_') != std::string::npos && pts.empty() == false) {
        cameraPointsCache_[ptsKey] = pts;
    }
    if (pts.empty()) return cv::Rect();
    auto [mask, rect] = roiCalc_.compute(std::to_string(cameraId), pts, cv::Size(width, height));
    return rect;
}

cv::Mat SettingsManager::cropAndMask(int cameraId, const cv::Mat& img) const {
    cv::Rect roi = getGeneralRoi(cameraId, img.cols, img.rows);
    if (roi.empty())
        return img;
    auto [mask, _] = roiCalc_.compute(std::to_string(cameraId), {}, img.size());

    cv::Mat masked;
    if (mask.size() != img.size()) {
        int width = std::min(img.cols, mask.cols);
        int height = std::min(img.rows, mask.rows);

        cv::Rect roi(0, 0, width, height);

        cv::Mat imgCropped = img(roi);
        cv::Mat maskCropped = mask(roi);

        imgCropped.copyTo(masked, maskCropped);
    }
    else {
        img.copyTo(masked, mask);
    }

    if (auto camOpt = getCameraSettings(cameraId)) {
        const auto& cam = camOpt->get();
        auto normLines = cam.getAllLineNormals();
        std::string camIdStr = std::to_string(cameraId);
        lineCache_.cacheLines(camIdStr, normLines, img.cols, img.rows);

        for (auto& [idx, _] : normLines) {
            if (auto pixOpt = lineCache_.getLine(camIdStr, idx)) {
                cv::polylines(masked, *pixOpt, false,
                    cv::Scalar(0, 255, 0), 2);
            }
        }
    }
   
    if (roi.area() > 0) {
        int x = std::max(roi.x, 0);
        int y = std::max(roi.y, 0);
        int width = std::min(roi.width, masked.cols - x-1);
        int height = std::min(roi.height, masked.rows - y-1);

        cv::Rect adjustedRoi(x, y, width, height);

        return masked(adjustedRoi);
    }
    else {
        return masked;
    }
}

int SettingsManager::getLinePointsCount(int cameraId) const {
    if (auto camOpt = getCameraSettings(cameraId))
        return static_cast<int>(camOpt->get().getAllLineNormals().size());
    return 0;
}

int SettingsManager::getLineContainingPoint(int cameraId, const cv::Point& pt, int width, int height) const {
    cv::Rect roi = getGeneralRoi(cameraId, width, height);
    if (roi.empty())
        return -1;

    int line_id = -1;
    if (auto camOpt = getCameraSettings(cameraId)) {
        const auto& cam = camOpt->get();
        for (auto& [idx, _] : cam.getAllLineNormals()) {
            line_id = 0;
            if (auto pixOpt = lineCache_.getLine(std::to_string(cameraId), idx)) {
                if (cv::pointPolygonTest(*pixOpt, pt + roi.tl(), false) >= 0)
                    return idx;
            }
        }
    }
    return line_id;
}

cv::Rect SettingsManager::getMaskROI(int cameraId, int width, int height) const {
    getGeneralRoi(cameraId, width, height);
    auto [mask, rect] = roiCalc_.compute(std::to_string(cameraId), {}, cv::Size(width, height));
    return rect;
}

template<typename T>
std::optional<T> SettingsManager::getCameraSettingByIdAndKey(int cameraId, const std::string& key) const {
    if (auto camOpt = getCameraSettings(cameraId))
        return camOpt->get().get<T>(key);
    return std::nullopt;
}

template std::optional<int>
SettingsManager::getLpr<int>(const std::string&) const;
template std::optional<float>
SettingsManager::getLpr<float>(const std::string&) const;
template std::optional<std::string>
SettingsManager::getLpr<std::string>(const std::string&) const;

template std::optional<int>
SettingsManager::getCameraSettingByIdAndKey<int>(int, const std::string&) const;
template std::optional<float>
SettingsManager::getCameraSettingByIdAndKey<float>(int, const std::string&) const;
template std::optional<std::string>
SettingsManager::getCameraSettingByIdAndKey<std::string>(int, const std::string&) const;

} // namespace lpr