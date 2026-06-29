#pragma once
// InMemoryTransport - an IMessageTransport that just records what would have been published.
// Useful as a safe default (no broker required) and for testing PlateSender /
// CameraStatusNotifier without a live NATS server.
#include "lpr/net/IMessageTransport.h"

#include <mutex>
#include <string>
#include <vector>

namespace lpr {

class InMemoryTransport : public IMessageTransport {
public:
    struct Message {
        std::string subject;
        std::string payload;
        bool        durable = false;
    };

    bool publish(const std::string& subject, const std::string& payload) override {
        std::lock_guard<std::mutex> lk(mtx_);
        messages_.push_back({subject, payload, false});
        return true;
    }
    bool publishDurable(const std::string& subject, const std::string& payload) override {
        std::lock_guard<std::mutex> lk(mtx_);
        messages_.push_back({subject, payload, true});
        return true;
    }
    bool connected() const override { return true; }

    // Record subscriptions and let tests simulate the backend pushing a message.
    bool subscribe(const std::string& subject, MessageHandler handler) override {
        std::lock_guard<std::mutex> lk(mtx_);
        subs_.push_back({subject, std::move(handler)});
        return true;
    }
    // Test helper: deliver a message to all handlers subscribed to `subject`.
    void deliver(const std::string& subject, const std::string& payload) {
        std::vector<MessageHandler> handlers;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& s : subs_) if (s.subject == subject) handlers.push_back(s.handler);
        }
        for (auto& h : handlers) h(subject, payload);
    }

    std::vector<Message> messages() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return messages_;
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return messages_.size();
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        messages_.clear();
    }

private:
    struct Sub { std::string subject; MessageHandler handler; };
    mutable std::mutex   mtx_;
    std::vector<Message> messages_;
    std::vector<Sub>     subs_;
};

} // namespace lpr
