#include "lpr/manager/DetectionWorker.h"
#include "lpr/detect/IPlateRecognizer.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace lpr;

// Stand-in recognizer: one plate per non-empty frame, text derived from the gate.
class FakeRecognizer : public IPlateRecognizer {
public:
    std::vector<PlateResult> recognize(const cv::Mat& frame, const std::string& gate, long ts) override {
        if (frame.empty()) return {};
        PlateResult r; r.text = "PLATE-" + gate; r.confidence = 0.9f; r.gate = gate; r.timestamp = ts;
        return { r };
    }
};

int main() {
    auto frames = std::make_shared<FrameQueue>(100);
    auto plates = std::make_shared<PlateQueue>(100);
    FakeRecognizer rec;

    DetectionWorker worker(frames, rec, DetectionWorker::Config{50});
    worker.setPlateQueue(plates);                                   // sink -> PlateQueue (PlateSender stand-in)

    // PlateProcessor stand-in: drop gate "skip" (dedup/invalid), canonicalise the rest.
    worker.setPlateProcessor([](PlateResult&& p) -> std::optional<PlateResult> {
        if (p.gate == "skip") return std::nullopt;                  // dropped
        p.text = "OK-" + p.gate;                                    // transformed (proves it can rewrite)
        return std::optional<PlateResult>(std::move(p));
    });

    // RecordingService / LiveViewService stand-in: count every (non-empty) frame.
    std::atomic<int> framesSeen{0};
    worker.addFrameObserver([&](const FrameItem&) { ++framesSeen; });

    worker.start();

    for (int i = 0; i < 3; ++i) { FrameItem f; f.image = cv::Mat::zeros(4,4,CV_8UC3); f.gate = "1"; f.timestamp = i; frames->push(std::move(f)); }
    { FrameItem f; f.image = cv::Mat();                    f.gate = "1";    frames->push(std::move(f)); } // empty -> no plate, no observer
    { FrameItem f; f.image = cv::Mat::zeros(4,4,CV_8UC3);  f.gate = "skip"; frames->push(std::move(f)); } // observed, plate dropped

    for (int i = 0; i < 400 && worker.framesProcessed() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    worker.stop();   // safe with a shared, un-closed queue (no hang / terminate)

    std::cout << "frames=" << worker.framesProcessed()
              << " observed=" << framesSeen.load()
              << " emitted=" << worker.platesEmitted()
              << " queued="  << plates->size() << "\n";

    assert(worker.framesProcessed() == 5);   // all 5 consumed (incl. empty + skip)
    assert(framesSeen.load()        == 4);   // observers run on the 4 non-empty frames
    assert(worker.platesEmitted()   == 3);   // empty -> none, skip -> dropped by processor
    assert(plates->size()           == 3);

    auto first = plates->pop();
    assert(first && first->plate.text == "OK-1" && first->gate == "1");   // processor rewrote text

    std::cout << "detection_worker: OK\n";
    return 0;
}
