#include "lpr/net/NatsTransport.h"
#include "lpr/Log.h"

#include <nats/nats.h>   // cnats SDK
#include <nlohmann/json.hpp>

#include <memory>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <optional>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

namespace lpr {

namespace fs = std::filesystem;

// One queued item parsed back into its parts. A "durable" item also carries the outbox
// file that backs it, so the sender can delete it once the server has accepted the message.
struct ParsedMsg {
    std::string            subject;
    std::string            payload;
    bool                   durable   = false;   // backed by an outbox file (offline plate)
    bool                   jetstream = false;    // publish via JetStream (plates only)
    std::optional<fs::path> file;
};

struct NatsTransport::Impl {
    natsConnection* conn = nullptr;
    natsOptions*    opts = nullptr;
    jsCtx*          js   = nullptr;
    std::vector<natsSubscription*>                                 subs;
    std::vector<std::unique_ptr<IMessageTransport::MessageHandler>> handlers;

    // --- async send queue + durable outbox (faithful to the original NatsClient) ---
    std::deque<std::string>  queue;
    std::mutex               qmtx;
    std::condition_variable  qcv;
    std::thread              sender;
    std::atomic<bool>        running{false};
    std::atomic<bool>        disconnected{false};
    fs::path                 outboxDir = "nats_outbox";
    std::size_t              maxQueue  = 2000;

    // Build a unique, time-ordered outbox filename.
    static std::string nowTicks() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        return std::to_string(ms);
    }

    void enqueueRaw(std::string envelope) {
        std::size_t qsize = 0;
        {
            std::lock_guard<std::mutex> lk(qmtx);
            if (queue.size() >= maxQueue) queue.pop_front();   // drop oldest (bounded)
            queue.push_back(std::move(envelope));
            qsize = queue.size();
        }
        qcv.notify_one();
        LOGD() << "NatsTransport: enqueued (queue size=" << qsize << ")";
    }

    // Persist a payload to the outbox so it survives a disconnect; returns the file path.
    fs::path persistToDisk(const std::string& subject, const std::string& payload) {
        std::error_code ec;
        fs::create_directories(outboxDir, ec);
        static std::atomic<unsigned long long> seq{0};
        fs::path finalPath = outboxDir / (nowTicks() + "_" + std::to_string(seq++) + ".json");
        fs::path tmpPath = finalPath; tmpPath += ".tmp";
        nlohmann::json env = {{"subject", subject}, {"payload", payload}};
        try {
            { std::ofstream ofs(tmpPath, std::ios::binary); ofs << env.dump(); ofs.flush(); }
            fs::rename(tmpPath, finalPath, ec);   // atomic on the same volume
            if (ec) LOGE() << "NatsTransport: outbox rename failed: " << ec.message();
        } catch (const std::exception& e) {
            LOGE() << "NatsTransport: persistToDisk failed: " << e.what();
        }
        return finalPath;
    }

    // Read every outbox file (oldest first) and re-enqueue it as a durable envelope.
    void enqueueOutbox() {
        std::error_code ec;
        if (!fs::exists(outboxDir, ec)) return;
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(outboxDir, ec))
            if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            std::ifstream ifs(f, std::ios::binary);
            if (!ifs) continue;
            nlohmann::json env;
            try { ifs >> env; } catch (...) { continue; }
            if (!env.contains("subject") || !env.contains("payload")) continue;
            enqueueRaw("DURABLE|" + f.string() + "|" +
                       env["subject"].get<std::string>() + ":" + env["payload"].get<std::string>());
        }
        if (!files.empty()) LOGI() << "NatsTransport: re-enqueued " << files.size() << " outbox file(s)";
    }

    // "DURABLE|<path>|<subject>:<payload>"  or  "<subject>:<payload>"
    // "DURABLE|<path>|<subject>:<payload>"  -> JetStream + outbox file (offline plate)
    // "J|<subject>:<payload>"               -> JetStream (online plate)
    // "<subject>:<payload>"                 -> core publish (live/heartbeat/resources/camera/recording)
    static ParsedMsg parseEnvelope(const std::string& s) {
        ParsedMsg p;
        static constexpr char kDur[] = "DURABLE|";
        const std::size_t durLen = sizeof(kDur) - 1;
        if (s.size() >= durLen && s.compare(0, durLen, kDur) == 0) {
            const std::size_t bar = s.find('|', durLen);
            if (bar == std::string::npos) return p;
            p.durable = true; p.jetstream = true;
            p.file = fs::path(s.substr(durLen, bar - durLen));
            const std::string rest = s.substr(bar + 1);
            const std::size_t colon = rest.find(':');
            if (colon != std::string::npos) { p.subject = rest.substr(0, colon); p.payload = rest.substr(colon + 1); }
            return p;
        }
        if (s.size() >= 2 && s[0] == 'J' && s[1] == '|') {
            p.jetstream = true;
            const std::string rest = s.substr(2);
            const std::size_t colon = rest.find(':');
            if (colon != std::string::npos) { p.subject = rest.substr(0, colon); p.payload = rest.substr(colon + 1); }
            return p;
        }
        const std::size_t colon = s.find(':');
        if (colon != std::string::npos) { p.subject = s.substr(0, colon); p.payload = s.substr(colon + 1); }
        return p;
    }

    // Background sender: drain the queue, publish + flush, delete the backing file on success.
    // Durable items are always backed by an outbox file, so when offline we simply drop the
    // in-memory copy (the file is re-enqueued on reconnect). Non-durable items (live, heartbeat)
    // are ephemeral and never touch disk.
    void senderLoop() {
        LOGI() << "NatsTransport: sender thread started";
        while (running.load()) {
            std::string raw;
            {
                std::unique_lock<std::mutex> lk(qmtx);
                qcv.wait(lk, [this] { return !queue.empty() || !running.load(); });
                if (!running.load() && queue.empty()) break;
                if (queue.empty()) continue;
                raw = std::move(queue.front());
                queue.pop_front();
            }
            ParsedMsg pm = parseEnvelope(raw);
            if (pm.subject.empty()) { LOGW() << "NatsTransport: sender dropped malformed envelope"; continue; }

            const bool dis = disconnected.load();
            LOGI() << "NatsTransport: sender dequeued subject='" << pm.subject << "' bytes=" << pm.payload.size()
                   << " jetstream=" << (pm.jetstream ? 1 : 0) << " durable=" << (pm.durable ? 1 : 0)
                   << " disconnected=" << (dis ? 1 : 0) << " conn=" << (conn ? 1 : 0);

            if (dis || !conn) {
                LOGW() << "NatsTransport: offline, holding subject='" << pm.subject << "'";
                if (!pm.durable) { std::lock_guard<std::mutex> lk(qmtx); queue.push_back(std::move(raw)); }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            natsStatus s;
            if (pm.jetstream && js) {
                // Plates -> JetStream: js_Publish is synchronous and waits for a PubAck, so the
                // message is confirmed stored in the stream (no separate flush needed).
                jsPubAck*  ack  = nullptr;
                jsErrCode  jerr = (jsErrCode)0;
                s = js_Publish(&ack, js, pm.subject.c_str(), pm.payload.data(),
                               (int)pm.payload.size(), nullptr, &jerr);
                if (s == NATS_OK) {
                    LOGI() << "NatsTransport: js_Publish('" << pm.subject << "') -> OK (stored in stream)";
                    if (ack) jsPubAck_Destroy(ack);
                } else {
                    LOGE() << "NatsTransport: js_Publish('" << pm.subject << "') FAILED: "
                           << natsStatus_GetText(s) << " jsErr=" << (int)jerr;
                }
            } else if (pm.jetstream && !js) {
                LOGW() << "NatsTransport: JetStream unavailable; core-publishing plate to '" << pm.subject << "'";
                s = natsConnection_PublishString(conn, pm.subject.c_str(), pm.payload.c_str());
                if (s == NATS_OK) s = natsConnection_FlushTimeout(conn, 5000);
            } else {
                // Everything else -> core publish + flush (NOT JetStream).
                s = natsConnection_PublishString(conn, pm.subject.c_str(), pm.payload.c_str());
                LOGI() << "NatsTransport: PublishString('" << pm.subject << "') -> " << natsStatus_GetText(s);
                if (s == NATS_OK) {
                    natsStatus f = natsConnection_FlushTimeout(conn, 5000);
                    if (f != NATS_OK) LOGW() << "NatsTransport: Flush('" << pm.subject << "') -> " << natsStatus_GetText(f);
                    s = f;
                }
            }

            if (s == NATS_OK) {
                if (pm.durable && pm.file) {
                    std::error_code ec; fs::remove(*pm.file, ec);
                    LOGI() << "NatsTransport: delivered+removed outbox " << pm.file->filename().string();
                }
            } else {
                LOGW() << "NatsTransport: send of '" << pm.subject << "' failed (requeueing)";
                std::lock_guard<std::mutex> lk(qmtx);
                if (pm.durable) queue.push_front(std::move(raw)); else queue.push_back(std::move(raw));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        LOGI() << "NatsTransport: sender thread exiting";
    }

    void startSender() {
        if (running.exchange(true)) return;
        sender = std::thread(&Impl::senderLoop, this);
    }
    void stopSender() {
        running.store(false);
        qcv.notify_all();
        if (sender.joinable()) sender.join();
    }

    ~Impl() {
        stopSender();
        for (auto* s : subs) if (s) natsSubscription_Destroy(s);
        if (js)   jsCtx_Destroy(js);
        if (conn) natsConnection_Destroy(conn);
        if (opts) natsOptions_Destroy(opts);
    }
};

static void natsMsgTrampoline(natsConnection*, natsSubscription*, natsMsg* msg, void* closure) {
    auto* handler = static_cast<IMessageTransport::MessageHandler*>(closure);
    const char* subj = natsMsg_GetSubject(msg);
    const char* data = natsMsg_GetData(msg);
    int         len  = natsMsg_GetDataLength(msg);
    LOGI() << "NatsTransport: <<< received subject='" << (subj ? subj : "") << "' bytes=" << (data ? len : 0);
    if (handler && *handler)
        (*handler)(subj ? subj : "", std::string(data ? data : "", data ? len : 0));
    natsMsg_Destroy(msg);
}

// Connection-state callbacks update the disconnected flag and trigger an outbox resend on
// reconnect. The closure is the Impl*.
static void natsDisconnectedCb(natsConnection*, void* closure) {
    if (auto* im = static_cast<NatsTransport::Impl*>(closure)) im->disconnected.store(true);
    LOGW() << "NatsTransport: disconnected from server; cnats will keep retrying (messages persist to outbox)";
}
static void natsReconnectedCb(natsConnection*, void* closure) {
    if (auto* im = static_cast<NatsTransport::Impl*>(closure)) {
        im->disconnected.store(false);
        im->enqueueOutbox();   // resend anything persisted while offline
    }
    LOGI() << "NatsTransport: reconnected to server (resending persisted messages)";
}
static void natsClosedCb(natsConnection*, void* closure) {
    if (auto* im = static_cast<NatsTransport::Impl*>(closure)) im->disconnected.store(true);
    LOGE() << "NatsTransport: connection permanently closed";
}

// Async errors from the SERVER (e.g. "Permissions Violation", "Maximum Payload"). Core publishes
// are fire-and-forget, so without this handler a rejected publish still looks like OK locally.
static void natsErrorCb(natsConnection*, natsSubscription* sub, natsStatus err, void*) {
    const char* subj = sub ? natsSubscription_GetSubject(sub) : nullptr;
    LOGE() << "NatsTransport: *** SERVER ERROR *** " << natsStatus_GetText(err)
           << (subj ? std::string(" (subject='") + subj + "')" : "");
}

NatsTransport::NatsTransport(Config cfg)
    : impl_(std::make_unique<Impl>()), cfg_(std::move(cfg)) {
    if (!cfg_.outboxDir.empty()) impl_->outboxDir = cfg_.outboxDir;
}

NatsTransport::~NatsTransport() = default;

bool NatsTransport::connect() {
    if (natsOptions_Create(&impl_->opts) != NATS_OK) {
        LOGE() << "NatsTransport: natsOptions_Create failed";
        return false;
    }
    natsOptions_SetURL(impl_->opts, cfg_.url.c_str());

    if (!cfg_.expectedHostname.empty())
        natsOptions_SetExpectedHostname(impl_->opts, cfg_.expectedHostname.c_str());
    natsOptions_SetNoEcho(impl_->opts, true);
    natsOptions_SetMaxReconnect(impl_->opts, -1);
    natsOptions_SetReconnectWait(impl_->opts, 2000);
    natsOptions_SetTimeout(impl_->opts, 5000);
    natsOptions_SetDisconnectedCB(impl_->opts, &natsDisconnectedCb, impl_.get());
    natsOptions_SetReconnectedCB(impl_->opts, &natsReconnectedCb, impl_.get());
    natsOptions_SetClosedCB(impl_->opts, &natsClosedCb, impl_.get());
    natsOptions_SetErrorHandler(impl_->opts, &natsErrorCb, nullptr);

    if (!cfg_.user.empty())
        natsOptions_SetUserInfo(impl_->opts, cfg_.user.c_str(), cfg_.password.c_str());

    if (!cfg_.certFile.empty() && !cfg_.keyFile.empty()) {
        natsOptions_SetSecure(impl_->opts, true);
        if (cfg_.tlsHandshakeFirst)
            natsOptions_TLSHandshakeFirst(impl_->opts);
        if (!cfg_.caFile.empty()) {
            natsStatus s = natsOptions_LoadCATrustedCertificates(impl_->opts, cfg_.caFile.c_str());
            if (s != NATS_OK)
                LOGE() << "NatsTransport: failed to load CA '" << cfg_.caFile << "': " << natsStatus_GetText(s);
        }
        natsStatus s = natsOptions_LoadCertificatesChain(impl_->opts, cfg_.certFile.c_str(), cfg_.keyFile.c_str());
        if (s != NATS_OK)
            LOGE() << "NatsTransport: failed to load client cert/key ('" << cfg_.certFile << "', '"
                   << cfg_.keyFile << "'): " << natsStatus_GetText(s);
        LOGI() << "NatsTransport: TLS enabled (handshakeFirst=" << (cfg_.tlsHandshakeFirst ? "yes" : "no")
               << ", expectedHostname='" << cfg_.expectedHostname << "')";
    }

    natsStatus cs = natsConnection_Connect(&impl_->conn, impl_->opts);
    if (cs != NATS_OK) {
        const char* detail = nats_GetLastError(nullptr);
        LOGE() << "NatsTransport: failed to connect to " << cfg_.url
               << " (" << natsStatus_GetText(cs) << (detail ? std::string(": ") + detail : "") << ")";
        return false;
    }
    if (cfg_.useJetStream) {
        if (natsConnection_JetStream(&impl_->js, impl_->conn, nullptr) != NATS_OK) {
            LOGW() << "NatsTransport: JetStream unavailable; durable publishes fall back to core";
            impl_->js = nullptr;
        }
    }
    impl_->disconnected.store(false);
    LOGI() << "NatsTransport: connected to " << cfg_.url;

    // Start the async sender and flush any messages persisted on a previous run.
    impl_->startSender();
    impl_->enqueueOutbox();
    return true;
}

bool NatsTransport::publishWithReply(const std::string& subject, const std::string& payload,
                                     const std::string& replySubject) {
    // Replies are part of the synchronous request/response handshake (auth, settings); send now.
    if (!impl_->conn) return false;
    return natsConnection_PublishRequestString(impl_->conn, subject.c_str(),
                                               replySubject.c_str(), payload.c_str()) == NATS_OK;
}

bool NatsTransport::publish(const std::string& subject, const std::string& payload) {
    // Non-blocking: enqueue and let the sender thread publish + flush. Returns true = queued.
    if (!impl_->running.load()) return false;
    impl_->enqueueRaw(subject + ":" + payload);
    return true;
}

bool NatsTransport::flush() {
    if (!impl_->conn) return false;
    return natsConnection_FlushTimeout(impl_->conn, 5000) == NATS_OK;
}

bool NatsTransport::publishDurable(const std::string& subject, const std::string& payload) {
    // Plates go via JetStream. When connected, enqueue a 'J|' (JetStream) envelope. When offline,
    // persist to the outbox and enqueue a 'DURABLE|' envelope (also JetStream) so it survives until
    // reconnect; the sender deletes the file once the stream acks it.
    if (!impl_->running.load()) { impl_->persistToDisk(subject, payload); return true; }
    if (impl_->disconnected.load()) {
        fs::path f = impl_->persistToDisk(subject, payload);
        LOGI() << "NatsTransport: offline -> persisted plate to " << f.filename().string();
        impl_->enqueueRaw("DURABLE|" + f.string() + "|" + subject + ":" + payload);
    } else {
        LOGI() << "NatsTransport: publishDurable(JetStream) subject='" << subject << "' bytes=" << payload.size();
        impl_->enqueueRaw("J|" + subject + ":" + payload);
    }
    return true;
}

bool NatsTransport::connected() const {
    return impl_->conn && natsConnection_Status(impl_->conn) == NATS_CONN_STATUS_CONNECTED;
}

bool NatsTransport::subscribe(const std::string& subject, MessageHandler handler) {
    if (!impl_->conn) return false;
    impl_->handlers.push_back(std::make_unique<MessageHandler>(std::move(handler)));
    void* closure = impl_->handlers.back().get();
    natsSubscription* sub = nullptr;
    natsStatus s = natsConnection_Subscribe(&sub, impl_->conn, subject.c_str(),
                                            &natsMsgTrampoline, closure);
    if (s != NATS_OK) {
        LOGE() << "NatsTransport: subscribe failed for " << subject;
        impl_->handlers.pop_back();
        return false;
    }
    impl_->subs.push_back(sub);
    LOGI() << "NatsTransport: subscribed to " << subject;
    return true;
}

} // namespace lpr
