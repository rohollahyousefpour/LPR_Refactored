#pragma once
// MediaSender - publishes the "live" and "recording" messages (port of
// Handle_Clients::send_live / send_recording). It connects RecordingService and
// LiveViewService to the broker via IMessageTransport, so those services stay free of any
// network dependency. Wire it with the adapters:
//
//   liveView.setLiveSink(media.liveSink());                       // every forwarded live frame
//   recording.setSegmentCompleteCallback(media.recordingCallback()); // each finished segment
//
// Original formats reproduced:
//   live      -> subject "socketio.live"            body {timestamp, camera_id, live_image}
//   recording -> subject "message.recording.<gate>" body {timestamp, camera_id, video_address,
//                                                          frame, end_recording}
#include "lpr/net/IMessageTransport.h"
#include "lpr/net/ImageCodec.h"

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <vector>

#include <opencv2/core.hpp>

namespace lpr {

class MediaSender {
public:
    struct Config {
        std::string   liveSubject            = "socketio.live";
        std::string   recordingSubjectPrefix = "message.recording.";   // + gate
        std::string   crudeSubject           = "message.crud";         // first-frame "crud_image"
        std::string   screenshotSubject      = "message.screenshot";   // on-demand screenshot reply
        int           liveJpegQuality        = 20;
        int           recordingJpegQuality   = 60;
        int           crudeJpegQuality       = 20;
        int           screenshotJpegQuality  = 35;
        bool          liveHalfSize           = true;   // original sent frames at half resolution
        ImageEncoding imageEncoding          = ImageEncoding::ByteArray;
        // Manual-control live carries TWO images per message, so keep it compact:
        // base64 is ~3x smaller than a JSON byte-array, and the scale divisor lets you
        // shrink further (2 = half, 4 = quarter).
        ImageEncoding manualLiveEncoding     = ImageEncoding::Base64;
        int           manualLiveJpegQuality  = 20;
        int           manualLiveScaleDiv     = 2;       // 1 = full, 2 = half, 4 = quarter
    };

    MediaSender(IMessageTransport& transport, Config cfg);
    MediaSender(IMessageTransport& transport) : MediaSender(transport, Config{}) {}

    void sendLiveFrame(const std::string& gate, const cv::Mat& img);
    void sendRecordingEvent(const std::string& gate, const std::string& videoAddress,
                            bool endRecording, const cv::Mat& frame = cv::Mat());
    // First-frame "crud_image": the full crude frame the backend uses to let the operator
    // draw the ROI polygon (plate reading region). Sent once per camera at start.
    void sendCrudeImage(const std::string& gate, const cv::Mat& frame);
    // On-demand screenshot: current frame for a camera, returned in response to a command.
    void sendScreenshot(const std::string& gate, const cv::Mat& frame);

    // One camera entry for a manual-control live message.
    struct ManualLiveCam {
        std::string serial;          // physical camera serial
        std::string role;            // "rgb" | "mono" | "single"
        cv::Mat     image;           // that camera's frame
        double      exposureUs = -1; // current exposure (us); <0 => omit
        double      gain       = -1; // current gain;        <0 => omit
    };
    // Manual-control live: ONE message ("live_manual_control") carrying every camera of a
    // gate (both sensors of a mono+RGB pair, or the single camera), each with its serial
    // and current exposure/gain.
    void sendManualLive(const std::string& gate, const std::vector<ManualLiveCam>& cams);

    // Adapters that plug into the services built earlier.
    std::function<void(const std::string&, const cv::Mat&)>   liveSink();
    std::function<void(const std::string&, const std::string&)> recordingCallback();

    // While a camera is under manual control, tag its live messages with the
    // physical camera serial so the backend knows which camera the image is for.
    // Set on a manual command; cleared on revert. Thread-safe.
    void setLiveSerial(const std::string& gate, const std::string& serial);
    void clearLiveSerial(const std::string& gate);

    // Pure builders (no I/O), exposed for testing.
    std::string buildLiveMessage(const std::string& gate, const cv::Mat& img) const;
    std::string buildManualLiveMessage(const std::string& gate,
                                       const std::vector<ManualLiveCam>& cams) const;
    std::string buildCrudeMessage(const std::string& gate, const cv::Mat& frame) const;
    std::string buildScreenshotMessage(const std::string& gate, const cv::Mat& frame) const;
    std::string buildRecordingMessage(const std::string& gate, const std::string& videoAddress,
                                      bool endRecording, const cv::Mat& frame) const;

private:
    IMessageTransport& transport_;
    Config             cfg_;
    mutable std::mutex liveSerialMtx_;
    std::map<std::string, std::string> liveSerial_;   // gate -> serial (manual control only)
};

} // namespace lpr
