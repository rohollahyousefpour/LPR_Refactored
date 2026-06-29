#ifdef LPR_WITH_ONNXRUNTIME
#include "lpr/detect/OnnxRuntimeModel.h"
#include "lpr/Log.h"

#include <cstring>
#include <functional>

namespace lpr {

OnnxRuntimeModel::OnnxRuntimeModel()
    : env_(ORT_LOGGING_LEVEL_WARNING, "lpr") {}

bool OnnxRuntimeModel::load(const std::string& modelPath, const std::string& device) {
    try {
        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Try the requested execution provider; fall back to CPU if it isn't built in.
        auto tryEP = [&](const char* name, std::function<void()> append) {
            try { append(); LOGI() << "OnnxRuntimeModel: using " << name << " EP"; }
            catch (const std::exception& e) { LOGW() << "OnnxRuntimeModel: " << name << " EP unavailable (" << e.what() << "), using CPU"; }
        };
        if (device == "CUDA")          tryEP("CUDA",     [&]{ OrtCUDAProviderOptions o{};     opts.AppendExecutionProvider_CUDA(o); });
        else if (device == "TENSORRT") tryEP("TensorRT", [&]{ OrtTensorRTProviderOptions o{}; opts.AppendExecutionProvider_TensorRT(o); });
        else if (device == "XNNPACK")  tryEP("XNNPACK",  [&]{ opts.AppendExecutionProvider("XNNPACK", {}); });  // fast ARM CPU
        // CPU is always present by default.

        session_ = std::make_unique<Ort::Session>(env_, modelPath.c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < session_->GetInputCount(); ++i)
            inNames_.push_back(session_->GetInputNameAllocated(i, alloc).get());
        for (size_t i = 0; i < session_->GetOutputCount(); ++i)
            outNames_.push_back(session_->GetOutputNameAllocated(i, alloc).get());
        for (auto& s : inNames_)  inNamesC_.push_back(s.c_str());
        for (auto& s : outNames_) outNamesC_.push_back(s.c_str());

        ready_ = true;
        LOGI() << "OnnxRuntimeModel: loaded '" << modelPath << "' (" << inNames_.size()
               << " in, " << outNames_.size() << " out)";
        return true;
    } catch (const std::exception& e) {
        LOGE() << "OnnxRuntimeModel: failed to load '" << modelPath << "': " << e.what();
        return false;
    }
}

std::vector<cv::Mat> OnnxRuntimeModel::infer(const cv::Mat& blobNCHW) {
    if (!ready_) return {};

    std::vector<int64_t> shape;
    for (int i = 0; i < blobNCHW.dims; ++i) shape.push_back(blobNCHW.size[i]);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(
        mem, const_cast<float*>(blobNCHW.ptr<float>()), blobNCHW.total(), shape.data(), shape.size());

    auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                 inNamesC_.data(), &input, 1,
                                 outNamesC_.data(), outNamesC_.size());

    std::vector<cv::Mat> outs;
    for (auto& v : outputs) {
        auto info = v.GetTensorTypeAndShapeInfo();
        auto sh   = info.GetShape();
        std::vector<int> dims;
        for (auto d : sh) dims.push_back(static_cast<int>(d));
        cv::Mat m(static_cast<int>(dims.size()), dims.data(), CV_32F);
        std::memcpy(m.data, v.GetTensorData<float>(), info.GetElementCount() * sizeof(float));
        outs.push_back(m);
    }
    return outs;
}

} // namespace lpr
#endif // LPR_WITH_ONNXRUNTIME
