#pragma once
// Hailo NPU backend (Raspberry Pi AI HAT+ / Hailo-8, Hailo-8L). Uses HailoRT and a
// .hef model compiled from ONNX with the Hailo Dataflow Compiler. Proprietary SDK,
// so this is a skeleton compiled only when WITH_HAILO is set.
#ifdef LPR_WITH_HAILO
#include "lpr/detect/InferModel.h"
#include <hailo/hailort.hpp>
#include <memory>

namespace lpr {

class HailoModel : public InferModel {
public:
    bool                 load(const std::string& hefPath, const std::string& device = "NPU") override;
    std::vector<cv::Mat> infer(const cv::Mat& blobNCHW) override;
    Backend              backend() const override { return Backend::Hailo; }

private:
    std::unique_ptr<hailort::VDevice> vdevice_;
    std::shared_ptr<hailort::ConfiguredNetworkGroup> networkGroup_;
    bool ready_ = false;
};

} // namespace lpr
#endif // LPR_WITH_HAILO
