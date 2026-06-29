#include "lpr/detect/VehicleDetector.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace lpr;

// One 1x1 grid head, 1 anchor, 5 classes -> 10 values per cell.
// Logits chosen so every sigmoid is predictable:
//   tx=ty=tw=th=0 -> sigmoid=0.5 ; obj=2 -> ~0.881 ; class0=2, others=-5.
// With width=height=64, anchor={32,32}, scaleX=scaleY=1, the decode must yield
// box (16,16,32,32), class 0. (Hand-derived from the YOLOv5 formulas.)
int main() {
    VehicleConfig cfg;
    cfg.width = 64; cfg.height = 64;
    cfg.numClasses = 5;
    cfg.scoreThreshold = 0.45f;
    cfg.anchors = { {32, 32} };
    cfg.anchorMasks = { {0} };

    std::vector<float> data(10, 0.f);
    data[0] = 0.f; data[1] = 0.f; data[2] = 0.f; data[3] = 0.f;   // tx,ty,tw,th
    data[4] = 2.f;                                                // objectness
    data[5] = 2.f; data[6] = -5.f; data[7] = -5.f; data[8] = -5.f; data[9] = -5.f; // classes

    YoloHead head;
    head.data = data.data();
    head.gridH = 1; head.gridW = 1;
    head.anchorMask = {0};

    auto dets = decodeYolo({head}, cfg, /*scaleX*/1.f, /*scaleY*/1.f);

    std::cout << "decoded " << dets.size() << " detection(s)\n";
    assert(dets.size() == 1);
    const auto& d = dets[0];
    std::cout << "box=(" << d.box.x << "," << d.box.y << "," << d.box.width << "," << d.box.height
              << ") class=" << d.classId << " score=" << d.score << "\n";
    assert(d.box.x == 16 && d.box.y == 16);
    assert(d.box.width == 32 && d.box.height == 32);
    assert(d.classId == 0);
    assert(d.score > 0.7f && d.score <= 1.0f);

    // Below-threshold objectness must produce nothing.
    data[4] = -5.f;
    auto none = decodeYolo({head}, cfg, 1.f, 1.f);
    assert(none.empty());

    std::cout << "vehicle_decode: OK\n";
    return 0;
}
