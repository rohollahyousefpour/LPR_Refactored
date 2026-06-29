#pragma once
// AppConfig (was "Config") - the application's local key=value config file.
// Cleaned: namespaced, boost-free (uses an internal STL splitter), constructor
// takes a clear `configPath`, default copy semantics. The config-FILE keys are
// unchanged, so existing .conf files keep working.
#include <string>
#include <vector>

namespace lpr {

class AppConfig {
public:
    // Parses the given key=value file. On failure, isLoaded() returns false.
    explicit AppConfig(const std::string& configPath);
    AppConfig(const AppConfig&) = default;
    AppConfig& operator=(const AppConfig&) = default;
    ~AppConfig() = default;

    bool isLoaded() const { return is_load; }

    // ---- view / live ----
    int live_scale = 0;
    std::vector<int> ViewPointX, ViewPointY, ViewPointWidth, ViewPointHeight;
    int show_live = 0;

    // ---- nationality / detection model params ----
    int Iraq_use = 0, Afghan_use = 0;
    int deep_plate_width_1 = 0, deep_plate_width_2 = 0, deep_plate_height = 0;
    int deep_width = 0, deep_height = 0;
    float deep_detect_prob = 0.0f;
    int combine_plates = 0;
    float max_IOU = 0.0f, min_IOU = 0.0f;
    int nation_alpr = 0;

    // ---- tracking ----
    float MaxDeviation = 0.0f, MinDeviation = 0.0f;
    int ObjectSize = 0;
    int track_plates = 0;
    int BufferSize = 0;

    // ---- capture ----
    int CameraDelayTime = 0;
    int num_send_frame = 0, num_frame_process = 0;
    std::vector<std::string> CameraAddress;
    std::string type_of_link;
    std::vector<std::string> gates;

    // ---- model files ----
    std::string config_file;
    std::string ocr_file, ocr_file_afghan;
    std::string plate_detection_file;
    std::string car_file;
    float ocr_prob = 0.0f;

    // ---- system / serial ----
    std::string serial;
    int recive_plate_status = 0;
    std::string relay_ip;
    int relay_port = 0;
    int debug = 0;
    int video = 0;
    int last_read_send = 0;
    int use_cpu = 0, use_cuda = 0, TCP_IP = 0;
    int car_detection = 0, multi_language = 0;
    std::string base_api;

    // ---- car detection model ----
    int deep_car_width = 0, deep_car_height = 0;
    float start_car_detect = 0.0f;
    int change_day_night = 0;
    int deep_width_car_plate = 0, deep_height_car_plate = 0;
    int deep_width_no_car_plate = 0, deep_height_no_car_plate = 0;

    // ---- paths / db ----
    std::string db_adress;
    std::string base_path;

    // ---- relay ----
    std::vector<std::vector<int>> relay_keys;

private:
    bool is_load = false;
};

} // namespace lpr
