#pragma once
// EastTextDetector - plate/text detector ported from the original EastDetector, but
// running through the backend-agnostic InferModel (OpenVINO/TensorRT/ONNX/DNN).
// Decodes an EAST score map + RBOX geometry into rotated boxes. The decode math is a
// free function (decodeEast) so it can be unit-tested without a model.
#include "lpr/detect/InferModel.h"
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace lpr {

struct EastConfig {
    int   detectWidth  = 640;          // forced to a multiple of 32 at load
    int   detectHeight = 320;
    int   stride       = 4;
    float scoreThresh  = 0.65f;
    float nmsThresh    = 0.40f;
    float heightScale  = 480.f;        // deep_plate_height
    float widthScale1  = 640.f;        // deep_plate_width_1
    float widthScale2  = 640.f;        // deep_plate_width_2
    float normScale    = 1.0f / 127.5f;// (pixel * normScale + normMean)
    float normMean     = -1.0f;
    bool  nhwc         = true;         // original OpenVINO model is NHWC
    bool  debugLog     = false;        // log score-map size + box count per frame
};

// Decode EAST score [Hf*Wf] + RBOX geometry [Hf*Wf*5] (NHWC, row-major) into rotated
// boxes (in detect-resized coordinates), with NMS applied. Pointers must be valid.
std::vector<cv::RotatedRect> decodeEast(const float* scores, const float* geometry,
                                        int Hf, int Wf, const EastConfig& cfg);

class EastTextDetector {
public:
    bool load(const std::string& modelPath, Backend backend,
              const std::string& device, const EastConfig& cfg = {});

    // Rotated boxes in ORIGINAL image coordinates.
    std::vector<cv::RotatedRect> detect(const cv::Mat& imageBgr);

    // Rotate + crop a box out of the frame (ported from the original).
    static void fourPointsTransform(const cv::Mat& frame, const cv::RotatedRect& box, cv::Mat& result);

    const EastConfig& config() const { return cfg_; }
    EastConfig&       config()       { return cfg_; }

private:
    std::unique_ptr<InferModel> model_;
    EastConfig                  cfg_;
    bool                        ready_ = false;
};

} // namespace lpr
