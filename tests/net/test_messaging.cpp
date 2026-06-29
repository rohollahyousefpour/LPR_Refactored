#include "lpr/net/PlateSender.h"
#include "lpr/net/CameraStatusNotifier.h"
#include "lpr/net/MediaSender.h"
#include "lpr/net/HeartbeatMonitor.h"
#include "lpr/net/InMemoryTransport.h"
#include "lpr/services/LiveViewService.h"
#include "lpr/services/RecordingService.h"
#include "../lpr_check.hpp"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace lpr;
using json = nlohmann::json;

static PlateItem mkPlate(const std::string& text, const std::string& gate, int trackId) {
    PlateItem it;
    it.image = cv::Mat::zeros(48, 64, CV_8UC3);
    it.gate = gate; it.timestamp = 1000;
    it.plate.text = text; it.plate.confidence = 0.9f; it.plate.trackId = trackId;
    it.plate.plateImage = cv::Mat::zeros(12, 32, CV_8UC3);
    it.plate.box = cv::RotatedRect(cv::Point2f(30, 20), cv::Size2f(40, 16), 0.f);
    return it;
}

int main() {
    // ---- PlateSender.buildMessage: correct plates_data structure ----
    InMemoryTransport t;
    PlateSender::Config cfg;
    cfg.imageEncoding = PlateSender::ImageEncoding::Base64;   // compact for the test
    cfg.cooldownMs = 0;                                       // disable for the builder check
    PlateSender sender(t, cfg);

    const std::string doc = sender.buildMessage(mkPlate("12A34567", "3", 7));
    json j = json::parse(doc);
    LPR_CHECK(j["messageType"] == "plates_data");
    LPR_CHECK(j["messageBody"]["camera_id"] == "3");
    LPR_CHECK(j["messageBody"]["cars"].is_array() && j["messageBody"]["cars"].size() == 1);
    LPR_CHECK(j["messageBody"]["cars"][0]["plate"]["plate"] == "12A34567");
    LPR_CHECK(j["messageBody"]["cars"][0]["plate"]["track_id"] == 7);
    LPR_CHECK(j["messageBody"].contains("full_image"));

    // ---- async send + durable publish ----
    sender.start();
    sender.send(mkPlate("12A34567", "3", 7));
    sender.send(mkPlate("99B88777", "3", 8));
    for (int i = 0; i < 200 && sender.sentCount() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sender.stop();
    LPR_CHECK(sender.sentCount() == 2);
    LPR_CHECK(t.count() == 2);
    LPR_CHECK(t.messages()[0].durable == true);                 // cfg.durable default
    LPR_CHECK(t.messages()[0].subject == "messages.plates_data");
    std::cout << "sent " << sender.sentCount() << " plate messages\n";

    // ---- cooldown dedup: same gate:plate within window is suppressed ----
    InMemoryTransport t2;
    PlateSender::Config cd; cd.cooldownMs = 60000;
    PlateSender sender2(t2, cd);
    sender2.start();
    sender2.send(mkPlate("AA11111", "1", 1));   // wrong format but cooldown is on text, not validity
    sender2.send(mkPlate("AA11111", "1", 1));   // duplicate within window -> suppressed
    for (int i = 0; i < 200 && sender2.sentCount() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sender2.stop();
    LPR_CHECK(sender2.sentCount() == 1);        // only the first got through
    LPR_CHECK(t2.count() == 1);

    // ---- CameraStatusNotifier: connect/disconnect messages + change-dedup ----
    InMemoryTransport tc;
    CameraStatusNotifier notifier(tc);
    auto cb = notifier.asCallback();            // == CameraManager::setStatusCallback type

    cb("7", true);                              // connected
    cb("7", true);                              // no change -> suppressed
    cb("7", false);                             // DISCONNECTED -> sent
    LPR_CHECK(tc.count() == 2);

    json c0 = json::parse(tc.messages()[0].payload);
    json c1 = json::parse(tc.messages()[1].payload);
    LPR_CHECK(c0["messageType"] == "camera_connection");
    LPR_CHECK(c0["messageBody"]["camera_id"] == "7");
    LPR_CHECK(c0["messageBody"]["Connection"] == true);
    LPR_CHECK(c1["messageBody"]["Connection"] == false);     // the disconnect notification
    LPR_CHECK(tc.messages()[0].subject == "socketio.camera_connection");
    std::cout << "camera_connection messages: " << tc.count() << "\n";

    // ---- MediaSender: live + recording messages, and wiring to the services ----
    InMemoryTransport tm;
    MediaSender media(tm);

    // direct live/recording builders
    json lj = json::parse(media.buildLiveMessage("4", cv::Mat::zeros(80, 120, CV_8UC3)));
    LPR_CHECK(lj["messageType"] == "live");
    LPR_CHECK(lj["messageBody"]["camera_id"] == "4");
    LPR_CHECK(lj["messageBody"].contains("live_image"));

    json rj = json::parse(media.buildRecordingMessage("4", "/vid/seg0.mp4", true, cv::Mat()));
    LPR_CHECK(rj["messageType"] == "recording");
    LPR_CHECK(rj["messageBody"]["video_address"] == "/vid/seg0.mp4");
    LPR_CHECK(rj["messageBody"]["end_recording"] == true);

    // live_manual_control: ONE message with both sensors, each with serial + exposure/gain
    std::vector<MediaSender::ManualLiveCam> cams = {
        { "40012345", "rgb",  cv::Mat::zeros(80, 120, CV_8UC3), 5000.0, 6.0 },
        { "40012346", "mono", cv::Mat::zeros(80, 120, CV_8UC3), 1000.0, -1.0 }   // gain unknown -> omitted
    };
    json mj = json::parse(media.buildManualLiveMessage("1", cams));
    LPR_CHECK(mj["messageType"] == "live_manual_control");
    LPR_CHECK(mj["messageBody"]["camera_id"] == "1");
    LPR_CHECK(mj["messageBody"]["cameras"].size() == 2);
    LPR_CHECK(mj["messageBody"]["cameras"][0]["camera_serial"] == "40012345");
    LPR_CHECK(mj["messageBody"]["cameras"][0]["role"] == "rgb");
    LPR_CHECK(mj["messageBody"]["cameras"][0]["exposure_us"] == 5000.0);
    LPR_CHECK(mj["messageBody"]["cameras"][0]["gain"] == 6.0);
    LPR_CHECK(mj["messageBody"]["cameras"][0].contains("live_image"));
    LPR_CHECK(mj["messageBody"]["cameras"][1]["role"] == "mono");
    LPR_CHECK(!mj["messageBody"]["cameras"][1].contains("gain"));   // -1 omitted

    // crud_image: full first frame the operator draws the ROI polygon on
    json cj = json::parse(media.buildCrudeMessage("2", cv::Mat::zeros(60, 90, CV_8UC3)));
    LPR_CHECK(cj["messageType"] == "crud_image");
    LPR_CHECK(cj["messageBody"]["camera_id"] == "2");
    LPR_CHECK(cj["messageBody"].contains("crud_image"));

    // screenshot: on-demand current frame for a camera
    json sj = json::parse(media.buildScreenshotMessage("3", cv::Mat::zeros(48, 64, CV_8UC3)));
    LPR_CHECK(sj["messageType"] == "screenshot");
    LPR_CHECK(sj["messageBody"]["camera_id"] == "3");
    LPR_CHECK(sj["messageBody"]["width"] == 64);
    LPR_CHECK(sj["messageBody"]["height"] == 48);
    LPR_CHECK(sj["messageBody"].contains("screenshot"));

    // LiveViewService -> MediaSender -> transport (the live frame path now reaches NATS)
    LiveViewService liveSvc(LiveViewService::Config{1});   // send every frame
    liveSvc.setLiveSink(media.liveSink());
    liveSvc.enableLive("4", 0);
    liveSvc.onFrame("4", cv::Mat::zeros(80, 120, CV_8UC3));
    LPR_CHECK(tm.count() == 1);
    LPR_CHECK(tm.messages()[0].subject == "socketio.live");

    // RecordingService -> MediaSender -> transport (segment close now reaches NATS)
    RecordingService::Config rcfg;
    rcfg.baseDir = "/tmp/lpr_rec_msg"; rcfg.fourcc = "MJPG"; rcfg.extension = ".avi";
    rcfg.frameSize = cv::Size(80, 60); rcfg.timestampOverlay = false;
    RecordingService recSvc(rcfg);
    recSvc.setSegmentCompleteCallback(media.recordingCallback());
    recSvc.startRecording("4", 0);
    recSvc.onFrame("4", cv::Mat::zeros(60, 80, CV_8UC3));
    recSvc.stopRecording("4");                              // closes the segment -> recording msg
    LPR_CHECK(tm.count() == 2);
    LPR_CHECK(tm.messages()[1].subject == "message.recording.4");
    json rmsg = json::parse(tm.messages()[1].payload);
    LPR_CHECK(rmsg["messageType"] == "recording" && rmsg["messageBody"]["end_recording"] == true);
    std::cout << "media messages (live+recording): " << tm.count() << "\n";

    // ---- bootstrap flow: subscribe + backend pushes settings -> handler runs ----
    InMemoryTransport tb;
    std::string gotSubject, gotPayload;
    bool subscribed = tb.subscribe("lpr.settings.token123", [&](const std::string& s, const std::string& p) {
        gotSubject = s; gotPayload = p;
    });
    LPR_CHECK(subscribed);
    tb.deliver("lpr.settings.token123", R"({"lpr":{"car_detection":1}})");   // backend push
    LPR_CHECK(gotSubject == "lpr.settings.token123");
    LPR_CHECK(json::parse(gotPayload)["lpr"]["car_detection"] == 1);
    std::cout << "bootstrap subscribe delivered settings payload\n";

    // ---- HeartbeatMonitor: alternating heartbeat + resources ----
    InMemoryTransport th;
    HeartbeatMonitor::Config hbc;
    hbc.lprId = "cam-7"; hbc.intervalMs = 60;
    HeartbeatMonitor hb(th, hbc);
    hb.setResourceProvider([] { return ResourceStats{12.5, 40.0, 80.0}; });
    // direct builders
    LPR_CHECK(json::parse(hb.buildHeartbeatMessage())["messageType"] == "heartbeat");
    json rjs = json::parse(hb.buildResourcesMessage(ResourceStats{12.5, 40.0, 80.0}));
    LPR_CHECK(rjs["messageType"] == "resources");
    LPR_CHECK(rjs["messageBody"]["lpr_id"] == "cam-7");
    LPR_CHECK(rjs["messageBody"]["CPU_USAGE"] == "12.50");
    // running thread publishes both kinds
    hb.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    hb.stop();
    bool sawHb = false, sawRes = false;
    for (auto& m : th.messages()) {
        auto t = json::parse(m.payload)["messageType"];
        if (t == "heartbeat") sawHb = true;
        if (t == "resources") sawRes = true;
    }
    LPR_CHECK(sawHb && sawRes);
    std::cout << "heartbeat messages: " << th.count() << "\n";

    if (LPR_TEST_RESULT() == 0) std::cout << "messaging: OK\n";
    return LPR_TEST_RESULT();
}
