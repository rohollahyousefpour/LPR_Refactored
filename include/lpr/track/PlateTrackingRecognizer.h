#pragma once
// PlateTrackingRecognizer - the clean, faithful implementation of plate_detection_track:
// run an inner plate recognizer (EAST+OCR), then TRACK the plate boxes across frames (per
// gate) and stamp each plate with a stable trackId. This is real position tracking (the
// plate boxes go through the tracker), unlike relying on PlateProcessor's text clustering.
// It is a DECORATOR over any IPlateRecognizer and itself an IPlateRecognizer, so it slots
// into the DetectionWorker seam.
#include "lpr/detect/IPlateRecognizer.h"
#include "lpr/track/VehicleTracker.h"

#include <map>
#include <memory>
#include <string>

namespace lpr {

class PlateTrackingRecognizer : public IPlateRecognizer {
public:
    struct Config {
        int    frameRate    = 30;
        int    trackBuffer  = 30;
        double minIoU       = 0.1;   // min overlap to bind a tracked box to a plate
    };

    PlateTrackingRecognizer(IPlateRecognizer& inner, Config cfg);
    PlateTrackingRecognizer(IPlateRecognizer& inner)
        : PlateTrackingRecognizer(inner, Config{}) {}

    std::vector<PlateResult> recognize(const cv::Mat& frame,
                                       const std::string& gate, long timestamp) override;

private:
    VehicleTracker& trackerFor(const std::string& gate);

    IPlateRecognizer& inner_;
    Config            cfg_;
    std::map<std::string, std::unique_ptr<VehicleTracker>> trackers_;
};

} // namespace lpr
