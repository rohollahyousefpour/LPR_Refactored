#include "lpr/detect/RoiCropRecognizer.h"
#include "lpr/Log.h"

#include <opencv2/imgproc.hpp>

namespace lpr {

RoiCropRecognizer::RoiCropRecognizer(IPlateRecognizer& inner, RoiProvider roi, MaskProvider mask)
    : inner_(inner), roi_(std::move(roi)), mask_(std::move(mask)) {}

std::vector<PlateResult> RoiCropRecognizer::recognize(const cv::Mat& frame,
                                                      const std::string& gate, long timestamp) {
    if (frame.empty()) return {};

    const cv::Rect full(0, 0, frame.cols, frame.rows);
    cv::Rect crop = full;
    if (roi_) {
        cv::Rect r = roi_(gate, frame.cols, frame.rows);
        if (r.area() > 0) crop = (r & full);
    }
    if (crop.area() <= 0) crop = full;

    cv::Mat region = frame(crop);

    // Optional polygon mask: zero everything outside the polygon (shifted into crop space).
    cv::Mat masked;
    std::size_t polyPts = 0;
    if (mask_) {
        std::vector<cv::Point> poly = mask_(gate, frame.cols, frame.rows);
        polyPts = poly.size();
        if (poly.size() >= 3) {
            for (auto& p : poly) p -= crop.tl();
            cv::Mat m = cv::Mat::zeros(region.size(), CV_8UC1);
            std::vector<std::vector<cv::Point>> polys{poly};
            cv::fillPoly(m, polys, cv::Scalar(255));
            masked = cv::Mat::zeros(region.size(), region.type());
            region.copyTo(masked, m);
            region = masked;
        }
    }

    LOGD() << "RoiCropRecognizer[" << gate << "]: frame=" << frame.cols << "x" << frame.rows
           << " crop=[" << crop.x << "," << crop.y << " " << crop.width << "x" << crop.height << "]"
           << (crop == full ? " (FULL-no ROI)" : " (cropped)") << " polygonPts=" << polyPts;

    std::vector<PlateResult> results = inner_.recognize(region, gate, timestamp);

    // Map boxes from crop space back to full-frame coordinates.
    const cv::Point off = crop.tl();
    lastOffset_ = off;
    for (PlateResult& r : results) {
        r.box.center.x += off.x;
        r.box.center.y += off.y;
        if (r.vehicleBox.area() > 0) r.vehicleBox += off;   // cv::Rect + cv::Point = shifted rect
    }
    return results;
}

std::vector<VehicleAnnotation> RoiCropRecognizer::lastVehicles() const {
    std::vector<VehicleAnnotation> v = inner_.lastVehicles();
    for (auto& a : v) a.box += lastOffset_;                 // crop space -> source frame
    return v;
}

} // namespace lpr
