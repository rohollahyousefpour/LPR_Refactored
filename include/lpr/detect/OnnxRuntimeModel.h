#pragma once
// ONNX Runtime backend. One .onnx, many execution providers (CPU, XNNPACK for fast
// ARM CPU, CUDA, TensorRT, OpenVINO, NNAPI, CoreML...). Usually faster than OpenCV-DNN
// on CPU and the best portable choice. Compiled when WITH_ONNXRUNTIME is set.
#ifdef LPR_WITH_ONNXRUNTIME
#include "lpr/detect/InferModel.h"
#include <onnxruntime_cxx_api.h>
#include <memory>
#include <string>
#include <vector>

namespace lpr {

class OnnxRuntimeModel : public InferModel {
public:
    OnnxRuntimeModel();
    bool                 load(const std::string& modelPath, const std::string& device = "CPU") override;
    std::vector<cv::Mat> infer(const cv::Mat& blobNCHW) override;
    Backend              backend() const override { return Backend::OnnxRuntime; }

private:
    Ort::Env                     env_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string>     inNames_, outNames_;
    std::vector<const char*>     inNamesC_, outNamesC_;
    bool                         ready_ = false;
};

} // namespace lpr
#endif // LPR_WITH_ONNXRUNTIME
