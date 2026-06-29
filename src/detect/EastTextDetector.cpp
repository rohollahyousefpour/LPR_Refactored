#include "lpr/detect/EastTextDetector.h"
#include "lpr/Log.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <cstring>

namespace lpr {

std::vector<cv::RotatedRect> decodeEast(const float* scores, const float* geometry,
                                        int Hf, int Wf, const EastConfig& cfg) {
    std::vector<cv::RotatedRect> rects;
    std::vector<float>           confs;
    rects.reserve(static_cast<size_t>(Hf) * Wf / 4 + 1);
    confs.reserve(rects.capacity());

    for (int y = 0; y < Hf; ++y) {
        for (int x = 0; x < Wf; ++x) {
            float score = scores[y * Wf + x];
            if (score < cfg.scoreThresh) continue;

            int   idx = (y * Wf + x) * 5;
            float d0  = geometry[idx + 0] * cfg.heightScale;
            float d1  = geometry[idx + 1] * cfg.widthScale1;
            float d2  = geometry[idx + 2] * cfg.heightScale;
            float d3  = geometry[idx + 3] * cfg.widthScale2;
            float angle = geometry[idx + 4];

            float h_box = d0 + d2;
            float w_box = d1 + d3;
            float ox = static_cast<float>(x * cfg.stride);
            float oy = static_cast<float>(y * cfg.stride);
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);

            cv::Point2f offset(ox + cosA * d1 + sinA * d2,
                               oy - sinA * d1 + cosA * d2);
            cv::Point2f p1 = cv::Point2f(-sinA * h_box, -cosA * h_box) + offset;
            cv::Point2f p3 = cv::Point2f(-cosA * w_box,  sinA * w_box) + offset;
            cv::RotatedRect r(0.5f * (p1 + p3), cv::Size2f(w_box, h_box),
                              -angle * 180.0f / static_cast<float>(CV_PI));
            rects.push_back(r);
            confs.push_back(score);
        }
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(rects, confs, cfg.scoreThresh, cfg.nmsThresh, keep);
    std::vector<cv::RotatedRect> out;
    out.reserve(keep.size());
    for (int i : keep) out.push_back(rects[i]);
    return out;
}

bool EastTextDetector::load(const std::string& modelPath, Backend backend,
                            const std::string& device, const EastConfig& cfg) {
    cfg_ = cfg;
    cfg_.detectWidth  = (cfg_.detectWidth  / 32) * 32;
    cfg_.detectHeight = (cfg_.detectHeight / 32) * 32;
    model_ = makeInferModel(backend);
    ready_ = model_ && model_->load(modelPath, device);
    if (ready_)
        LOGI() << "EastTextDetector: ready on " << toString(model_->backend())
               << " (" << cfg_.detectWidth << "x" << cfg_.detectHeight << ")";
    return ready_;
}

std::vector<cv::RotatedRect> EastTextDetector::detect(const cv::Mat& imageBgr) {
    if (!ready_ || imageBgr.empty()) return {};

    cv::Mat resized;
    cv::resize(imageBgr, resized, cv::Size(cfg_.detectWidth, cfg_.detectHeight));

    cv::Mat blob;
    if (cfg_.nhwc) {
        cv::Mat norm;
        resized.convertTo(norm, CV_32F, cfg_.normScale, cfg_.normMean);   // HWC float
        int sz[4] = {1, cfg_.detectHeight, cfg_.detectWidth, 3};
        blob = cv::Mat(4, sz, CV_32F);
        std::memcpy(blob.data, norm.data, norm.total() * norm.elemSize());
    } else {
        float meanVal = -cfg_.normMean / cfg_.normScale;                  // NCHW path
        blob = cv::dnn::blobFromImage(resized, cfg_.normScale,
                                      cv::Size(cfg_.detectWidth, cfg_.detectHeight),
                                      cv::Scalar(meanVal, meanVal, meanVal), false, false, CV_32F);
    }

    std::vector<cv::Mat> outs = model_->infer(blob);
    if (outs.size() < 2) { LOGW() << "EastTextDetector: expected 2 outputs (scores, geometry)"; return {}; }

    // Identify outputs by channel count, NOT index: OpenVINO may return them in either order.
    // score map = last dim 1, geometry (RBOX) = last dim 5.
    const cv::Mat* scoresP = nullptr; const cv::Mat* geometryP = nullptr;
    for (const cv::Mat& o : outs) {
        if (o.dims >= 4 && o.size[3] == 1) scoresP = &o;
        else if (o.dims >= 4 && o.size[3] == 5) geometryP = &o;
    }
    if (!scoresP || !geometryP) {
        LOGW() << "EastTextDetector: could not match score(1ch)+geometry(5ch) outputs; "
               << "out0 dims=" << outs[0].dims << " last=" << (outs[0].dims>=4?outs[0].size[3]:-1)
               << " out1 dims=" << outs[1].dims << " last=" << (outs[1].dims>=4?outs[1].size[3]:-1);
        return {};
    }
    const cv::Mat& scores   = *scoresP;
    const cv::Mat& geometry = *geometryP;
    int Hf = scores.size[1], Wf = scores.size[2];

    auto boxes = decodeEast(reinterpret_cast<const float*>(scores.datastart),
                            reinterpret_cast<const float*>(geometry.datastart), Hf, Wf, cfg_);
    if (cfg_.debugLog)
        LOGI() << "EastTextDetector: score map " << Wf << "x" << Hf << " -> " << boxes.size() << " box(es)";

    float sx = static_cast<float>(imageBgr.cols) / cfg_.detectWidth;
    float sy = static_cast<float>(imageBgr.rows) / cfg_.detectHeight;
    for (auto& b : boxes) {
        b.center.x *= sx; b.center.y *= sy;
        b.size.width *= sx; b.size.height *= sy;
    }
    return boxes;
}

void EastTextDetector::fourPointsTransform(const cv::Mat& frame, const cv::RotatedRect& box, cv::Mat& result) {
    cv::Mat rot = cv::getRotationMatrix2D(box.center, box.angle, 1.0);
    cv::Mat rotated;
    cv::warpAffine(frame, rotated, rot, frame.size(), cv::INTER_CUBIC);
    cv::Size rectSize = box.size;
    if (box.angle < -45.0f) std::swap(rectSize.width, rectSize.height);
    cv::getRectSubPix(rotated, rectSize, box.center, result);
}

} // namespace lpr
