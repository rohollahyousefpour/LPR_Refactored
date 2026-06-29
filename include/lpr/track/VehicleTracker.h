#pragma once
// VehicleTracker - a clean, dependency-light front for the vendored ByteTrack
// multi-object tracker (third_party algorithm under lpr/track/bytetrack/). It is the
// clean replacement for the original Managment_Cameras' per-gate BYTETracker map +
// process_tracker_car bookkeeping. ByteTrack's own types (STrack, Object, Eigen,
// byte_kalman) are kept entirely inside the .cpp via PIMPL, so the rest of the
// codebase only ever sees lpr::TrackedObject and never pulls in Eigen.
//
// One VehicleTracker instance tracks one stream/gate. The VehicleAwarePlateRecognizer
// holds one per gate, mirroring the original design.
#include <memory>
#include <vector>
#include <opencv2/core.hpp>

namespace lpr {

// A detection fed into the tracker (what the VehicleDetector produces).
struct TrackInput {
    cv::Rect box;          // pixel box in the full frame
    float    score = 0.f;
    int      classId = 0;
};

// A tracked object returned by the tracker - now carries a stable id across frames.
struct TrackedObject {
    int      trackId = -1;   // ByteTrack track_id (reassigned on re-activation; for association)
    int      labelId = -1;   // label_track: assigned once per confirmed track, never reused
                             // (this is what the original counts with -> stable vehicle count)
    cv::Rect box;            // smoothed/predicted pixel box
    float    score = 0.f;
    int      classId = 0;
};

class VehicleTracker {
public:
    // frameRate / trackBuffer match ByteTracker(frame_rate, track_buffer); the buffer
    // controls how long a lost track is kept before removal.
    explicit VehicleTracker(int frameRate = 30, int trackBuffer = 30);
    ~VehicleTracker();
    VehicleTracker(VehicleTracker&&) noexcept;
    VehicleTracker& operator=(VehicleTracker&&) noexcept;
    VehicleTracker(const VehicleTracker&) = delete;
    VehicleTracker& operator=(const VehicleTracker&) = delete;

    // Advance one frame: associate the given detections with existing tracks and
    // return the currently active tracked objects (with stable trackIds).
    std::vector<TrackedObject> update(const std::vector<TrackInput>& detections);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lpr
