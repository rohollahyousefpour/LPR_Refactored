#pragma once
// Portable CPU/GPU backend via OpenCV's dnn module. Works everywhere OpenCV does,
// including ARM / Raspberry Pi (CPU target). Loads ONNX / Caffe / TF models.
#include "lpr/detect/InferModel.h"
#include <opencv2/dnn.hpp>

namespace lpr {

class OpenCvDnnModel : public InferModel {
public:
    bool                 load(const std::string& modelPath, const std::string& device = "CPU") override;
    std::vector<cv::Mat> infer(const cv::Mat& blobNCHW) override;
    Backend              backend() const override { return Backend::OpenCvDnn; }

private:
    cv::dnn::Net             net_;
    std::vector<std::string> outNames_;
    bool                     ready_ = false;
};

} // namespace lpr
