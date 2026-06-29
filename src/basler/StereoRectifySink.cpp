#include "StereoRectifySink.h"
#include "AutoRoadStereoRectifier.h"
#include "AppLogger.h"
#include <opencv2/core.hpp>

void StereoRectifySink::onFramePair(const cv::Mat& left, const cv::Mat& right, std::time_t ts) {
    if (!inner_) return;

    if (!enabled_ || !rectifier_) {
        inner_->onFramePair(left, right, ts);     // pass-through
        return;
    }

    try {
        cv::Mat L, R;
        // AutoRoadStereoRectifier::processFramePair returns true only when it is
        // in RECTIFICATION_READY state; before calibration completes it copies
        // the inputs into L/R and returns false. Either way L/R are populated,
        // so we forward them regardless of the return value.
        const bool rectified = rectifier_->processFramePair(left, right, L, R);
        (void)rectified;

        if (L.empty() || R.empty())
            inner_->onFramePair(left, right, ts); // safety: fall back to originals
        else
            inner_->onFramePair(L, R, ts);
    }
    catch (const cv::Exception& e) {
        AppLogger::LogException(e, "[stereo] processFramePair");
        inner_->onFramePair(left, right, ts);     // never stall capture on rectify failure
    }
    catch (const std::exception& e) {
        AppLogger::LogException(e, "[stereo] processFramePair");
        inner_->onFramePair(left, right, ts);
    }
    catch (...) {
        AppLogger::LogUnknownException("[stereo] processFramePair");
        inner_->onFramePair(left, right, ts);
    }
}
