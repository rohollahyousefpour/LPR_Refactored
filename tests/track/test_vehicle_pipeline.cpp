#include "lpr/track/VehicleAwarePlateRecognizer.h"

#include <cassert>
#include <iostream>

using namespace lpr;

// Fake vehicle detector: always reports one vehicle box in the frame.
class FakeVehicleDetector : public IVehicleDetector {
public:
    cv::Rect box{50, 40, 120, 90};
    std::vector<VehicleDetection> detect(const cv::Mat&) override {
        return { VehicleDetection{ box, 0.9f, 1 } };
    }
};

// Fake plate stage: returns one plate whose box center is (10,5) IN THE CROP.
// We verify the recognizer offsets it back into full-frame coordinates.
class FakePlateStage : public IPlateRecognizer {
public:
    cv::Size lastCropSize;
    std::vector<PlateResult> recognize(const cv::Mat& crop, const std::string& gate, long ts) override {
        lastCropSize = crop.size();
        PlateResult p;
        p.text = "ABC123";
        p.confidence = 0.95f;
        p.box = cv::RotatedRect(cv::Point2f(10.f, 5.f), cv::Size2f(30.f, 12.f), 0.f);
        p.gate = gate; p.timestamp = ts;
        return { p };
    }
};

int main() {
    FakeVehicleDetector det;
    FakePlateStage     plate;
    VehicleAwarePlateRecognizer rec(det, plate);

    cv::Mat frame = cv::Mat::zeros(480, 640, CV_8UC3);
    auto plates = rec.recognize(frame, "gateA", 1234);

    std::cout << "plates=" << plates.size()
              << " cropSize=" << plate.lastCropSize.width << "x" << plate.lastCropSize.height << "\n";

    assert(plates.size() == 1);
    const PlateResult& p = plates[0];

    // The plate stage must have been run on the vehicle CROP (120x90), not the full frame.
    assert(plate.lastCropSize == cv::Size(120, 90));

    // Box center must be offset by the vehicle box origin (50,40): 10+50, 5+40.
    std::cout << "plate center=(" << p.box.center.x << "," << p.box.center.y << ")"
              << " trackId=" << p.trackId << " gate=" << p.gate << "\n";
    assert(p.box.center.x == 60.f && p.box.center.y == 45.f);

    // Track id was attached (tracker assigns a positive id for a confident detection).
    assert(p.trackId > 0);
    assert(p.vehicleBox == det.box);
    assert(p.gate == "gateA" && p.timestamp == 1234);
    assert(p.text == "ABC123");

    std::cout << "vehicle_pipeline: OK\n";
    return 0;
}
