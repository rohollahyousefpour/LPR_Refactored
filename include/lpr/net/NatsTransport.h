#pragma once
// NatsTransport - the real NATS-backed IMessageTransport (port of NatsClient's publish path).
// The cnats SDK (nats.h) is kept inside the .cpp via PIMPL, so including this header does not
// pull in cnats. Built only when WITH_NATS is enabled and the cnats SDK is found.
#include "lpr/net/IMessageTransport.h"

#include <memory>
#include <string>

namespace lpr {

class NatsTransport : public IMessageTransport {
public:
    struct Config {
        std::string url = "nats://127.0.0.1:4222";
        std::string token;                 // auth token (env AUTH_TOKEN)
        std::string user;                  // username    (env NATS_USER)
        std::string password;              // password    (env NATS_PASSWORD)
        // Optional mutual-TLS (as in the original NatsClient: cert/key/ca).
        std::string certFile;
        std::string keyFile;
        std::string caFile;
        std::string expectedHostname = "ALPR";   // TLS expected hostname (original used "ALPR")
        bool        tlsHandshakeFirst = false;    // set if the server uses tls{ handshake_first:true }
        bool        useJetStream = true;   // publishDurable -> JetStream when available
        std::string outboxDir = "nats_outbox";    // durable-message spool (resent on reconnect)
    };

    explicit NatsTransport(Config cfg);
    NatsTransport() : NatsTransport(Config{}) {}
    ~NatsTransport() override;
    NatsTransport(const NatsTransport&) = delete;
    NatsTransport& operator=(const NatsTransport&) = delete;

    bool connect();   // establish the connection (and JetStream context if enabled)

    bool publish(const std::string& subject, const std::string& payload) override;
    bool publishDurable(const std::string& subject, const std::string& payload) override;
    bool subscribe(const std::string& subject, MessageHandler handler) override;
    // Publish with a reply-to subject (request side of NATS request/reply).
    bool publishWithReply(const std::string& subject, const std::string& payload,
                          const std::string& replySubject);
    bool connected() const override;
    bool flush();   // force-write buffered publishes to the server (PING/PONG round-trip)

    struct Impl;    // public so the cnats connection-state callbacks (free functions) can use it

private:
    std::unique_ptr<Impl> impl_;
    Config cfg_;
};

} // namespace lpr
