#pragma once
// InferModel - backend-agnostic inference interface. One implementation per engine:
//   OpenVinoModel   (Intel CPU/GPU/NPU)          - WITH_OPENVINO
//   TensorRtModel   (NVIDIA GPU / Jetson, ARM)   - WITH_TENSORRT
//   OnnxRuntimeModel(cross-platform; XNNPACK on ARM, CUDA/TRT/OpenVINO EPs) - WITH_ONNXRUNTIME
//   HailoModel      (Raspberry Pi AI HAT+ NPU, Hailo-8/8L)   - WITH_HAILO
//   OpenCvDnnModel  (no-dependency CPU fallback, incl. ARM)  - always available
// The pre/post-processing (CTC decode, EAST geometry) is shared and lives outside
// the backend, so the same pipeline runs on any engine / platform.
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

namespace lpr {

enum class Backend { Auto, OpenVino, TensorRt, OnnxRuntime, Hailo, OpenCvDnn };
const char* toString(Backend b);
Backend     parseBackend(const std::string& s);

// Input feeding format. NchwF32 = cv::dnn::blobFromImage (default). NhwcU8 = raw HWC uint8
// image fed directly with the model normalizing internally (matches the original Alpr_vino /
// Ocr_Model which apply OpenVINO PPP: set_element_type(u8).set_layout("NHWC")).
enum class InputFormat { NchwF32, NhwcU8 };

class InferModel {
public:
    virtual ~InferModel() = default;

    // Load a model file (.xml/.onnx/.engine depending on backend). device is a hint
    // such as "CPU", "GPU", "CUDA" (interpreted per backend).
    virtual bool load(const std::string& modelPath, const std::string& device = "CPU") = 0;

    // Configure how inputs are fed. Must be called BEFORE load(). Default no-op (NchwF32).
    virtual void setInputFormat(InputFormat) {}

    // Run inference on a single preprocessed blob (NCHW, CV_32F, e.g. cv::dnn::blobFromImage),
    // or a raw HWC CV_8U image when the model was configured with InputFormat::NhwcU8.
    // Returns one n-dim CV_32F cv::Mat per network output.
    virtual std::vector<cv::Mat> infer(const cv::Mat& blobNCHW) = 0;

    virtual Backend backend() const = 0;
};

// Factory. Backend::Auto prefers TensorRT -> OpenVINO -> OpenCV-DNN among those compiled in.
// Throws std::runtime_error if a specific backend was requested but not compiled in.
std::unique_ptr<InferModel> makeInferModel(Backend backend = Backend::Auto);

} // namespace lpr
