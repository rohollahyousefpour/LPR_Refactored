#pragma once
// VehicleDetector - the clean, backend-agnostic port of the original vehicle_detection
// class (a YOLOv5-style anchor detector). Like EastTextDetector/PlateOcr it runs through
// the InferModel strategy, so the SAME detector works on OpenVINO / TensorRT / ONNX /
// OpenCV-DNN / Hailo. The decode math (anchor grid, sigmoid, (2x)^2 box scaling, NMS) is
// preserved from the original and exposed as a free function decodeYolo() for testing.
//
// Behaviour note (flagged change): the original only decoded ONE output head
// (`for (ou = 2; ou < 3)`), which limits it to the coarsest grid / largest anchors.
// This port decodes ALL heads by default (proper YOLOv5). To reproduce the original
// exactly, set cfg.headIndices = {2}.
#include "lpr/detect/InferModel.h"

#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace lpr {

struct VehicleDetection {
    cv::Rect box;          // pixel box in the source frame
    float    score = 0.f;
    int      classId = 0;
};

struct VehicleConfig {
    int   width  = 640;    // network input W (rounded down to a multiple of 32 on load)
    int   height = 480;    // network input H
    int   numClasses = 5;
    float scoreThreshold = 0.45f;
    float nmsThreshold   = 0.50f;

    // Preprocessing (original: BGR->RGB, raw u8 values as float, NHWC).
    bool  swapRB = true;
    float scale  = 1.0f;   // pixel * scale + mean
    float mean   = 0.0f;
    bool  nhwc   = true;   // input + output layout is NHWC

    // YOLOv5 anchors (9 pairs) and per-head masks, matching the original model.
    std::vector<std::vector<int>> anchors = {
        {10,16},{22,24},{19,42},{43,46},{31,82},{83,92},{57,160},{129,242},{351,576}
    };
    std::vector<std::vector<int>> anchorMasks = { {0,1,2},{3,4,5},{6,7,8} };
    std::vector<int> headIndices;   // empty => decode every output head
};

// One decoded output head: raw NHWC float data [gridH x gridW x (numAnchors*(5+numClasses))].
struct YoloHead {
    const float* data = nullptr;
    int gridH = 0;
    int gridW = 0;
    std::vector<int> anchorMask;    // which anchor rows this head uses
};

// Pure decode (sigmoid + anchor grid + box scaling + NMS), no inference. scaleX/scaleY
// map normalized network coords back to source pixels (= sourceSize / networkInputSize).
// Testable in isolation, like decodeEast().
std::vector<VehicleDetection> decodeYolo(const std::vector<YoloHead>& heads,
                                         const VehicleConfig& cfg,
                                         float scaleX, float scaleY);

// Interface so the pipeline can be tested with a fake detector.
class IVehicleDetector {
public:
    virtual ~IVehicleDetector() = default;
    virtual std::vector<VehicleDetection> detect(const cv::Mat& imageBgr) = 0;
};

class VehicleDetector : public IVehicleDetector {
public:
    bool load(const std::string& modelPath, Backend backend = Backend::Auto,
              const std::string& device = "CPU", const VehicleConfig& cfg = {});
    bool ready() const { return ready_; }
    const VehicleConfig& config() const { return cfg_; }

    std::vector<VehicleDetection> detect(const cv::Mat& imageBgr) override;

private:
    VehicleConfig                cfg_;
    std::unique_ptr<InferModel>  model_;
    bool                         ready_ = false;
};

} // namespace lpr
