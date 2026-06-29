#include "lpr/track/PlateTrackingRecognizer.h"

namespace lpr {

namespace {
double iou(const cv::Rect& a, const cv::Rect& b) {
    const int inter = (a & b).area();
    const int uni   = a.area() + b.area() - inter;
    return uni > 0 ? static_cast<double>(inter) / uni : 0.0;
}
} // namespace

PlateTrackingRecognizer::PlateTrackingRecognizer(IPlateRecognizer& inner, Config cfg)
    : inner_(inner), cfg_(cfg) {}

VehicleTracker& PlateTrackingRecognizer::trackerFor(const std::string& gate) {
    auto it = trackers_.find(gate);
    if (it == trackers_.end())
        it = trackers_.emplace(gate, std::make_unique<VehicleTracker>(cfg_.frameRate, cfg_.trackBuffer)).first;
    return *it->second;
}

std::vector<PlateResult> PlateTrackingRecognizer::recognize(const cv::Mat& frame,
                                                            const std::string& gate, long timestamp) {
    std::vector<PlateResult> plates = inner_.recognize(frame, gate, timestamp);

    // Feed plate boxes to the per-gate tracker (always update, even with none, to age tracks).
    std::vector<TrackInput> inputs;
    inputs.reserve(plates.size());
    for (const auto& p : plates)
        inputs.push_back({ p.box.boundingRect(), p.confidence, 0 });

    std::vector<TrackedObject> tracked = trackerFor(gate).update(inputs);

    // Bind each plate to the best-overlapping tracked box -> stable trackId.
    for (PlateResult& p : plates) {
        const cv::Rect pb = p.box.boundingRect();
        double best = cfg_.minIoU;
        int bestId = -1;
        for (const auto& t : tracked) {
            const double s = iou(pb, t.box);
            if (s >= best) { best = s; bestId = t.trackId; }
        }
        p.trackId = bestId;
    }
    return plates;
}

} // namespace lpr
