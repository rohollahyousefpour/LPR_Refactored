#include "lpr/track/VehicleTracker.h"
#include "lpr/track/bytetrack/BYTETracker.h"

namespace lpr {

struct VehicleTracker::Impl {
    BYTETracker tracker;
    Impl(int frameRate, int trackBuffer) : tracker(frameRate, trackBuffer) {}
};

VehicleTracker::VehicleTracker(int frameRate, int trackBuffer)
    : impl_(std::make_unique<Impl>(frameRate, trackBuffer)) {}

VehicleTracker::~VehicleTracker() = default;
VehicleTracker::VehicleTracker(VehicleTracker&&) noexcept = default;
VehicleTracker& VehicleTracker::operator=(VehicleTracker&&) noexcept = default;

std::vector<TrackedObject> VehicleTracker::update(const std::vector<TrackInput>& detections) {
    // Adapt clean inputs -> ByteTrack Object (rect is x,y,w,h in float).
    std::vector<Object> objects;
    objects.reserve(detections.size());
    for (const auto& d : detections) {
        Object o;
        o.rect  = cv::Rect_<float>(static_cast<float>(d.box.x), static_cast<float>(d.box.y),
                                   static_cast<float>(d.box.width), static_cast<float>(d.box.height));
        o.label = d.classId;
        o.prob  = d.score;
        objects.push_back(o);
    }

    std::vector<STrack*> lost;                              // ByteTrack fills this; we ignore it
    std::vector<STrack*> active = impl_->tracker.update(objects, lost);

    std::vector<TrackedObject> out;
    out.reserve(active.size());
    for (STrack* s : active) {
        if (!s) continue;
        TrackedObject t;
        t.trackId = s->track_id;
        t.labelId = s->label_track;     // stable per-vehicle id used for counting
        // STrack::tlwh is [x, y, w, h] in pixels.
        if (s->tlwh.size() >= 4)
            t.box = cv::Rect(cv::Point(static_cast<int>(s->tlwh[0]), static_cast<int>(s->tlwh[1])),
                             cv::Size(static_cast<int>(s->tlwh[2]), static_cast<int>(s->tlwh[3])));
        t.score   = s->score;
        t.classId = s->label;
        out.push_back(t);
    }
    return out;
}

} // namespace lpr
