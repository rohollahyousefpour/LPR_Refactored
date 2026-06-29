#pragma once
// VehicleAwarePlateRecognizer - the clean port of the original plate_detection_with_car
// pipeline: detect vehicles -> track them -> crop each tracked vehicle -> run the plate
// stage (EAST + OCR) on the crop -> emit plates carrying the vehicle's stable trackId.
//
// It IS an IPlateRecognizer, so it drops straight into the DetectionWorker's recognizer
// seam. It composes other classes through abstractions, so each is swappable/testable:
//   * IVehicleDetector& - vehicle boxes (VehicleDetector, or a fake)
//   * IPlateRecognizer&  - the plate stage run on each crop (PlateRecognizer, or a fake)
//   * VehicleTracker     - one per gate, created on demand (mirrors the original's map)
//
// Per-track plate de-duplication / "read once" lived in the original here; that is the
// PlateProcessor's job (next step), so this class just attaches trackId and leaves the
// keep/drop decision to the DetectionWorker's processor seam.
#include "lpr/detect/IPlateRecognizer.h"
#include "lpr/detect/VehicleDetector.h"
#include "lpr/track/VehicleTracker.h"

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

namespace lpr {

class VehicleAwarePlateRecognizer : public IPlateRecognizer {
public:
    struct Config {
        int  frameRate   = 30;     // passed to each per-gate VehicleTracker
        int  trackBuffer = 30;
        int  classFilter = -1;     // only crop vehicles of this classId (-1 = any)
        int  boxPadding  = 0;      // pixels added around each vehicle box before cropping
        bool useTracking = true;   // false => detect+crop+plate per frame, no tracker
                                   //          (the original plate_detection_withouttrack_car)
    };

    VehicleAwarePlateRecognizer(IVehicleDetector& detector, IPlateRecognizer& plateStage, Config cfg);
    VehicleAwarePlateRecognizer(IVehicleDetector& detector, IPlateRecognizer& plateStage)
        : VehicleAwarePlateRecognizer(detector, plateStage, Config{}) {}   // default config

    // Detect+track vehicles in the frame, read plates on each, return plates with trackId.
    std::vector<PlateResult> recognize(const cv::Mat& frame,
                                       const std::string& gate, long timestamp) override;

    // Vehicles detected on the most recent recognize() call (crop-space coords; RoiCrop remaps).
    std::vector<VehicleAnnotation> lastVehicles() const override { return lastVehicles_; }
    // Distinct vehicles seen so far: unique track ids when tracking, else cumulative frame count.
    int totalVehicleCount() const override { return totalVehicles_; }

private:
    VehicleTracker& trackerFor(const std::string& gate);

    IVehicleDetector& detector_;
    IPlateRecognizer& plateStage_;
    Config            cfg_;
    std::map<std::string, std::unique_ptr<VehicleTracker>> trackers_;   // one per gate
    std::vector<VehicleAnnotation> lastVehicles_;   // most recent frame's vehicles
    std::map<std::string, int>     maxLabel_;        // per-gate running max of label_track (count)
    int                            totalVehicles_ = 0;
};

} // namespace lpr
