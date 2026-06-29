#include "lpr/capture/DualCaptureSource.h"
#include "../lpr_check.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace lpr;

// Fake source that repeatedly emits a 4x4 frame filled with `value` until stopped.
class FakeSource : public CaptureSource {
public:
    explicit FakeSource(int value) : value_(value) {}
    void setAddress(const std::string&, int) override {}
    void setMonoAddress(const std::string&, int) override {}
    bool isLive() const override { return true; }
    void run() override {
        run_ = true;
        while (run_) {
            cv::Mat m(4, 4, CV_8UC1, cv::Scalar(value_));
            emitFrame(m, cv::Mat(), 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    void stop() override { run_ = false; }
private:
    int value_;
    std::atomic<bool> run_{false};
};

int main() {
    auto main = std::make_unique<FakeSource>(100);   // RGB / main
    auto mono = std::make_unique<FakeSource>(200);   // mono
    DualCaptureSource dual(std::move(main), std::move(mono));

    std::atomic<int> paired{0}, total{0};
    dual.onFrame([&](const cv::Mat& f, const cv::Mat& m, long) {
        ++total;
        if (!f.empty() && f.at<uchar>(0, 0) == 100 &&     // main frame
            !m.empty() && m.at<uchar>(0, 0) == 200)        // paired with latest mono
            ++paired;
    });

    std::thread t([&] { dual.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    dual.stop();
    t.join();

    std::cout << "dual: total=" << total << " paired_main+mono=" << paired << "\n";
    LPR_CHECK(total > 0);
    LPR_CHECK(paired > 0);     // main frames were delivered together with the mono frame

    if (LPR_TEST_RESULT() == 0) std::cout << "dual_capture: OK\n";
    return LPR_TEST_RESULT();
}
