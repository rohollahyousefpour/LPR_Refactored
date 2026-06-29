#include "lpr/config/LprSettings.h"
#include <cmath>
#include <iostream>

int main() {
    using json = nlohmann::json;
    int fails = 0;
    auto check = [&](bool c, const char* m){ if(!c){ std::cerr<<"  FAIL: "<<m<<"\n"; ++fails; } };

    json arr = json::array({
        { {"name","ocr_prob"},     {"value", 0.9} },
        { {"name","track_plates"}, {"value", 1} },
        { {"name","mode"},         {"value", "fast"} }
    });

    lpr::LprSettings s;
    s.load(arr);
    auto prob = s.get<double>("ocr_prob");
    auto trk  = s.get<int>("track_plates");
    auto mode = s.get<std::string>("mode");
    auto miss = s.get<int>("not_present");

    check(prob && std::abs(*prob - 0.9) < 1e-9, "double value");
    check(trk  && *trk == 1,        "int value");
    check(mode && *mode == "fast",  "string value");
    check(!miss,                    "missing key -> nullopt");

    lpr::LprSettings s2;
    s2.load(json::object());                 // not an array
    check(!s2.get<int>("x"), "non-array load is safe");

    if (fails == 0) { std::cout << "lpr_settings: ALL TESTS PASSED\n"; return 0; }
    return 1;
}
