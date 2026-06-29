#include "lpr/net/MediaSender.h"
#include "lpr/util/Uuid.h"
#include "lpr/util/Time.h"
#include "lpr/Log.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>

namespace lpr {

using json = nlohmann::json;

MediaSender::MediaSender(IMessageTransport& transport, Config cfg)
    : transport_(transport), cfg_(std::move(cfg)) {}

std::string MediaSender::buildLiveMessage(const std::string& gate, const cv::Mat& img) const {
    cv::Mat out = img;
    if (cfg_.liveHalfSize && !img.empty())
        cv::resize(img, out, cv::Size(img.cols / 2, img.rows / 2));
    json body = {
        {"timestamp", formatLocalTime("%Y-%m-%dT%H:%M:%S")},
        {"camera_id", gate},
        {"live_image", encodeJpeg(out, cfg_.liveJpegQuality, cfg_.imageEncoding)}
    };
    {   // tag with the serial under manual control, if any
        std::lock_guard<std::mutex> lk(liveSerialMtx_);
        auto it = liveSerial_.find(gate);
        if (it != liveSerial_.end() && !it->second.empty())
            body["camera_serial"] = it->second;
    }
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "live"},
        {"messageBody", body}
    };
    return document.dump();
}

std::string MediaSender::buildManualLiveMessage(const std::string& gate,
                                                const std::vector<ManualLiveCam>& cams) const {
    json arr = json::array();
    const int div = std::max(1, cfg_.manualLiveScaleDiv);
    for (const auto& c : cams) {
        cv::Mat out = c.image;
        if (div > 1 && !c.image.empty())
            cv::resize(c.image, out, cv::Size(c.image.cols / div, c.image.rows / div));
        json cam = {
            {"camera_serial", c.serial},
            {"role", c.role},
            {"live_image", encodeJpeg(out, cfg_.manualLiveJpegQuality, cfg_.manualLiveEncoding)}
        };
        if (c.exposureUs >= 0) cam["exposure_us"] = c.exposureUs;
        if (c.gain       >= 0) cam["gain"]        = c.gain;
        arr.push_back(std::move(cam));
    }
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "live_manual_control"},
        {"messageBody", {
            {"timestamp", formatLocalTime("%Y-%m-%dT%H:%M:%S")},
            {"camera_id", gate},
            {"cameras", std::move(arr)}
        }}
    };
    return document.dump();
}

void MediaSender::sendManualLive(const std::string& gate, const std::vector<ManualLiveCam>& cams) {
    if (cams.empty()) return;
    LOGD() << "MediaSender: >>> live_manual_control gate=" << gate << " cams=" << cams.size()
           << " to '" << cfg_.liveSubject << "'";
    transport_.publish(cfg_.liveSubject, buildManualLiveMessage(gate, cams));
}

std::string MediaSender::buildCrudeMessage(const std::string& gate, const cv::Mat& frame) const {
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "crud_image"},
        {"messageBody", {
            {"camera_id", gate},
            {"crud_image", encodeJpeg(frame, cfg_.crudeJpegQuality, cfg_.imageEncoding)}
        }}
    };
    return document.dump();
}

void MediaSender::sendCrudeImage(const std::string& gate, const cv::Mat& frame) {
    if (frame.empty()) {
        LOGW() << "MediaSender: crud_image SKIPPED gate=" << gate << " (empty frame)";
        return;
    }
    const std::string payload = buildCrudeMessage(gate, frame);
    LOGI() << "MediaSender: >>> crud_image gate=" << gate << " frame=" << frame.cols << "x" << frame.rows
           << " payload=" << payload.size() << " bytes -> '" << cfg_.crudeSubject << "'";
    transport_.publish(cfg_.crudeSubject, payload);
}

std::string MediaSender::buildScreenshotMessage(const std::string& gate, const cv::Mat& frame) const {
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "screenshot"},
        {"messageBody", {
            {"camera_id", gate},
            {"timestamp", formatLocalTime("%Y-%m-%dT%H:%M:%S")},
            {"width", frame.cols},
            {"height", frame.rows},
            {"screenshot", encodeJpeg(frame, cfg_.screenshotJpegQuality, cfg_.imageEncoding)}
        }}
    };
    return document.dump();
}

void MediaSender::sendScreenshot(const std::string& gate, const cv::Mat& frame) {
    if (frame.empty()) {
        LOGW() << "MediaSender: screenshot SKIPPED gate=" << gate << " (empty frame)";
        return;
    }
    const std::string payload = buildScreenshotMessage(gate, frame);
    LOGI() << "MediaSender: >>> screenshot gate=" << gate << " frame=" << frame.cols << "x" << frame.rows
           << " payload=" << payload.size() << " bytes -> '" << cfg_.screenshotSubject << "'";
    transport_.publish(cfg_.screenshotSubject, payload);
}

void MediaSender::setLiveSerial(const std::string& gate, const std::string& serial) {
    std::lock_guard<std::mutex> lk(liveSerialMtx_);
    liveSerial_[gate] = serial;
}

void MediaSender::clearLiveSerial(const std::string& gate) {
    std::lock_guard<std::mutex> lk(liveSerialMtx_);
    liveSerial_.erase(gate);
}

std::string MediaSender::buildRecordingMessage(const std::string& gate, const std::string& videoAddress,
                                               bool endRecording, const cv::Mat& frame) const {
    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "recording"},
        {"messageBody", {
            {"timestamp", formatLocalTime("%Y-%m-%dT%H:%M:%S")},
            {"camera_id", gate},
            {"video_address", videoAddress},
            {"frame", encodeJpeg(frame, cfg_.recordingJpegQuality, cfg_.imageEncoding)},
            {"end_recording", endRecording}
        }}
    };
    return document.dump();
}

void MediaSender::sendLiveFrame(const std::string& gate, const cv::Mat& img) {
    LOGD() << "MediaSender: >>> live frame gate=" << gate << " to '" << cfg_.liveSubject << "'";
    transport_.publish(cfg_.liveSubject, buildLiveMessage(gate, img));
}

void MediaSender::sendRecordingEvent(const std::string& gate, const std::string& videoAddress,
                                     bool endRecording, const cv::Mat& frame) {
    LOGI() << "MediaSender: >>> recording event gate=" << gate << " path='" << videoAddress
           << "' end=" << (endRecording ? 1 : 0) << " to '" << cfg_.recordingSubjectPrefix << gate << "'";
    transport_.publish(cfg_.recordingSubjectPrefix + gate,
                       buildRecordingMessage(gate, videoAddress, endRecording, frame));
}

std::function<void(const std::string&, const cv::Mat&)> MediaSender::liveSink() {
    return [this](const std::string& gate, const cv::Mat& img) { this->sendLiveFrame(gate, img); };
}

std::function<void(const std::string&, const std::string&)> MediaSender::recordingCallback() {
    // RecordingService fires (gate, path) when a segment closes -> a recording event (end=true).
    return [this](const std::string& gate, const std::string& path) {
        this->sendRecordingEvent(gate, path, /*endRecording*/true, cv::Mat());
    };
}

} // namespace lpr
