// test_detection_interleave
// -------------------------
// Regression: two vehicles interleaving at one gate must BOTH be recorded.
//
// The DetectionWorker keeps a SINGLE gate-keyed pass state and, with announce-hold on,
// STASHES the first vehicle's plate (pendingItem) until its direction settles. The bug:
// when a DIFFERENT vehicle's plate arrived on the KEPT path while the first was still
// held, the stash was overwritten (`st.pendingItem = out`) and the first vehicle was
// read but never emitted. The fix flushes the held plate before stashing the new one.
//
// Setup: announce-hold on, constant plate size (direction never settles, so the hold
// stays engaged), and a de-dup processor stand-in (emit each distinct plate once) so the
// held vehicle is released via hold-expiry on the de-dup path — exactly as in production.

#include "lpr/manager/DetectionWorker.h"
#include "lpr/detect/IPlateRecognizer.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace lpr;

// One scripted plate per non-empty frame; constant box size so the direction estimator
// never settles and the announce-hold stays engaged.
class SeqRecognizer : public IPlateRecognizer {
public:
    std::vector<std::string> seq;
    size_t i = 0;
    std::vector<PlateResult> recognize(const cv::Mat& frame, const std::string& gate, long ts) override {
        if (frame.empty() || i >= seq.size()) return {};
        PlateResult r;
        r.text = seq[i++]; r.confidence = 0.95f; r.gate = gate; r.timestamp = ts;
        r.box = cv::RotatedRect(cv::Point2f(100, 100), cv::Size2f(40, 15), 0);
        return { r };
    }
};

int main() {
    auto frames = std::make_shared<FrameQueue>(100);
    auto plates = std::make_shared<PlateQueue>(100);

    SeqRecognizer rec;
    // Vehicle A once, then vehicle B (different plate) repeatedly. A is held when B arrives.
    rec.seq = {"aaa11111", "bbb22222", "bbb22222", "bbb22222", "bbb22222"};

    DetectionWorker::Config cfg{};
    cfg.popTimeoutMs = 50;
    cfg.directionEnable = true;
    cfg.direction.announceHoldSightings = 2;   // hold the first announce until direction settles

    DetectionWorker worker(frames, rec, cfg);
    worker.setPlateQueue(plates);

    // De-dup processor stand-in: emit each distinct plate once, drop repeats (like a real pass).
    std::set<std::string> seen;
    worker.setPlateProcessor([&](PlateResult&& p) -> std::optional<PlateResult> {
        if (!seen.insert(p.text).second) return std::nullopt;   // repeat -> de-dup
        return std::optional<PlateResult>(std::move(p));
    });

    worker.start();
    for (int i = 0; i < 5; ++i) {
        FrameItem f; f.image = cv::Mat::zeros(8, 8, CV_8UC3); f.gate = "1"; f.timestamp = i;
        frames->push(std::move(f));
    }
    for (int i = 0; i < 400 && worker.framesProcessed() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));   // let the expiry-release frame emit
    worker.stop();

    std::vector<std::string> got;
    while (plates->size() > 0) { auto p = plates->pop(); if (p) got.push_back(p->plate.text); }

    std::cout << "interleave: emitted=" << got.size();
    for (auto& t : got) std::cout << " '" << t << "'";
    std::cout << "\n";

    bool hasA = false, hasB = false;
    for (auto& t : got) { if (t == "aaa11111") hasA = true; if (t == "bbb22222") hasB = true; }

    // Explicit checks (NOT assert — asserts are compiled out in the release preset).
    // The bug lost vehicle A (held announce overwritten by B) -> only B recorded.
    // The fix flushes A before stashing B -> BOTH recorded.
    if (!hasA) { std::cerr << "FAIL: vehicle A (held) was LOST when vehicle B interleaved\n"; return 1; }
    if (!hasB) { std::cerr << "FAIL: vehicle B was not recorded\n"; return 1; }

    std::cout << "detection_interleave: OK\n";
    return 0;
}
