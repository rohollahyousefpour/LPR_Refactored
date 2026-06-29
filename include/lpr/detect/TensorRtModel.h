#pragma once
// NVIDIA TensorRT backend (discrete GPU and Jetson / ARM). Compiled only when
// WITH_TENSORRT is set and TensorRT + CUDA are found. Loads a serialized engine
// (.engine / .plan) built by trtexec or the TensorRT API. Uses the modern
// name-based I/O API (TensorRT 8.5+ / 10.x): setInputShape + setTensorAddress +
// enqueueV3. Assumes FP32 network I/O bindings.
#ifdef LPR_WITH_TENSORRT
#include "lpr/detect/InferModel.h"
#include <NvInfer.h>
#include <string>
#include <vector>

namespace lpr {

class TensorRtModel : public InferModel {
public:
    ~TensorRtModel() override;
    bool                 load(const std::string& enginePath, const std::string& device = "CUDA") override;
    std::vector<cv::Mat> infer(const cv::Mat& blobNCHW) override;
    Backend              backend() const override { return Backend::TensorRt; }

private:
    void freeBuffers();

    nvinfer1::IRuntime*          runtime_ = nullptr;
    nvinfer1::ICudaEngine*       engine_  = nullptr;
    nvinfer1::IExecutionContext* context_ = nullptr;
    void*                        stream_  = nullptr;          // cudaStream_t

    std::string                  inputName_;
    std::vector<std::string>     outputNames_;
    std::vector<void*>           deviceBuffers_;              // one per I/O tensor (by index)
    std::vector<size_t>          bufferBytes_;
    std::vector<std::string>     ioNames_;
    bool                         ready_ = false;
};

} // namespace lpr
#endif // LPR_WITH_TENSORRT
