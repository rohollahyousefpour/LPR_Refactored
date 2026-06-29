#include "lpr/config/AppConfig.h"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace lpr {
namespace {

// Trim leading/trailing spaces and tabs.
std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Split on a single delimiter (boost-free replacement for boost::split).
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim))
        out.push_back(item);
    return out;
}

using Dict = std::map<std::string, std::string>;

int getInt(const Dict& d, const std::string& key, int def = 0) {
    auto it = d.find(key);
    if (it != d.end()) { try { return std::stoi(it->second); } catch (...) {} }
    std::cout << "Missing/invalid int field: " << key << ", using default " << def << "\n";
    return def;
}
float getFloat(const Dict& d, const std::string& key, float def = 0.0f) {
    auto it = d.find(key);
    if (it != d.end()) { try { return std::stof(it->second); } catch (...) {} }
    std::cout << "Missing/invalid float field: " << key << ", using default " << def << "\n";
    return def;
}
std::string getStr(const Dict& d, const std::string& key, const std::string& def = "") {
    auto it = d.find(key);
    if (it != d.end()) return it->second;
    std::cout << "Missing string field: " << key << ", using default '" << def << "'\n";
    return def;
}
std::vector<int> getIntList(const Dict& d, const std::string& key) {
    auto it = d.find(key);
    if (it == d.end()) { std::cout << "Missing list field: " << key << "\n"; return {}; }
    std::vector<int> out;
    for (const auto& tok : split(it->second, ',')) {
        try { out.push_back(std::stoi(tok)); } catch (...) {}
    }
    return out;
}

} // namespace

AppConfig::AppConfig(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        is_load = false;
        return;
    }

    Dict d;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            std::cerr << "Invalid line in config file: " << line << "\n";
            continue;
        }
        d.insert({ trim(line.substr(0, pos)), trim(line.substr(pos + 1)) });
    }

    track_plates       = getInt(d, "track_plates");
    ViewPointX         = getIntList(d, "ViewPointX");
    ViewPointY         = getIntList(d, "ViewPointY");
    ViewPointWidth     = getIntList(d, "ViewPointWidth");
    ViewPointHeight    = getIntList(d, "ViewPointHeight");
    MaxDeviation       = static_cast<float>(getInt(d, "MaxDeviation"));
    MinDeviation       = static_cast<float>(getInt(d, "MinDeviation"));
    ObjectSize         = getInt(d, "ObjectSize");
    BufferSize         = getInt(d, "BufferSize");
    CameraDelayTime    = getInt(d, "CameraDelayTime");
    config_file        = getStr(d, "config_file");
    Iraq_use           = getInt(d, "Iraq_use");
    Afghan_use         = getInt(d, "Afghan_use");
    deep_plate_width_1 = getInt(d, "deep_plate_width_1");
    deep_plate_width_2 = getInt(d, "deep_plate_width_2");
    deep_plate_height  = getInt(d, "deep_plate_height");
    deep_width         = getInt(d, "deep_width");
    deep_height        = getInt(d, "deep_height");
    deep_detect_prob   = getFloat(d, "deep_detect_prob");
    combine_plates     = getInt(d, "combine_plates");
    max_IOU            = getFloat(d, "max_IOU");
    min_IOU            = getFloat(d, "min_IOU");
    nation_alpr        = getInt(d, "nation_alpr");
    ocr_file           = getStr(d, "ocr_file");
    car_file           = getStr(d, "car_file");
    plate_detection_file = getStr(d, "plate_detection_file");
    num_frame_process  = getInt(d, "num_frame_process");
    num_send_frame     = getInt(d, "num_send_frame");
    live_scale         = getInt(d, "live_scale");
    gates              = split(getStr(d, "gate"), ',');
    CameraAddress      = split(getStr(d, "CameraAddress"), ',');
    serial             = getStr(d, "serial");
    recive_plate_status = getInt(d, "recive_plate_status");
    relay_ip           = getStr(d, "relay_ip");
    relay_port         = getInt(d, "relay_port");
    debug              = getInt(d, "debug");
    video              = getInt(d, "video");
    type_of_link       = getStr(d, "type_of_link");
    show_live          = getInt(d, "show_live");
    ocr_prob           = getFloat(d, "ocr_prob");
    use_cpu            = getInt(d, "use_cpu");
    last_read_send     = getInt(d, "last_read_send");
    use_cuda           = getInt(d, "use_cuda");
    TCP_IP             = getInt(d, "TCP_IP");
    car_detection      = getInt(d, "car_detection");
    multi_language     = getInt(d, "multi_language");
    base_api           = getStr(d, "base_api");
    deep_car_width     = getInt(d, "deep_car_width");
    deep_car_height    = getInt(d, "deep_car_height");
    start_car_detect   = getFloat(d, "start_car_detect");
    db_adress          = getStr(d, "db_adress");
    base_path          = getStr(d, "base_path");
    ocr_file_afghan    = getStr(d, "ocr_file_afghan");

    // relay_key:  groups separated by ';', ints within a group separated by ','
    auto it = d.find("relay_key");
    if (it != d.end()) {
        for (const auto& group : split(it->second, ';')) {
            std::vector<int> keys;
            for (const auto& tok : split(group, ',')) {
                try { keys.push_back(std::stoi(tok)); }
                catch (...) { std::cout << "Invalid relay key: " << tok << "\n"; }
            }
            relay_keys.push_back(keys);
        }
    } else {
        std::cout << "relay_key not found.\n";
    }

    is_load = true;
}

} // namespace lpr
