#include "lpr/detect/VehicleDetector.h"
#include "lpr/Log.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace lpr {

static inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

std::vector<VehicleDetection> decodeYolo(const std::vector<YoloHead>& heads,
                                         const VehicleConfig& cfg,
                                         float scaleX, float scaleY) {
    const int step = 5 + cfg.numClasses;                       // values per anchor
    std::vector<cv::Rect> boxes;
    std::vector<float>    scores;
    std::vector<int>      labels;

    for (const auto& head : heads) {
        if (!head.data || head.anchorMask.empty()) continue;
        const int numAnchors = static_cast<int>(head.anchorMask.size());
        const int channels   = numAnchors * step;

        for (int h = 0; h < head.gridH; ++h) {
            for (int w = 0; w < head.gridW; ++w) {
                const int cellBase = (h * head.gridW + w) * channels;
                for (int a = 0; a < numAnchors; ++a) {
                    const int ind = cellBase + a * step;
                    const float boxConf = sigmoid(head.data[ind + 4]);
                    if (boxConf <= cfg.scoreThreshold) continue;

                    const float xs = (sigmoid(head.data[ind + 0]) * 2.0f - 0.5f + w) / head.gridW;
                    const float ys = (sigmoid(head.data[ind + 1]) * 2.0f - 0.5f + h) / head.gridH;
                    const float wr = sigmoid(head.data[ind + 2]);
                    const float hr = sigmoid(head.data[ind + 3]);

                    const auto& anchor = cfg.anchors[head.anchorMask[a]];
                    const float width  = std::pow(wr * 2.0f, 2.0f) * anchor[0] / cfg.width;
                    const float height = std::pow(hr * 2.0f, 2.0f) * anchor[1] / cfg.height;

                    const float xt = (xs - width  / 2.0f) * scaleX * cfg.width;
                    const float yt = (ys - height / 2.0f) * scaleY * cfg.height;
                    const float ws = std::min(width  * cfg.width  * scaleX, cfg.width  * scaleX);
                    const float hs = std::min(height * cfg.height * scaleY, cfg.height * scaleY);

                    int   bestClass = -1;
                    float bestScore = -1.f;
                    for (int c = 0; c < cfg.numClasses; ++c) {
                        const float cs = sigmoid(head.data[ind + 5 + c]);
                        if (cs > bestScore) { bestScore = cs; bestClass = c; }
                    }
                    boxes.emplace_back(cvRound(xt), cvRound(yt), cvRound(ws), cvRound(hs));
                    scores.push_back(bestScore * boxConf);
                    labels.push_back(bestClass);
                }
            }
        }
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, scores, cfg.scoreThreshold, cfg.nmsThreshold, keep);

    std::vector<VehicleDetection> out;
    out.reserve(keep.size());
    for (int i : keep) out.push_back({ boxes[i], scores[i], labels[i] });
    return out;
}

bool VehicleDetector::load(const std::string& modelPath, Backend backend,
                           const std::string& device, const VehicleConfig& cfg) {
    cfg_ = cfg;
    cfg_.width  = (cfg_.width  / 32) * 32;
    cfg_.height = (cfg_.height / 32) * 32;
    model_ = makeInferModel(backend);
    ready_ = model_ && model_->load(modelPath, device);
    if (ready_)
        LOGI() << "VehicleDetector: ready on " << toString(model_->backend())
               << " (" << cfg_.width << "x" << cfg_.height << ", " << cfg_.numClasses << " classes)";
    return ready_;
}

std::vector<VehicleDetection> VehicleDetector::detect(const cv::Mat& imageBgr) {
    if (!ready_ || imageBgr.empty()) return {};

    cv::Mat resized;
    cv::resize(imageBgr, resized, cv::Size(cfg_.width, cfg_.height));
    if (cfg_.swapRB) cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);

    cv::Mat blob;
    if (cfg_.nhwc) {
        cv::Mat f;
        resized.convertTo(f, CV_32F, cfg_.scale, cfg_.mean);
        int sz[4] = {1, cfg_.height, cfg_.width, 3};
        blob = cv::Mat(4, sz, CV_32F);
        std::memcpy(blob.data, f.data, f.total() * f.elemSize());
    } else {
        blob = cv::dnn::blobFromImage(resized, cfg_.scale, cv::Size(cfg_.width, cfg_.height),
                                      cv::Scalar(-cfg_.mean, -cfg_.mean, -cfg_.mean), false, false, CV_32F);
    }

    std::vector<cv::Mat> outs = model_->infer(blob);
    if (outs.empty()) { LOGW() << "VehicleDetector: model returned no outputs"; return {}; }

    // Which heads to decode (default: all).
    std::vector<int> heads = cfg_.headIndices;
    if (heads.empty())
        for (int i = 0; i < static_cast<int>(outs.size()); ++i) heads.push_back(i);

    std::vector<YoloHead> yh;
    for (int idx : heads) {
        if (idx < 0 || idx >= static_cast<int>(outs.size())) continue;
        const cv::Mat& o = outs[idx];
        if (o.dims < 4) { LOGW() << "VehicleDetector: head " << idx << " is not 4-D NHWC"; continue; }
        YoloHead head;
        head.data       = reinterpret_cast<const float*>(o.datastart);   // NHWC [1,gH,gW,C]
        head.gridH      = o.size[1];
        head.gridW      = o.size[2];
        head.anchorMask = (idx < static_cast<int>(cfg_.anchorMasks.size()))
                          ? cfg_.anchorMasks[idx] : cfg_.anchorMasks.back();
        yh.push_back(head);
    }

    const float scaleX = static_cast<float>(imageBgr.cols) / cfg_.width;
    const float scaleY = static_cast<float>(imageBgr.rows) / cfg_.height;
    return decodeYolo(yh, cfg_, scaleX, scaleY);
}

} // namespace lpr
