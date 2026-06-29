#ifdef LPR_WITH_HAILO
#include "lpr/detect/HailoModel.h"
#include "lpr/Log.h"

namespace lpr {

bool HailoModel::load(const std::string& hefPath, const std::string& /*device*/) {
    // Skeleton (finish on a Pi with HailoRT installed):
    //   auto vdev = hailort::VDevice::create();
    //   auto hef  = hailort::Hef::create(hefPath);
    //   auto cfg  = vdev->configure(hef, ...); networkGroup_ = cfg[0];
    //   create input/output vstreams from networkGroup_.
    (void)hefPath;
    LOGW() << "HailoModel::load skeleton - implement against installed HailoRT";
    return false;
}

std::vector<cv::Mat> HailoModel::infer(const cv::Mat& blobNCHW) {
    // Skeleton: write blob to the input vstream, read output vstream(s),
    // wrap each as a CV_32F cv::Mat (dequantized).
    (void)blobNCHW;
    return {};
}

} // namespace lpr
#endif // LPR_WITH_HAILO
