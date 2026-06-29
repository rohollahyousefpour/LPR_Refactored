#pragma once
// IMessageTransport - the seam that hides the message broker (NATS) from the rest of the
// codebase. PlateSender and CameraStatusNotifier publish through this interface, so they
// don't depend on cnats/boost. Implementations:
//   * NatsTransport      - real NATS (gated behind WITH_NATS; needs the cnats SDK)
//   * InMemoryTransport  - records published messages; used by tests and as a safe default
#include <functional>
#include <string>

namespace lpr {

class IMessageTransport {
public:
    // Delivered (subject, payload) for a subscription.
    using MessageHandler = std::function<void(const std::string& subject, const std::string& payload)>;

    virtual ~IMessageTransport() = default;

    // Fire-and-forget publish (NATS core). Returns false on failure.
    virtual bool publish(const std::string& subject, const std::string& payload) = 0;

    // Durable / at-least-once publish (NATS JetStream). Falls back to publish() if the
    // implementation has no durable channel.
    virtual bool publishDurable(const std::string& subject, const std::string& payload) {
        return publish(subject, payload);
    }

    // Publish with a reply-to subject (request side of request/reply). Default: plain publish.
    virtual bool publishWithReply(const std::string& subject, const std::string& payload,
                                  const std::string& /*replySubject*/) {
        return publish(subject, payload);
    }

    // Subscribe to a subject; the handler is invoked for each message. Needed for the
    // bootstrap flow (receiving LPR/camera settings and commands pushed by the backend).
    // Default: not supported (returns false).
    virtual bool subscribe(const std::string& /*subject*/, MessageHandler /*handler*/) {
        return false;
    }

    virtual bool connected() const = 0;
};

} // namespace lpr
