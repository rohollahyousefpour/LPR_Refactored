#include "lpr/track/PlateTrackingRecognizer.h"
#include "../lpr_check.hpp"
#include <iostream>

using namespace lpr;

// Emits one plate whose box drifts a few px per frame (same physical plate).
class DriftingPlate : public IPlateRecognizer {
public:
    int f = 0;
    std::vector<PlateResult> recognize(const cv::Mat&, const std::string& g, long ts) override {
        PlateResult p; p.text = "12A34567"; p.confidence = 0.9f; p.gate = g; p.timestamp = ts;
        p.box = cv::RotatedRect(cv::Point2f(100.f + 2 * f, 100.f + 2 * f), cv::Size2f(40, 16), 0);
        ++f;
        return { p };
    }
};

int main() {
    DriftingPlate inner;
    PlateTrackingRecognizer rec(inner);

    cv::Mat frame = cv::Mat::zeros(480, 640, CV_8UC3);
    int lastId = -1, stable = 0;
    for (int i = 0; i < 6; ++i) {
        auto out = rec.recognize(frame, "1", i);
        LPR_CHECK(out.size() == 1);
        if (!out.empty()) {
            if (i >= 3) { if (out[0].trackId == lastId && lastId > 0) ++stable; }
            lastId = out[0].trackId;
        }
    }
    std::cout << "plate trackId=" << lastId << " stableFrames=" << stable << "\n";
    LPR_CHECK(lastId > 0);          // a real id was assigned to the tracked plate
    LPR_CHECK(stable >= 2);         // and it stayed stable across the settled frames

    if (LPR_TEST_RESULT() == 0) std::cout << "plate_tracking: OK\n";
    return LPR_TEST_RESULT();
}
