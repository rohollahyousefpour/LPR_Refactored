#pragma once
//
// StereoRectifySink  (Decorator over IFrameSink)
// ----------------------------------------------
// Preserves the stereo-rectification feature from the old BaslerCamera
// (AutoRoadStereoRectifier + enableStereoRectification_) without putting that
// logic back inside the capture loop.
//
// It wraps the real sink: on each pair it optionally rectifies (left,right),
// then forwards to the wrapped sink. If rectification is disabled or fails, it
// forwards the originals unchanged, so capture never stalls on a rectify error.
//
// Wire it in the facade INSTEAD of handing the LegacySendSink directly to the
// context:
//     auto legacy = std::make_unique<LegacySendSink>(this);
//     sink_ = std::make_unique<StereoRectifySink>(std::move(legacy),
//                                                 enableStereo, rectifier);
//     ctx_.sink = sink_.get();
//
#include <memory>
#include "IFrameSink.h"

class AutoRoadStereoRectifier;   // your existing rectifier

class StereoRectifySink : public IFrameSink {
public:
    StereoRectifySink(std::unique_ptr<IFrameSink> inner,
                      bool enabled,
                      std::shared_ptr<AutoRoadStereoRectifier> rectifier)
        : inner_(std::move(inner)), enabled_(enabled), rectifier_(std::move(rectifier)) {}

    void onFramePair(const cv::Mat& left, const cv::Mat& right, std::time_t ts) override;
    void onIncompletePair(const cv::Mat& available, std::time_t ts) override {
        if (inner_) inner_->onIncompletePair(available, ts);   // can't rectify one frame
    }
    void onCaptureError(int code) override {
        if (inner_) inner_->onCaptureError(code);
    }

private:
    std::unique_ptr<IFrameSink>                 inner_;
    bool                                        enabled_;
    std::shared_ptr<AutoRoadStereoRectifier>    rectifier_;
};
