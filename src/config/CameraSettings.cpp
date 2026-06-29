// =============== DIAGNOSTIC VERSION - ULTRA VERBOSE ===============
// Replace src/CameraSettings.cpp with this version

#include "lpr/config/JsonAbi.h"      // FIRST: pin json ABI macros before json.hpp is pulled in
#include "lpr/config/CameraSettings.h"
#include <iostream>
#include "lpr/Log.h"        // log via your Boost.Log logger

#ifndef PROBE_FLUSH
#define PROBE_FLUSH() boost::log::core::get()->flush()
#endif

#include <atomic>

namespace lpr {
// One-time ABI fingerprint for THIS translation unit. Compare with the other
// files' [ABI] lines; any difference in sizeof(json)/sizeof(CameraSettings)
// means an inconsistent JSON_DIAGNOSTICS -> the mutex crash.
static void CamAbiFingerprintOnce() {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (done.compare_exchange_strong(expected, true)) {
        LOGI() << "[ABI][CameraSettings.cpp]"
               << " JSON_DIAGNOSTICS=" << (int)JSON_DIAGNOSTICS
               << " sizeof(json)=" << sizeof(nlohmann::json)
               << " sizeof(CameraSettings)=" << sizeof(CameraSettings)
               << " sizeof(std::mutex)=" << sizeof(std::mutex);
        PROBE_FLUSH();
    }
}

CameraSettings::CameraSettings(int cameraId) : cameraId_(cameraId) {}

void CameraSettings::load(const json& cameraJson) {
    CamAbiFingerprintOnce();
    LOGI() << "  [CAMERA " << cameraId_ << " LOAD] Starting load..."
           << " this=" << static_cast<const void*>(this)
           << " &mutex_=" << static_cast<const void*>(&mutex_);
    PROBE_FLUSH();

    // *** DIAGNOSTIC ONLY: load()'s lock is temporarily disabled so the run
    // reaches CameraSettings::get() (the crash point). RESTORE THIS LINE once the
    // ABI mismatch is fixed -- it is required for thread safety. ***
    // std::lock_guard<std::mutex> lock(mutex_);   // <-- RESTORE AFTER FIX
    values_.clear();
    linesNorm_.clear();
    pointsNorm_.clear();

    LOGI() << "  [CAMERA " << cameraId_ << ".1] Cleared previous data";
    PROBE_FLUSH();

    // Parse settings
    LOGI() << "  [CAMERA " << cameraId_ << ".2] Parsing settings...";
    PROBE_FLUSH();
    
    if (cameraJson.contains("settings") && cameraJson["settings"].is_array()) {
        int setting_count = 0;
        for (const auto& s : cameraJson["settings"]) {
            setting_count++;
            try {
                if (s.contains("name") && s.contains("value")) {
                    std::string name = s["name"].get<std::string>();
                    values_[name] = s["value"];
                    LOGI() << "    [Setting " << setting_count << "] Loaded: " << name;
                    PROBE_FLUSH();
                }
            }
            catch (const std::exception& e) {
                LOGE() << "    [Setting " << setting_count << "] ERROR: " << e.what();
                PROBE_FLUSH();
                continue;
            }
        }
        LOGI() << "  [CAMERA " << cameraId_ << ".2] [OK] Loaded " << setting_count << " settings";
        PROBE_FLUSH();
    } else {
        LOGW() << "  [CAMERA " << cameraId_ << ".2] No settings array found";
        PROBE_FLUSH();
    }

    // Parse points
    LOGI() << "  [CAMERA " << cameraId_ << ".3] Parsing points...";
    PROBE_FLUSH();
    
    if (cameraJson.contains("points") && cameraJson["points"].is_array()) {
        int point_count = 0;
        for (const auto& pt : cameraJson["points"]) {
            point_count++;
            try {
                if (pt.is_array() && pt.size() == 2) {
                    float x = pt[0].is_number() ? pt[0].get<float>() : 0.0f;
                    float y = pt[1].is_number() ? pt[1].get<float>() : 0.0f;
                    pointsNorm_.emplace_back(x, y);
                    LOGI() << "    [Point " << point_count << "] Loaded: (" << x << ", " << y << ")";
                    PROBE_FLUSH();
                }
            }
            catch (const std::exception& e) {
                LOGE() << "    [Point " << point_count << "] ERROR: " << e.what();
                PROBE_FLUSH();
                continue;
            }
        }
        LOGI() << "  [CAMERA " << cameraId_ << ".3] [OK] Loaded " << point_count << " points";
        PROBE_FLUSH();
    } else {
        LOGW() << "  [CAMERA " << cameraId_ << ".3] No points array found";
        PROBE_FLUSH();
    }

    // Parse lines (MOST LIKELY CRASH LOCATION)
    LOGI() << "  [CAMERA " << cameraId_ << ".4] Parsing lines...";
    PROBE_FLUSH();
    
    if (cameraJson.contains("lines") && cameraJson["lines"].is_object()) {
        LOGI() << "  [CAMERA " << cameraId_ << ".4] Lines object found";
        PROBE_FLUSH();
        
        int line_count = 0;
        for (auto& [lineStr, coords] : cameraJson["lines"].items()) {
            line_count++;
            LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << "] Processing line key: '" << lineStr << "'";
            PROBE_FLUSH();
            
            try {
                // CRITICAL: Convert line string to number
                LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".1] Converting line number...";
                PROBE_FLUSH();
                
                int lineNum = -1;
                try {
                    lineNum = std::stoi(lineStr);
                    LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".1] [OK] Got line number: " << lineNum;
                    PROBE_FLUSH();
                }
                catch (const std::invalid_argument& e) {
                    LOGE() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".1] [FAIL] INVALID LINE NUMBER: '" << lineStr << "' - " << e.what();
                    PROBE_FLUSH();
                    continue;
                }
                catch (const std::out_of_range& e) {
                    LOGE() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".1] [FAIL] LINE NUMBER OUT OF RANGE: '" << lineStr << "' - " << e.what();
                    PROBE_FLUSH();
                    continue;
                }

                LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".2] Checking coords array...";
                PROBE_FLUSH();
                
                if (!coords.is_array()) {
                    LOGW() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".2] Coords is not array, skipping";
                    PROBE_FLUSH();
                    continue;
                }
                
                LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".2] [OK] Coords is array with " << coords.size() << " points";
                PROBE_FLUSH();
                
                std::vector<cv::Point2f> poly;
                int coord_count = 0;
                for (auto& pt : coords) {
                    coord_count++;
                    try {
                        LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".POINT" << coord_count << "] Parsing point...";
                        PROBE_FLUSH();
                        
                        if (pt.is_array() && pt.size() == 2) {
                            float x = pt[0].is_number() ? pt[0].get<float>() : 0.0f;
                            float y = pt[1].is_number() ? pt[1].get<float>() : 0.0f;
                            poly.emplace_back(x, y);
                            LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".POINT" << coord_count << "] [OK] Got: (" << x << ", " << y << ")";
                            PROBE_FLUSH();
                        } else {
                            LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".POINT" << coord_count << "] Invalid point format";
                            PROBE_FLUSH();
                        }
                    }
                    catch (const std::exception& e) {
                        LOGE() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".POINT" << coord_count << "] ERROR: " << e.what();
                        PROBE_FLUSH();
                        continue;
                    }
                }
                
                LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << ".3] Processed " << poly.size() << " points for line " << lineNum;
                PROBE_FLUSH();
                
                if (!poly.empty()) {
                    linesNorm_[lineNum] = std::move(poly);
                    LOGI() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << "] [OK] Line stored";
                    PROBE_FLUSH();
                }
            }
            catch (const std::exception& e) {
                LOGE() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << "] [FAIL] ERROR: " << e.what();
                LOGE() << "  [CAMERA " << cameraId_ << ".4.LINE" << line_count << "] Exception type: " << typeid(e).name();
                PROBE_FLUSH();
                continue;
            }
        }
        
        LOGI() << "  [CAMERA " << cameraId_ << ".4] [OK] Processed " << line_count << " lines total";
        PROBE_FLUSH();
    } else {
        LOGW() << "  [CAMERA " << cameraId_ << ".4] No lines object found";
        PROBE_FLUSH();
    }
    
    LOGI() << "  [CAMERA " << cameraId_ << "] [OK] LOAD COMPLETED SUCCESSFULLY";
    LOGI() << "    - Settings: " << values_.size();
    LOGI() << "    - Points: " << pointsNorm_.size();
    LOGI() << "    - Lines: " << linesNorm_.size();
    PROBE_FLUSH();
}

std::optional<CameraSettings::json> CameraSettings::getAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return values_;
}

template <typename T>
std::optional<T> CameraSettings::get(const std::string& key) const {
    // ---- PINPOINT DIAGNOSTIC: this prints CameraSettings.cpp's view of the
    // object, BEFORE locking. Compare with getCameraSettings()'s line (the
    // SettingsManager_.cpp view of the SAME object). If sizeof(CameraSettings),
    // sizeof(json), or JSON_DIAGNOSTICS differ between the two lines -> ABI
    // mismatch, and &mutex_ is at the wrong offset -> this lock crashes. ----
    /*LOGI() << "[CameraSettings::get] (CameraSettings.cpp view)"
           << " key=" << key
           << " this=" << static_cast<const void*>(this)
           << " &mutex_=" << static_cast<const void*>(&mutex_)
           << " sizeof(CameraSettings)=" << sizeof(CameraSettings)
           << " sizeof(json)=" << sizeof(nlohmann::json)
           << " JSON_DIAGNOSTICS=" << (int)JSON_DIAGNOSTICS;*/
    PROBE_FLUSH();

    std::lock_guard<std::mutex> lock(mutex_);
    //LOGI() << "[CameraSettings::get] lock acquired (offset OK)"; PROBE_FLUSH();

    if (values_.contains(key)) {
        try { return values_[key].get<T>(); }
        catch (...) {}
    }
    return std::nullopt;
}

const std::unordered_map<int, std::vector<cv::Point2f>>&
CameraSettings::getAllLineNormals() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return linesNorm_;
}

std::vector<cv::Point2f> CameraSettings::getPoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pointsNorm_;
}

// Explicit instantiations
template std::optional<int> CameraSettings::get<int>(const std::string&) const;
template std::optional<float> CameraSettings::get<float>(const std::string&) const;
template std::optional<std::string> CameraSettings::get<std::string>(const std::string&) const;

} // namespace lpr