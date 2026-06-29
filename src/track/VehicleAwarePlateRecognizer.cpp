#include "lpr/track/VehicleAwarePlateRecognizer.h"

#include <algorithm>
#include <cmath>

namespace lpr {

VehicleAwarePlateRecognizer::VehicleAwarePlateRecognizer(IVehicleDetector& detector,
                                                         IPlateRecognizer& plateStage,
                                                         Config cfg)
    : detector_(detector), plateStage_(plateStage), cfg_(cfg) {}

VehicleTracker& VehicleAwarePlateRecognizer::trackerFor(const std::string& gate) {
    auto it = trackers_.find(gate);
    if (it == trackers_.end())
        it = trackers_.emplace(gate, std::make_unique<VehicleTracker>(cfg_.frameRate, cfg_.trackBuffer)).first;
    return *it->second;
}

std::vector<PlateResult> VehicleAwarePlateRecognizer::recognize(const cv::Mat& frame,
                                                                const std::string& gate,
                                                                long timestamp) {
    std::vector<PlateResult> results;
    if (frame.empty()) return results;

    // 1) Detect vehicles, 2) update this gate's tracker to get stable ids.
    std::vector<VehicleDetection> dets = detector_.detect(frame);
    std::vector<TrackInput> inputs;
    inputs.reserve(dets.size());
    for (const auto& d : dets) {
        if (cfg_.classFilter >= 0 && d.classId != cfg_.classFilter) continue;
        inputs.push_back({ d.box, d.score, d.classId });
    }

    // With tracking: associate detections to stable ids. Without: treat each detection as a
    // one-frame "track" (trackId = -1), i.e. the original plate_detection_withouttrack_car.
    std::vector<TrackedObject> tracked;
    if (cfg_.useTracking) {
        tracked = trackerFor(gate).update(inputs);
    } else {
        tracked.reserve(inputs.size());
        for (const auto& in : inputs)
            tracked.push_back(TrackedObject{ -1, -1, in.box, in.score, in.classId });
    }

    const cv::Rect frameRect(0, 0, frame.cols, frame.rows);

    // Record every tracked vehicle for the preview (boxes + ids), independent of whether a plate
    // is read inside it, and maintain the vehicle count exactly as the original did:
    //   tracked_vehicles[gate] = max(label_track, tracked_vehicles[gate])
    // label_track is assigned once per confirmed track and never reused, so the count is stable
    // (track_id, by contrast, is reassigned on re-activation and would inflate the count).
    lastVehicles_.clear();
    for (const auto& v : tracked) {
        cv::Rect b = v.box & frameRect;
        if (b.width < 2 || b.height < 2) continue;
        const int displayId = (v.labelId > 0) ? v.labelId : v.trackId;
        lastVehicles_.push_back(VehicleAnnotation{ b, displayId, v.classId });
        if (cfg_.useTracking && v.labelId > 0)
            maxLabel_[gate] = std::max(maxLabel_[gate], v.labelId);
    }
    totalVehicles_ = cfg_.useTracking ? maxLabel_[gate]
                                      : totalVehicles_ + (int)lastVehicles_.size();

    // 3) For each tracked vehicle, crop the plate region and run the plate stage on it.
    for (const auto& v : tracked) {
        // Plate-on-vehicle crop, matching the original process_tracker_car: the lower-centre of
        // the vehicle box (trim ~10% left / ~5% right, start about half-a-width up from the
        // bottom, extend ~15% below). This is where the plate sits; cropping the FULL box instead
        // shrinks the plate and distorts its aspect ratio when the detector resizes the crop to
        // its input, which made EAST miss plates that the full-frame path found.
        const float bw = static_cast<float>(v.box.width);
        const float bh = static_cast<float>(v.box.height);
        const int x0 = static_cast<int>(std::lround(v.box.x + bw * 0.10f));
        const int y0 = static_cast<int>(std::lround(v.box.y + std::max(0.0f, bh - bw * 0.5f)));
        const int x1 = static_cast<int>(std::lround(v.box.x + bw * 0.95f));
        const int y1 = static_cast<int>(std::lround(v.box.y + bh * 1.15f));
        cv::Rect box = cv::Rect(cv::Point(x0, y0), cv::Point(x1, y1));
        if (cfg_.boxPadding > 0) {                       // optional extra margin (default 0)
            box.x -= cfg_.boxPadding; box.y -= cfg_.boxPadding;
            box.width += 2 * cfg_.boxPadding; box.height += 2 * cfg_.boxPadding;
        }
        box &= frameRect;                                // clamp to frame
        if (box.width < 2 || box.height < 2) continue;

        cv::Mat crop = frame(box);
        std::vector<PlateResult> plates = plateStage_.recognize(crop, gate, timestamp);

        // 4) Map plate boxes from crop space back to the full frame and tag the track.
        for (PlateResult& p : plates) {
            p.box.center.x += box.x;
            p.box.center.y += box.y;
            p.trackId    = v.trackId;
            p.vehicleBox = box;
            p.gate       = gate;
            p.timestamp  = timestamp;
            results.push_back(std::move(p));
        }
    }
    return results;
}

} // namespace lpr
