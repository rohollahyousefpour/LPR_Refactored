#include "lpr/detect/PlateOcr.h"
#include "lpr/detect/CtcDecoder.h"
#include "lpr/Log.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace lpr {

bool PlateOcr::load(const std::string& modelPath, const Config& cfg) {
    cfg_   = cfg;
    model_ = makeInferModel(cfg.backend);
    if (model_ && cfg_.nhwcU8)
        model_->setInputFormat(InputFormat::NhwcU8);   // must be set before load()
    ready_ = model_ && model_->load(modelPath, cfg.device);
    // NHWC/u8 only applies to the OpenVINO backend; for any other engine fall back to NCHW/f32.
    if (ready_ && model_->backend() != Backend::OpenVino)
        cfg_.nhwcU8 = false;
    if (ready_)
        LOGI() << "PlateOcr: ready on " << toString(model_->backend())
               << " (" << (cfg_.nhwcU8 ? "NHWC/u8" : "NCHW/f32")
               << ", " << cfg_.inputSize.width << "x" << cfg_.inputSize.height << ")";
    return ready_;
}

std::string PlateOcr::read(const cv::Mat& plateBgr, double& confidence) const {
    confidence = 0.0;
    if (!ready_ || plateBgr.empty()) return {};

    std::vector<cv::Mat> outs;
    if (cfg_.nhwcU8) {
        // Original Ocr_Model: cv::resize to (input_w,input_h), feed raw BGR u8 (NHWC), no scaling.
        cv::Mat resized;
        cv::resize(plateBgr, resized, cfg_.inputSize);
        if (resized.type() != CV_8UC3) resized.convertTo(resized, CV_8UC3);
        outs = model_->infer(resized);
    } else {
        cv::Mat blob = cv::dnn::blobFromImage(plateBgr, cfg_.scale, cfg_.inputSize,
                                              cv::Scalar(), cfg_.swapRB, /*crop=*/false, CV_32F);
        outs = model_->infer(blob);
    }
    if (outs.empty()) return {};

    // Output is [timesteps x numClasses]. numClasses = alphabet.size() + 1 (the CTC blank is the
    // EXTRA class). Original puts the blank at the LAST index (prob[num_classes-1]); index i->alphabet[i].
    const cv::Mat& o = outs.front();
    int total = static_cast<int>(o.total());
    const int numClasses = static_cast<int>(cfg_.alphabet.size()) + 1;
    if (numClasses <= 1 || total % numClasses != 0) {
        LOGW() << "PlateOcr: output size " << total << " not divisible by numClasses " << numClasses;
        return {};
    }
    const int timesteps = total / numClasses;
    const int blankIdx  = cfg_.blankLast ? (numClasses - 1) : cfg_.blankIndex;

    std::vector<float> data(reinterpret_cast<const float*>(o.datastart),
                            reinterpret_cast<const float*>(o.datastart) + total);
    return ctcGreedyDecode(data, timesteps, numClasses, cfg_.alphabet, blankIdx, &confidence);
}

} // namespace lpr
