#pragma once
// PlateSender - the clean port of Handle_Clients::send_plates + the Send_Que_Data background
// thread. It is the DOWNSTREAM SINK of the pipeline: processed plates are handed to it, it
// builds the "plates_data" JSON message (matching the original wire format) and publishes it
// through an IMessageTransport (NATS in production, in-memory in tests).
//
//   DetectionWorker --sink--> PlateSender.send(PlateItem)  ==(queue+thread)==>  transport.publishDurable
//
// Wire it with:  worker.setPlateSink(plateSender.asSink());
//
// Improvements over the original: the broker is behind IMessageTransport (no cnats/boost
// dependency leaks here); the JSON builder is a pure function (buildMessage) that is unit
// tested; the per-(gate:plate) send cooldown is configurable; image encoding (byte-array to
// match the backend, or base64) is configurable.
#include "lpr/detect/PlateItem.h"
#include "lpr/net/IMessageTransport.h"
#include "lpr/net/ImageCodec.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace lpr {

class PlateSender {
public:
    using ImageEncoding = lpr::ImageEncoding;   // ByteArray (backend-matching) or Base64

    struct Config {
        std::string subject       = "messages.plates_data";  // JetStream/durable subject
        bool        durable       = true;
        int         jpegQuality   = 50;     // full frame
        int         plateJpegQuality = 60;  // plate crop
        bool        includeFullImage  = true;
        bool        includePlateImage = true;
        long        cooldownMs    = 60000;  // suppress same gate:plate within this window
        ImageEncoding imageEncoding = ImageEncoding::ByteArray;
        // Optional structural validity test reported as plate.is_valid (e.g. checkPlate8).
        std::function<bool(const std::string&)> validator;
    };

    PlateSender(IMessageTransport& transport, Config cfg);
    PlateSender(IMessageTransport& transport) : PlateSender(transport, Config{}) {}
    ~PlateSender();
    PlateSender(const PlateSender&) = delete;
    PlateSender& operator=(const PlateSender&) = delete;

    void start();
    void stop();

    // Enqueue a plate for sending (non-blocking). Safe to call from the detection thread.
    void send(PlateItem item);

    // Adapter for DetectionWorker::setPlateSink.
    std::function<void(PlateItem&&)> asSink();

    // Pure: build the plates_data JSON document for one plate (no I/O). Public for testing.
    std::string buildMessage(const PlateItem& item) const;

    std::uint64_t sentCount() const { return sent_; }

private:
    void run();
    bool cooldownPasses(const std::string& gate, const std::string& plate, long nowMs);

    IMessageTransport& transport_;
    Config             cfg_;

    std::shared_ptr<PlateQueue> queue_;
    std::thread                 thread_;
    std::atomic<bool>           running_{false};
    std::atomic<std::uint64_t>  sent_{0};

    std::mutex                              cooldownMtx_;
    std::unordered_map<std::string, long>   lastSent_;   // "gate:plate" -> ms
};

} // namespace lpr
