#include "lpr/detect/OpenCvDnnModel.h"
#include "lpr/Log.h"

namespace lpr {

bool OpenCvDnnModel::load(const std::string& modelPath, const std::string& device) {
    try {
        net_ = cv::dnn::readNet(modelPath);
    } catch (const cv::Exception& e) {
        LOGE() << "OpenCvDnnModel: failed to read '" << modelPath << "': " << e.what();
        return false;
    }
    if (net_.empty()) { LOGE() << "OpenCvDnnModel: empty net for '" << modelPath << "'"; return false; }

    // Device targeting. CUDA/OPENCL fall back to CPU automatically if unavailable.
    if (device == "CUDA" || device == "GPU") {
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);
    } else if (device == "OPENCL") {
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);
    } else {
        net_.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net_.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);   // ARM / Raspberry Pi path
    }

    outNames_ = net_.getUnconnectedOutLayersNames();
    ready_    = true;
    LOGI() << "OpenCvDnnModel: loaded '" << modelPath << "' (" << outNames_.size() << " outputs, device=" << device << ")";
    return true;
}

std::vector<cv::Mat> OpenCvDnnModel::infer(const cv::Mat& blobNCHW) {
    if (!ready_) return {};
    net_.setInput(blobNCHW);
    std::vector<cv::Mat> outs;
    net_.forward(outs, outNames_);
    return outs;
}

} // namespace lpr
