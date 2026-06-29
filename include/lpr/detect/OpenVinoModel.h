#pragma once
// Intel OpenVINO backend (CPU / GPU / NPU). Compiled only when WITH_OPENVINO is set
// and find_package(OpenVINO) succeeds.
#ifdef LPR_WITH_OPENVINO
#include "lpr/detect/InferModel.h"
#include <openvino/openvino.hpp>

namespace lpr {

class OpenVinoModel : public InferModel {
public:
    bool                 load(const std::string& modelPath, const std::string& device = "CPU") override;
    void                 setInputFormat(InputFormat f) override { fmt_ = f; }
    std::vector<cv::Mat> infer(const cv::Mat& blob) override;
    Backend              backend() const override { return Backend::OpenVino; }

private:
    ov::Core         core_;
    ov::CompiledModel compiled_;
    ov::InferRequest  request_;
    bool              ready_ = false;
    InputFormat       fmt_ = InputFormat::NchwF32;
};

} // namespace lpr
#endif // LPR_WITH_OPENVINO
