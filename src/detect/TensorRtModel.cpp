#ifdef LPR_WITH_TENSORRT
#include "lpr/detect/TensorRtModel.h"
#include "lpr/Log.h"

#include <cuda_runtime_api.h>
#include <fstream>
#include <numeric>

namespace lpr {
namespace {

class TrtLogger : public nvinfer1::ILogger {
    void log(Severity s, const char* msg) noexcept override {
        if (s == Severity::kERROR || s == Severity::kINTERNAL_ERROR) LOGE() << "[TensorRT] " << msg;
        else if (s == Severity::kWARNING)                            LOGW() << "[TensorRT] " << msg;
    }
};
TrtLogger gTrtLogger;

size_t volume(const nvinfer1::Dims& d) {
    size_t v = 1;
    for (int i = 0; i < d.nbDims; ++i) v *= static_cast<size_t>(d.d[i]);
    return v;
}

} // namespace

TensorRtModel::~TensorRtModel() {
    freeBuffers();
    if (stream_)  cudaStreamDestroy(static_cast<cudaStream_t>(stream_));
    delete context_;
    delete engine_;
    delete runtime_;
}

void TensorRtModel::freeBuffers() {
    for (void*& b : deviceBuffers_) { if (b) cudaFree(b); b = nullptr; }
}

bool TensorRtModel::load(const std::string& enginePath, const std::string& /*device*/) {
    std::ifstream f(enginePath, std::ios::binary);
    if (!f) { LOGE() << "TensorRtModel: cannot open engine '" << enginePath << "'"; return false; }
    std::vector<char> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    runtime_ = nvinfer1::createInferRuntime(gTrtLogger);
    if (!runtime_) { LOGE() << "TensorRtModel: createInferRuntime failed"; return false; }
    engine_ = runtime_->deserializeCudaEngine(blob.data(), blob.size());
    if (!engine_) { LOGE() << "TensorRtModel: deserializeCudaEngine failed"; return false; }
    context_ = engine_->createExecutionContext();
    if (!context_) { LOGE() << "TensorRtModel: createExecutionContext failed"; return false; }

    cudaStream_t stream;
    if (cudaStreamCreate(&stream) != cudaSuccess) { LOGE() << "TensorRtModel: cudaStreamCreate failed"; return false; }
    stream_ = stream;

    const int n = engine_->getNbIOTensors();
    deviceBuffers_.assign(n, nullptr);
    bufferBytes_.assign(n, 0);
    ioNames_.clear();
    outputNames_.clear();
    for (int i = 0; i < n; ++i) {
        const char* name = engine_->getIOTensorName(i);
        ioNames_.emplace_back(name);
        if (engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) inputName_ = name;
        else                                                                  outputNames_.emplace_back(name);
    }
    if (inputName_.empty()) { LOGE() << "TensorRtModel: no input tensor found"; return false; }

    ready_ = true;
    LOGI() << "TensorRtModel: loaded '" << enginePath << "' (" << n << " I/O tensors, "
           << outputNames_.size() << " outputs)";
    return true;
}

std::vector<cv::Mat> TensorRtModel::infer(const cv::Mat& blobNCHW) {
    if (!ready_) return {};
    auto stream = static_cast<cudaStream_t>(stream_);

    // 1) Set the dynamic input shape from the blob (NCHW).
    nvinfer1::Dims dims;
    dims.nbDims = blobNCHW.dims;
    for (int i = 0; i < blobNCHW.dims; ++i) dims.d[i] = blobNCHW.size[i];
    if (!context_->setInputShape(inputName_.c_str(), dims)) {
        LOGE() << "TensorRtModel: setInputShape failed";
        return {};
    }

    // 2) (Re)allocate device buffers to match resolved shapes and bind addresses.
    for (int i = 0; i < static_cast<int>(ioNames_.size()); ++i) {
        const std::string& name = ioNames_[i];
        nvinfer1::Dims sh = context_->getTensorShape(name.c_str());
        size_t bytes = volume(sh) * sizeof(float);
        if (bytes != bufferBytes_[i]) {
            if (deviceBuffers_[i]) cudaFree(deviceBuffers_[i]);
            if (cudaMalloc(&deviceBuffers_[i], bytes) != cudaSuccess) {
                LOGE() << "TensorRtModel: cudaMalloc failed for '" << name << "'";
                return {};
            }
            bufferBytes_[i] = bytes;
        }
        context_->setTensorAddress(name.c_str(), deviceBuffers_[i]);
    }

    // 3) Host -> device for the input.
    int inIdx = static_cast<int>(std::distance(ioNames_.begin(),
                    std::find(ioNames_.begin(), ioNames_.end(), inputName_)));
    cudaMemcpyAsync(deviceBuffers_[inIdx], blobNCHW.ptr<float>(),
                    bufferBytes_[inIdx], cudaMemcpyHostToDevice, stream);

    // 4) Run.
    if (!context_->enqueueV3(stream)) { LOGE() << "TensorRtModel: enqueueV3 failed"; return {}; }

    // 5) Device -> host for each output, wrapped as an n-dim CV_32F cv::Mat.
    std::vector<cv::Mat> outs;
    outs.reserve(outputNames_.size());
    for (const std::string& name : outputNames_) {
        int idx = static_cast<int>(std::distance(ioNames_.begin(),
                      std::find(ioNames_.begin(), ioNames_.end(), name)));
        nvinfer1::Dims sh = context_->getTensorShape(name.c_str());
        std::vector<int> d(sh.d, sh.d + sh.nbDims);
        cv::Mat m(static_cast<int>(d.size()), d.data(), CV_32F);
        cudaMemcpyAsync(m.data, deviceBuffers_[idx], bufferBytes_[idx],
                        cudaMemcpyDeviceToHost, stream);
        outs.push_back(std::move(m));
    }
    cudaStreamSynchronize(stream);
    return outs;
}

} // namespace lpr
#endif // LPR_WITH_TENSORRT
