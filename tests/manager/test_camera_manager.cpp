// Exercises CameraWorker reconnect + CameraManager multi-camera fan-in using mock
// sources (no real cameras). Needs OpenCV (FrameItem holds cv::Mat).
#include "lpr/manager/CameraManager.h"
#include "lpr/capture/CaptureSource.h"
#include "lpr/capture/FrameQueue.h"

#include <opencv2/core.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)

// Emits a few frames then errors out -> drives the reconnect path.
class FlakySource : public lpr::CaptureSource {
public:
    void setAddress(const std::string&, int) override {}
    void run() override {
        for (int i = 0; i < 3 && !stop_; ++i)
            emitFrame(cv::Mat::zeros(2, 2, CV_8UC3), cv::Mat(), i);
        if (!stop_) emitError();             // simulate disconnect
    }
    void stop() override { stop_ = true; }
    bool isLive() const override { return !stop_; }
private:
    std::atomic<bool> stop_{false};
};

static lpr::MotionConfig fastCfg() {
    lpr::MotionConfig c;
    c.enableMotionGate = false;   // forward every frame for easy counting
    c.delayMs          = 0;
    c.reconnectBaseMs  = 5;
    c.reconnectMaxMs   = 10;
    return c;
}

void test_worker_reconnect() {
    std::cout << "test_worker_reconnect\n";
    auto queue = std::make_shared<lpr::FrameQueue>(1000);
    std::atomic<int> attempts{0};
    std::atomic<int> disconnects{0};

    lpr::CameraWorker worker("7",
        [&]() -> std::unique_ptr<lpr::CaptureSource> { ++attempts; return std::make_unique<FlakySource>(); },
        fastCfg());
    worker.setFrameSink([&](lpr::FrameItem&& it){ queue->push(std::move(it)); });
    worker.setStatusCallback([&](const std::string&, bool up){ if(!up) ++disconnects; });

    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));   // several connect/error cycles
    worker.stop();

    int frames = (int)queue->size();
    CHECK(attempts.load() >= 2);     // reconnected at least once
    CHECK(disconnects.load() >= 1);  // reported disconnects
    CHECK(frames > 3);               // more than a single session's worth
    // all frames carry the right gate
    bool gateOk = true; lpr::FrameItem it;
    while (queue->tryPop(it)) { if (it.gate != "7") gateOk = false; }
    CHECK(gateOk);
}

void test_manager_multicamera() {
    std::cout << "test_manager_multicamera\n";
    auto queue = std::make_shared<lpr::FrameQueue>(2000);
    lpr::CameraManager mgr;
    mgr.setOutputQueue(queue);

    for (const char* g : {"1", "2"})
        mgr.addCamera(g, []() -> std::unique_ptr<lpr::CaptureSource> { return std::make_unique<FlakySource>(); }, fastCfg());
    CHECK(mgr.cameraCount() == 2);

    mgr.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    mgr.stop();

    bool saw1=false, saw2=false; lpr::FrameItem it;
    while (queue->tryPop(it)) { if(it.gate=="1") saw1=true; if(it.gate=="2") saw2=true; }
    CHECK(saw1 && saw2);             // frames from both cameras
}

// Ends cleanly (no emitError) -> worker must stop itself and NOT reconnect.
class CleanEndSource : public lpr::CaptureSource {
public:
    void setAddress(const std::string&, int) override {}
    void run() override { for (int i = 0; i < 3 && !stop_; ++i) emitFrame(cv::Mat::zeros(2,2,CV_8UC3), cv::Mat(), i); }
    void stop() override { stop_ = true; }
    bool isLive() const override { return !stop_; }
private:
    std::atomic<bool> stop_{false};
};

void test_clean_end_stops() {
    std::cout << "test_clean_end_stops\n";
    auto queue = std::make_shared<lpr::FrameQueue>(100);
    lpr::CameraWorker worker("9",
        []() -> std::unique_ptr<lpr::CaptureSource> { return std::make_unique<CleanEndSource>(); },
        fastCfg());
    worker.setFrameSink([&](lpr::FrameItem&& it){ queue->push(std::move(it)); });
    worker.start();
    for (int i = 0; i < 200 && worker.isRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!worker.isRunning());          // self-stopped on clean end
    CHECK(queue->size() <= 3);           // one session only -> no reconnect replay
    worker.stop();                       // must be safe after self-stop (no terminate)
}

// Emits ONE distinguishable (first, second) frame pair repeatedly, then idles
// until stop — so the routing of a mono+RGB pair can be asserted deterministically.
class MockPairSource : public lpr::CaptureSource {
public:
    MockPairSource(cv::Mat first, cv::Mat second) : a_(std::move(first)), b_(std::move(second)) {}
    void setAddress(const std::string&, int) override {}
    void run() override {
        for (int i = 0; i < 3 && !stop_; ++i) {
            emitFrame(a_, b_, i);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(2));   // stay "live"
    }
    void stop() override { stop_ = true; }
    bool isLive() const override { return !stop_; }
private:
    cv::Mat a_, b_;
    std::atomic<bool> stop_{false};
};

// The live-COLOR / plate-MONO policy (CameraWorker::onFrame): whatever the wiring,
// the LIVE view must carry the COLOR stream while DETECTION and the plate crop run
// on the MONO stream, and the evidence/full_image is COLOR. Proven for BOTH real
// pair configs, which arrive with opposite frame ordering:
//   * Basler: the facade delivers the pair MONO-FIRST -> onFrame(frame=mono, mono=color),
//     detectOnSecondary=false.
//   * IP:     DualCaptureSource delivers main=RGB, mono=mono -> onFrame(frame=color, mono=mono),
//     detectOnSecondary=true.
void test_color_mono_routing() {
    std::cout << "test_color_mono_routing\n";
    const cv::Mat mono(4, 4, CV_8UC1, cv::Scalar(70));         // MONO stream: 1 channel
    const cv::Mat color(4, 4, CV_8UC3, cv::Scalar(10, 20, 30)); // COLOR stream: 3 channels

    auto runCase = [](bool detectOnSecondary, cv::Mat first, cv::Mat second,
                      cv::Mat& live, cv::Mat& detect, cv::Mat& evidence) {
        lpr::MotionConfig c = fastCfg();
        c.detectOnSecondary = detectOnSecondary;   // forward every frame (gate off)
        std::atomic<bool> gotLive{false}, gotSink{false};
        lpr::CameraWorker worker("5",
            [&]() -> std::unique_ptr<lpr::CaptureSource> {
                return std::make_unique<MockPairSource>(first, second);
            }, c);
        worker.setRawFrameObserver([&](lpr::FrameItem&& it){ live = it.image; gotLive = true; });
        worker.setFrameSink([&](lpr::FrameItem&& it){ detect = it.image; evidence = it.evidenceImage; gotSink = true; });
        worker.start();
        for (int i = 0; i < 100 && !(gotLive && gotSink); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        worker.stop();
    };

    // Basler pair: onFrame(frame=mono, mono=color), detectOnSecondary=false.
    cv::Mat live, det, evi;
    runCase(false, mono, color, live, det, evi);
    CHECK(!live.empty() && live.channels() == 3);   // live view  = COLOR
    CHECK(!det.empty()  && det.channels()  == 1);   // plate/detect = MONO
    CHECK(!evi.empty()  && evi.channels()  == 3);   // full_image   = COLOR

    // IP pair: onFrame(frame=color, mono=mono), detectOnSecondary=true.
    cv::Mat live2, det2, evi2;
    runCase(true, color, mono, live2, det2, evi2);
    CHECK(!live2.empty() && live2.channels() == 3); // live view  = COLOR
    CHECK(!det2.empty()  && det2.channels()  == 1); // plate/detect = MONO
    CHECK(!evi2.empty()  && evi2.channels()  == 3); // full_image   = COLOR
}

int main(){
    test_worker_reconnect();
    test_manager_multicamera();
    test_clean_end_stops();
    test_color_mono_routing();
    if(fails==0){ std::cout<<"camera_manager: ALL TESTS PASSED\n"; return 0; }
    std::cout<<fails<<" failed\n"; return 1;
}
