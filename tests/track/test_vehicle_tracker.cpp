#include "lpr/track/VehicleTracker.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace lpr;

// A single vehicle drifting a few pixels per frame should keep ONE stable track id.
int main() {
    VehicleTracker tracker(/*frameRate*/30, /*trackBuffer*/30);

    std::vector<int> idsLast;
    int lastId = -1, lastCount = 0;
    for (int f = 0; f < 6; ++f) {
        TrackInput in;
        in.box = cv::Rect(100 + 2 * f, 100 + 2 * f, 40, 40);   // moves slowly, constant size
        in.score = 0.9f;                                       // > high_thresh
        in.classId = 1;
        auto tracked = tracker.update({ in });
        lastCount = static_cast<int>(tracked.size());
        if (!tracked.empty()) {
            lastId = tracked[0].trackId;
            if (f >= 3) idsLast.push_back(tracked[0].trackId);   // record the settled frames
        }
        std::cout << "frame " << f << ": tracked=" << tracked.size()
                  << (tracked.empty() ? "" : (" id=" + std::to_string(tracked[0].trackId))) << "\n";
    }

    assert(lastCount == 1);                 // exactly one vehicle at the end
    assert(lastId > 0);                     // a real id was assigned
    assert(idsLast.size() >= 3);            // settled for the last frames
    for (int id : idsLast) assert(id == lastId);   // and it never changed

    std::cout << "vehicle_tracker: OK (stable id=" << lastId << ")\n";
    return 0;
}
