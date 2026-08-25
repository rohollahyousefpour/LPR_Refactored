#include "lpr/services/RecordingService.h"
#include "lpr/services/LiveViewService.h"
#include "lpr/services/CommandRouter.h"
#include "../lpr_check.hpp"

#include <opencv2/core.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace lpr;
using json = nlohmann::json;

static json camMsg(const std::string& id, int duration) {
    json j;
    j["messageBody"]["data"]["cameraId"] = id;
    j["messageBody"]["data"]["duration"] = duration;
    return j;
}

int main() {
    cv::Mat frame = cv::Mat::zeros(120, 160, CV_8UC3);

    // ---- LiveViewService: enable, frame-skip, expiry, sink ----
    LiveViewService::Config lvc; lvc.sendEveryN = 2;
    LiveViewService live(lvc);
    int sent = 0;
    live.setLiveSink([&](const std::string&, const cv::Mat&) { ++sent; });

    live.onFrame("1", frame);                 // gate not live -> ignored
    LPR_CHECK(sent == 0);

    live.enableLive("1", /*durationSeconds*/0);
    LPR_CHECK(live.isLive("1"));
    live.onFrame("1", frame);                 // counter 1 -> skipped (every 2nd)
    live.onFrame("1", frame);                 // counter 2 -> sent
    live.onFrame("1", frame);                 // counter 3 -> skipped
    live.onFrame("1", frame);                 // counter 4 -> sent
    LPR_CHECK(sent == 2);
    live.disableLive("1");
    LPR_CHECK(!live.isLive("1"));

    // expiry: very short live window, then a frame after it expires
    LiveViewService live2(LiveViewService::Config{1});
    int sent2 = 0;
    live2.setLiveSink([&](const std::string&, const cv::Mat&) { ++sent2; });
    live2.enableLive("9", 1);                  // 1 second
    live2.onFrame("9", frame);                 // sent (counter 1 % 1 == 0)
    LPR_CHECK(sent2 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    live2.onFrame("9", frame);                 // expired -> not sent, gate removed
    LPR_CHECK(sent2 == 1);
    LPR_CHECK(!live2.isLive("9"));

    // keep-longest (concurrent viewers): a later SHORTER request must NOT shorten
    // an already-active stream, and a later LONGER request extends it. Two people
    // requesting live with different durations => union of deadlines.
    LiveViewService live4(LiveViewService::Config{1});
    live4.enableLive("kl", 100);               // viewer A: long window
    live4.enableLive("kl", 1);                 // viewer B: short — must NOT win
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    LPR_CHECK(live4.isLive("kl"));             // still live thanks to A's 100s
    // reverse order: short first, then a longer request extends it
    live4.enableLive("kl2", 1);
    live4.enableLive("kl2", 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    LPR_CHECK(live4.isLive("kl2"));

    // ---- RecordingService: state machine + segment-complete callback ----
    RecordingService::Config rc;
    rc.baseDir = "/tmp/lpr_rec_test";
    rc.fourcc = "MJPG"; rc.extension = ".avi";   // widely available in sandbox OpenCV
    rc.frameSize = cv::Size(160, 120);
    rc.timestampOverlay = false;
    RecordingService rec(rc);

    int segDone = 0;
    std::string lastPath;
    rec.setSegmentCompleteCallback([&](const std::string&, const std::string& p) { ++segDone; lastPath = p; });

    rec.onFrame("1", frame);                   // not recording -> ignored
    LPR_CHECK(!rec.isRecording("1"));

    LPR_CHECK(rec.startRecording("1", /*duration*/0));
    LPR_CHECK(rec.isRecording("1"));
    LPR_CHECK(!rec.startRecording("1"));       // already recording -> false
    for (int i = 0; i < 5; ++i) rec.onFrame("1", frame);
    rec.onFrame("2", frame);                   // gate 2 not recording -> ignored
    LPR_CHECK(rec.activeCount() == 1);
    rec.stopRecording("1");
    LPR_CHECK(!rec.isRecording("1"));
    LPR_CHECK(segDone == 1);                   // closing the segment fired the callback
    std::cout << "recorded segment: " << lastPath << "\n";

    // ---- CommandRouter: dispatch to services + injected setting handlers ----
    RecordingService rec2(rc);
    LiveViewService  live3;
    CommandRouter router(rec2, live3);

    router.liveView(camMsg("5", 30));
    LPR_CHECK(live3.isLive("5"));

    router.startRecording(camMsg("5", 0));
    LPR_CHECK(rec2.isRecording("5"));
    router.stopRecording("5");
    LPR_CHECK(!rec2.isRecording("5"));

    int gotSetting = 0, gotConfig = 0;
    router.setGetSettingHandler([&](const json&) { ++gotSetting; });
    router.setSetCameraConfigHandler([&](const json&) { ++gotConfig; });
    router.getSetting(json::object());
    router.setCameraConfig(json::object());
    LPR_CHECK(gotSetting == 1 && gotConfig == 1);

    // duration clamping: liveView min is 2s even if asked for 0
    LPR_CHECK(CommandRouter::durationOf(camMsg("5", 0), 2, 2, 1200) == 2);
    // any long duration the operator picks is honoured now (ceiling raised to 24h);
    // a 2-hour request passes through instead of being truncated to the old 20-min cap.
    LPR_CHECK(CommandRouter::durationOf(camMsg("5", 7200), 2, 2, 86400) == 7200);
    LPR_CHECK(CommandRouter::cameraIdOf(camMsg("42", 5)) == "42");

    // ---- RecordingService watchdog: a timed recording closes even if frames stop ----
    RecordingService::Config wc;
    wc.baseDir = "/tmp/lpr_rec_wd"; wc.fourcc = "MJPG"; wc.extension = ".avi";
    wc.frameSize = cv::Size(80, 60); wc.timestampOverlay = false; wc.watchdogMs = 100;
    RecordingService recWd(wc);
    int wdSeg = 0;
    recWd.setSegmentCompleteCallback([&](const std::string&, const std::string&) { ++wdSeg; });
    recWd.startRecording("8", /*duration*/1);   // 1 second
    recWd.onFrame("8", frame);                   // opens a segment
    LPR_CHECK(recWd.isRecording("8"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));   // stop sending frames
    LPR_CHECK(!recWd.isRecording("8"));          // watchdog closed it without any more frames
    LPR_CHECK(wdSeg == 1);                        // and fired the segment-complete callback
    std::cout << "watchdog closed timed recording\n";

    if (LPR_TEST_RESULT() == 0) std::cout << "services: OK\n";
    return LPR_TEST_RESULT();
}
