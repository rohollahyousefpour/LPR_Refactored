#include "lpr/detect/RoiCropRecognizer.h"
#include "../lpr_check.hpp"
#include <iostream>

using namespace lpr;

// Records the size of the frame it was given; returns a plate at (5,5) in that frame.
class SizeProbe : public IPlateRecognizer {
public:
    cv::Size seen;
    std::vector<PlateResult> recognize(const cv::Mat& f, const std::string& g, long ts) override {
        seen = f.size();
        PlateResult p; p.text = "X"; p.gate = g; p.timestamp = ts;
        p.box = cv::RotatedRect(cv::Point2f(5, 5), cv::Size2f(10, 6), 0);
        p.vehicleBox = cv::Rect(2, 3, 8, 8);
        return { p };
    }
};

int main() {
    SizeProbe probe;
    RoiCropRecognizer roi(probe,
        [](const std::string&, int, int) { return cv::Rect(50, 40, 100, 80); });

    cv::Mat frame = cv::Mat::zeros(480, 640, CV_8UC3);
    auto out = roi.recognize(frame, "1", 7);

    LPR_CHECK(probe.seen == cv::Size(100, 80));        // inner saw the CROP, not the full frame
    LPR_CHECK(out.size() == 1);
    if (!out.empty()) {
        LPR_CHECK(out[0].box.center.x == 55 && out[0].box.center.y == 45);   // offset back
        LPR_CHECK(out[0].vehicleBox == cv::Rect(52, 43, 8, 8));              // rect shifted too
    }

    // No ROI provider given -> whole frame, no offset.
    RoiCropRecognizer whole(probe, RoiCropRecognizer::RoiProvider{});
    auto out2 = whole.recognize(frame, "1", 7);
    LPR_CHECK(probe.seen == cv::Size(640, 480));
    LPR_CHECK(!out2.empty() && out2[0].box.center.x == 5);

    if (LPR_TEST_RESULT() == 0) std::cout << "roi_crop: OK\n";
    return LPR_TEST_RESULT();
}
