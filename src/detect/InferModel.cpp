#include "lpr/detect/InferModel.h"
#include "lpr/detect/OpenCvDnnModel.h"
#ifdef LPR_WITH_OPENVINO
#include "lpr/detect/OpenVinoModel.h"
#endif
#ifdef LPR_WITH_TENSORRT
#include "lpr/detect/TensorRtModel.h"
#endif
#ifdef LPR_WITH_ONNXRUNTIME
#include "lpr/detect/OnnxRuntimeModel.h"
#endif
#ifdef LPR_WITH_HAILO
#include "lpr/detect/HailoModel.h"
#endif

#include <stdexcept>

namespace lpr {

const char* toString(Backend b) {
    switch (b) {
        case Backend::Auto:        return "Auto";
        case Backend::OpenVino:    return "OpenVINO";
        case Backend::TensorRt:    return "TensorRT";
        case Backend::OnnxRuntime: return "ONNX-Runtime";
        case Backend::Hailo:       return "Hailo-NPU";
        case Backend::OpenCvDnn:   return "OpenCV-DNN";
    }
    return "Unknown";
}

Backend parseBackend(const std::string& s) {
    if (s == "openvino" || s == "OpenVINO" || s == "vino")     return Backend::OpenVino;
    if (s == "tensorrt" || s == "TensorRT" || s == "trt")      return Backend::TensorRt;
    if (s == "onnx"     || s == "onnxruntime" || s == "ort")   return Backend::OnnxRuntime;
    if (s == "hailo"    || s == "npu")                         return Backend::Hailo;
    if (s == "dnn"      || s == "opencv"   || s == "cpu")      return Backend::OpenCvDnn;
    return Backend::Auto;
}

static std::unique_ptr<InferModel> makeOpenVino() {
#ifdef LPR_WITH_OPENVINO
    return std::make_unique<OpenVinoModel>();
#else
    throw std::runtime_error("OpenVINO backend not compiled in (-DWITH_OPENVINO=ON)");
#endif
}
static std::unique_ptr<InferModel> makeTensorRt() {
#ifdef LPR_WITH_TENSORRT
    return std::make_unique<TensorRtModel>();
#else
    throw std::runtime_error("TensorRT backend not compiled in (-DWITH_TENSORRT=ON)");
#endif
}
static std::unique_ptr<InferModel> makeOnnx() {
#ifdef LPR_WITH_ONNXRUNTIME
    return std::make_unique<OnnxRuntimeModel>();
#else
    throw std::runtime_error("ONNX Runtime backend not compiled in (-DWITH_ONNXRUNTIME=ON)");
#endif
}
static std::unique_ptr<InferModel> makeHailo() {
#ifdef LPR_WITH_HAILO
    return std::make_unique<HailoModel>();
#else
    throw std::runtime_error("Hailo backend not compiled in (-DWITH_HAILO=ON)");
#endif
}

std::unique_ptr<InferModel> makeInferModel(Backend backend) {
    switch (backend) {
        case Backend::OpenVino:    return makeOpenVino();
        case Backend::TensorRt:    return makeTensorRt();
        case Backend::OnnxRuntime: return makeOnnx();
        case Backend::Hailo:       return makeHailo();
        case Backend::OpenCvDnn:   return std::make_unique<OpenCvDnnModel>();
        case Backend::Auto:
        default:
            // Hardware-native first, then ONNX Runtime, then the no-dependency fallback.
#ifdef LPR_WITH_TENSORRT
            return std::make_unique<TensorRtModel>();
#elif defined(LPR_WITH_OPENVINO)
            return std::make_unique<OpenVinoModel>();
#elif defined(LPR_WITH_HAILO)
            return std::make_unique<HailoModel>();
#elif defined(LPR_WITH_ONNXRUNTIME)
            return std::make_unique<OnnxRuntimeModel>();
#else
            return std::make_unique<OpenCvDnnModel>();
#endif
    }
}

} // namespace lpr
