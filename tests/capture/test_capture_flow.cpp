// Proves the SDK-free capture flow end-to-end with a mock source:
// CaptureSource.onFrame -> FrameItem -> FrameQueue -> consumer. Needs OpenCV.
#include "lpr/capture/CaptureSource.h"
#include "lpr/capture/FrameQueue.h"
#include "lpr/capture/CameraSourceFactory.h"

#include <opencv2/core.hpp>
#include <atomic>
#include <iostream>

static int fails=0;
#define CHECK(c) do{ if(!(c)){ std::cerr<<"  FAIL: "<<#c<<" ("<<__LINE__<<")\n"; ++fails; } }while(0)

class MockSource : public lpr::CaptureSource {
public:
    void setAddress(const std::string&, int) override {}
    void run() override {
        for (int i = 0; i < 5 && !stop_; ++i)
            emitFrame(cv::Mat::zeros(2, 2, CV_8UC1), cv::Mat(), i);
    }
    void stop() override { stop_ = true; }
    bool isLive() const override { return !stop_; }
private:
    std::atomic<bool> stop_{false};
};

int main(){
    lpr::FrameQueue q(100);
    MockSource src;
    src.onFrame([&](const cv::Mat& f, const cv::Mat& m, long t){
        lpr::FrameItem item;
        item.image = f.clone();
        item.evidenceImage = m;
        item.timestamp = t;
        item.gate = "1";
        q.push(std::move(item));
    });

    src.run();                 // emits 5 frames through the callback into the queue
    q.close();

    int n = 0;
    while (auto item = q.pop()) {
        ++n;
        CHECK(item->image.rows == 2 && item->image.cols == 2);
        CHECK(item->gate == "1");
    }
    CHECK(n == 5);

    // factory: unknown kind always throws
    bool threw=false;
    try { lpr::CameraSourceParams p; p.typeOfLink="nope"; lpr::CameraSourceFactory::create(p); }
    catch (const lpr::CameraSourceError&) { threw=true; }
    CHECK(threw);

    // video source is always available (OpenCV); bad path still yields an object
    { lpr::CameraSourceParams p; p.typeOfLink="video"; p.address="/no/such/file.mp4";
      auto s = lpr::CameraSourceFactory::create(p); CHECK(s != nullptr); }

#ifdef LPR_WITH_VLC
    { lpr::CameraSourceParams p; p.typeOfLink="rtsp"; p.address="rtsp://example/stream";
      auto s = lpr::CameraSourceFactory::create(p); CHECK(s != nullptr); }
#endif
#ifdef LPR_WITH_GSTREAMER
    { lpr::CameraSourceParams p; p.typeOfLink="gstreamer"; p.address="rtsp://example/stream";
      auto s = lpr::CameraSourceFactory::create(p); CHECK(s != nullptr); }
#endif

    if(fails==0){ std::cout<<"capture_flow: ALL TESTS PASSED\n"; return 0; } return 1;
}
