#pragma once
// RoiCropRecognizer - the clean port of the per-camera detection ROI that the original
// applied before ALPR (SettingsManager::getGeneralRoi + cropAndMask). It is a DECORATOR:
// it crops each frame to the gate's ROI (and optionally zeroes everything outside a polygon
// mask), delegates to an inner IPlateRecognizer, then maps the result boxes back to
// full-frame coordinates. Works in front of PlateRecognizer or VehicleAwarePlateRecognizer.
#include "lpr/detect/IPlateRecognizer.h"

#include <functional>
#include <string>
#include <vector>

namespace lpr {

class RoiCropRecognizer : public IPlateRecognizer {
public:
    // gate,width,height -> crop rect (in full-frame pixels). Empty/zero-area = whole frame.
    using RoiProvider  = std::function<cv::Rect(const std::string& gate, int width, int height)>;
    // gate,width,height -> polygon (full-frame pixels) outside which pixels are masked to 0.
    // Empty vector = no mask.
    using MaskProvider = std::function<std::vector<cv::Point>(const std::string& gate, int width, int height)>;

    RoiCropRecognizer(IPlateRecognizer& inner, RoiProvider roi, MaskProvider mask = {});

    std::vector<PlateResult> recognize(const cv::Mat& frame,
                                       const std::string& gate, long timestamp) override;

    // Pass-through of the inner recognizer's vehicles, remapped from crop space to source coords.
    std::vector<VehicleAnnotation> lastVehicles() const override;
    int totalVehicleCount() const override { return inner_.totalVehicleCount(); }

private:
    IPlateRecognizer& inner_;
    RoiProvider  roi_;
    MaskProvider mask_;
    cv::Point    lastOffset_{0, 0};   // crop.tl() from the last recognize(), to remap vehicles
};

} // namespace lpr
