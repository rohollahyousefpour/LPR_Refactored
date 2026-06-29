#pragma once
// BaslerCaptureSource - clean minimal single-camera Pylon grabber (the original
// Basler/ facade with stereo/bandwidth/exposure/trigger is a later specialized
// port). Entire file is active only when the Pylon SDK is compiled in.
#ifdef LPR_WITH_PYLON

#include "lpr/capture/CaptureSource.h"
#include <opencv2/core.hpp>
#include <pylon/PylonIncludes.h>
#include <atomic>
#include <string>

namespace lpr {

class BaslerCaptureSource : public CaptureSource {
public:
    explicit BaslerCaptureSource(cv::Rect roi = cv::Rect());
    ~BaslerCaptureSource() override;

    // address = camera serial number ("" => first device found)
    void setAddress(const std::string& serial, int delayMs) override;
    void run()  override;
    void stop() override;
    bool isLive() const override;

private:
    std::string            serial_;
    cv::Rect               roi_;
    std::atomic<bool>      running_{false};
    Pylon::CInstantCamera  camera_;
};

} // namespace lpr

#endif // LPR_WITH_PYLON
