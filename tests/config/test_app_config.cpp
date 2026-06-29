#include "lpr/config/AppConfig.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>

int main() {
    int fails = 0;
    auto check = [&](bool c, const char* m){ if(!c){ std::cerr<<"  FAIL: "<<m<<"\n"; ++fails; } };

    const char* path = "test_sample.conf";
    { std::ofstream f(path);
      f << "# a comment line\n"
        << "track_plates = 1\n"
        << "ocr_prob = 0.85\n"
        << "type_of_link = rtsp\n"
        << "gate = 1,2,3\n"
        << "ViewPointX = 10,20,30\n"
        << "relay_key = 1,2;3,4\n"; }

    lpr::AppConfig c(path);
    check(c.isLoaded(), "config loaded");
    check(c.track_plates == 1, "track_plates parsed");
    check(std::abs(c.ocr_prob - 0.85f) < 1e-6f, "ocr_prob parsed");
    check(c.type_of_link == "rtsp", "type_of_link parsed");
    check(c.gates.size() == 3, "gate list size");
    check(c.ViewPointX.size() == 3 && c.ViewPointX[2] == 30, "int-list parsed");
    check(c.relay_keys.size() == 2 && c.relay_keys[1].size() == 2 && c.relay_keys[1][1] == 4,
          "relay_key groups parsed");

    lpr::AppConfig missing("nope_does_not_exist.conf");
    check(!missing.isLoaded(), "missing file -> not loaded");

    std::remove(path);
    if (fails == 0) { std::cout << "app_config: ALL TESTS PASSED\n"; return 0; }
    return 1;
}
