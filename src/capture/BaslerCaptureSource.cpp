#include "lpr/capture/BaslerCaptureSource.h"
#ifdef LPR_WITH_PYLON

#include "lpr/Log.h"
#include "lpr/util/Time.h"
#include <opencv2/opencv.hpp>

using namespace Pylon;

namespace lpr {

BaslerCaptureSource::BaslerCaptureSource(cv::Rect roi) : roi_(roi) {
    PylonInitialize();
}

BaslerCaptureSource::~BaslerCaptureSource() {
    stop();
    PylonTerminate();
}

void BaslerCaptureSource::setAddress(const std::string& serial, int /*delayMs*/) {
    serial_ = serial;
}

void BaslerCaptureSource::run() {
    try {
        CDeviceInfo wanted;
        if (!serial_.empty())
            wanted.SetSerialNumber(serial_.c_str());

        camera_.Attach(serial_.empty()
            ? CTlFactory::GetInstance().CreateFirstDevice()
            : CTlFactory::GetInstance().CreateFirstDevice(wanted));
        camera_.Open();
        camera_.MaxNumBuffer = 5;
        camera_.StartGrabbing(GrabStrategy_LatestImageOnly);
        running_ = true;

        CImageFormatConverter converter;
        converter.OutputPixelFormat = PixelType_BGR8packed;
        CPylonImage   pylonImage;
        CGrabResultPtr result;

        while (running_ && camera_.IsGrabbing()) {
            camera_.RetrieveResult(5000, result, TimeoutHandling_Return);
            if (!result || !result->GrabSucceeded())
                continue;

            converter.Convert(pylonImage, result);
            cv::Mat frame(static_cast<int>(result->GetHeight()),
                          static_cast<int>(result->GetWidth()),
                          CV_8UC3,
                          static_cast<uint8_t*>(pylonImage.GetBuffer()));

            const cv::Rect bounds(0, 0, frame.cols, frame.rows);
            if (roi_.area() > 0 && (roi_ & bounds) == roi_)
                emitFrame(frame(roi_).clone(), cv::Mat(), nowEpochSeconds());
            else
                emitFrame(frame, cv::Mat(), nowEpochSeconds());
        }
    } catch (const GenericException& e) {
        LOGE() << "BaslerCaptureSource: " << e.GetDescription();
        emitError();
    }
    running_ = false;
}

void BaslerCaptureSource::stop() {
    running_ = false;
    try {
        if (camera_.IsGrabbing()) camera_.StopGrabbing();
        if (camera_.IsOpen())     camera_.Close();
    } catch (const GenericException& e) {
        LOGW() << "BaslerCaptureSource: stop: " << e.GetDescription();
    }
}

bool BaslerCaptureSource::isLive() const {
    return running_;
}

} // namespace lpr

#endif // LPR_WITH_PYLON
