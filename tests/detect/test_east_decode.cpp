#include "lpr/detect/EastTextDetector.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    // 8x8 feature map, one active cell at (x=4, y=3). angle=0, unit scales so geometry
    // values pass through. d0=d2=10 -> h=20; d1=d3=30 -> w=60. With angle=0 the decoded
    // center lands on (x*stride, y*stride).
    const int Hf = 8, Wf = 8;
    lpr::EastConfig cfg;
    cfg.stride = 4; cfg.scoreThresh = 0.5f; cfg.nmsThresh = 0.4f;
    cfg.heightScale = 1.f; cfg.widthScale1 = 1.f; cfg.widthScale2 = 1.f;

    std::vector<float> scores(Hf * Wf, 0.f);
    std::vector<float> geo(Hf * Wf * 5, 0.f);
    const int ax = 4, ay = 3;
    scores[ay * Wf + ax] = 0.9f;
    int gi = (ay * Wf + ax) * 5;
    geo[gi+0]=10.f; geo[gi+1]=30.f; geo[gi+2]=10.f; geo[gi+3]=30.f; geo[gi+4]=0.f;

    auto boxes = lpr::decodeEast(scores.data(), geo.data(), Hf, Wf, cfg);
    std::cout << "boxes=" << boxes.size();
    assert(boxes.size() == 1);
    const auto& b = boxes[0];
    std::cout << " center=(" << b.center.x << "," << b.center.y << ")"
              << " size=(" << b.size.width << "x" << b.size.height << ")\n";
    assert(std::abs(b.center.x - ax * cfg.stride) < 1e-3);   // 16
    assert(std::abs(b.center.y - ay * cfg.stride) < 1e-3);   // 12
    assert(std::abs(b.size.width  - 60.f) < 1e-3);
    assert(std::abs(b.size.height - 20.f) < 1e-3);

    // below-threshold map -> no boxes
    std::vector<float> low(Hf * Wf, 0.1f);
    assert(lpr::decodeEast(low.data(), geo.data(), Hf, Wf, cfg).empty());

    std::cout << "east_decode: OK\n";
    return 0;
}
