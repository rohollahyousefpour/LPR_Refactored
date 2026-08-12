#include "lpr/net/PlateSender.h"
#include "lpr/util/Uuid.h"
#include "lpr/util/Time.h"
#include "lpr/Log.h"

#include <nlohmann/json.hpp>
#include <chrono>

namespace lpr {

using json = nlohmann::json;

namespace {
long nowMs() {
    using namespace std::chrono;
    return static_cast<long>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}
} // namespace

PlateSender::PlateSender(IMessageTransport& transport, Config cfg)
    : transport_(transport), cfg_(std::move(cfg)),
      queue_(std::make_shared<PlateQueue>(0)) {}   // unbounded: never drop a plate (as Send_Que_Data)

PlateSender::~PlateSender() { stop(); }

void PlateSender::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&PlateSender::run, this);
}

void PlateSender::stop() {
    if (!running_.exchange(false)) { if (thread_.joinable()) thread_.join(); return; }
    queue_->close();                 // wake the worker
    if (thread_.joinable()) thread_.join();
}

void PlateSender::send(PlateItem item) {
    queue_->push(std::move(item));
}

std::function<void(PlateItem&&)> PlateSender::asSink() {
    return [this](PlateItem&& it) { this->send(std::move(it)); };
}

bool PlateSender::cooldownPasses(const std::string& gate, const std::string& plate, long now) {
    if (cfg_.cooldownMs <= 0) return true;
    std::lock_guard<std::mutex> lk(cooldownMtx_);
    // prune entries older than 5 minutes
    for (auto it = lastSent_.begin(); it != lastSent_.end();) {
        if (now - it->second > 5L * 60 * 1000) it = lastSent_.erase(it); else ++it;
    }
    const std::string key = gate + ":" + plate;
    auto it = lastSent_.find(key);
    if (it != lastSent_.end() && now - it->second < cfg_.cooldownMs) return false;
    lastSent_[key] = now;
    return true;
}

std::string PlateSender::buildMessage(const PlateItem& item) const {
    const PlateResult& p = item.plate;
    cv::Rect box = p.box.boundingRect();
    if (p.vehicleBox.area() > 0) box = p.vehicleBox;

    json vehicle = {
        {"box", {{"left", box.x}, {"top", box.y},
                 {"right", box.x + box.width}, {"bottom", box.y + box.height}}},
        {"direction", p.direction},
        {"plate", {
            {"plate", p.text},
            {"nation", nullptr},
            {"prefix_2", nullptr},
            {"alpha", nullptr},
            {"mid_3", nullptr},
            {"suffix_2", nullptr},
            {"is_valid", cfg_.validator ? cfg_.validator(p.text) : true},
            {"city_code", 0},
            // Prefer the per-pass UUID (stable across the early + correction events);
            // fall back to the numeric tracking id when no passId was set.
            {"track_id", p.passId.empty() ? json(p.trackId) : json(p.passId)}}},
        {"ocr_accuracy", p.confidence},
        {"vehicle_class", {{"class", nullptr}, {"conf", 0}}},
        {"vehicle_type",  {{"class", nullptr}, {"conf", 0}}},
        {"vision_speed", 0.0},
        {"radar_speed", 0.0},
        {"vehicle_color", {{"class", nullptr}, {"conf", 0}}},
        {"meta_data", json::object()}
    };
    if (cfg_.includePlateImage)
        vehicle["plate"]["plate_image"] = encodeJpeg(p.plateImage, cfg_.plateJpegQuality, cfg_.imageEncoding);

    json cars = json::array();
    cars.push_back(std::move(vehicle));

    json body = {
        {"timestamp", formatLocalTime("%Y-%m-%dT%H:%M:%S")},
        {"camera_id", item.gate},
        {"cars", cars}
    };
    if (cfg_.includeFullImage)
        body["full_image"] = encodeJpeg(item.image, cfg_.jpegQuality, cfg_.imageEncoding);

    json document = {
        {"messageId", generateUuidV4()},
        {"messageType", "plates_data"},
        {"messageBody", std::move(body)}
    };
    return document.dump();
}

void PlateSender::run() {
    LOGI() << "PlateSender: started (subject=" << cfg_.subject << ")";
    while (running_) {
        auto item = queue_->pop();                 // blocks; returns nullopt when closed+empty
        if (!item) break;
        const std::string& plate = item->plate.text;
        if (plate.empty()) continue;
        // A correction of an already-announced pass (forceSend) must bypass the
        // per-plate cooldown; otherwise the second (corrected) send is suppressed.
        if (!item->forceSend && !cooldownPasses(item->gate, plate, nowMs())) {
            LOGD() << "PlateSender: cooldown skip " << item->gate << ":" << plate;
            continue;
        }
        const std::string payload = buildMessage(*item);
        LOGI() << "PlateSender: sending plate '" << plate << "' gate=" << item->gate
               << " (" << payload.size() << " bytes, durable=" << (cfg_.durable ? 1 : 0)
               << ", subject=" << cfg_.subject << ")";
        const bool queued = cfg_.durable ? transport_.publishDurable(cfg_.subject, payload)
                                          : transport_.publish(cfg_.subject, payload);
        // `queued` reflects ENQUEUE, not delivery: the durable/JetStream transport
        // guarantees delivery via its outbox + retry and logs the ACTUAL per-message
        // JetStream ack/failure itself (NatsTransport::senderLoop -> js_Publish). A
        // false here therefore means the transport refused the item outright — only
        // possible while the sender is shutting down — i.e. a genuine, loggable loss.
        if (queued) {
            ++sent_;
        } else {
            LOGE() << "PlateSender: transport REJECTED plate " << item->gate << ":" << plate
                   << " (sender not running) -> plate LOST";
        }
    }
    LOGI() << "PlateSender: stopped (sent=" << sent_ << ")";
}

} // namespace lpr
