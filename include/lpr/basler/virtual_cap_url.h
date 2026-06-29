#pragma once
// Compatibility shim (lpr_basler only): the vendored Basler facade was written
// against the legacy virtual_cap_url base (boost::signals2 send_frame/send_cap).
// This maps that base onto the clean lpr::CaptureSource so the facade compiles
// unchanged and plugs into the new factory/worker.
#include "lpr/capture/CaptureSource.h"
#include <opencv2/core.hpp>
#include <functional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

// Minimal boost::signals2-like signal.
template <typename Sig> class LprSignal;
template <typename... Args>
class LprSignal<void(Args...)> {
public:
    template <typename F> void connect(F&& f) { slots_.emplace_back(std::forward<F>(f)); }
    void disconnect_all_slots() { slots_.clear(); }
    void operator()(Args... args) { for (auto& s : slots_) s(args...); }
private:
    std::vector<std::function<void(Args...)>> slots_;
};

class virtual_cap_url : public lpr::CaptureSource {
public:
    virtual_cap_url() {
        send_frame.connect([this](const cv::Mat& a, const cv::Mat& b, long t) { this->emitFrame(a, b, t); });
        send_cap.connect([this](int) { this->emitError(); });
    }

    // legacy output signals the facade emits on
    LprSignal<void(const cv::Mat&, const cv::Mat&, long)> send_frame;
    LprSignal<void(int)>                                  send_cap;

    // legacy virtuals the facade overrides
    virtual void set_adress(std::string adr, int dl) = 0;
    virtual void set_mono_adress(std::string /*adr*/, int /*dl*/) {}
    virtual bool is_live() = 0;
    virtual void stop_vlc() = 0;
    virtual void handleCommand(const std::string& /*key*/, const nlohmann::json& /*value*/) {}

    // bridge: clean CaptureSource interface -> legacy API
    void setAddress(const std::string& adr, int dl) override { set_adress(adr, dl); }
    void setMonoAddress(const std::string& adr, int dl) override { set_mono_adress(adr, dl); }
    void stop() override { stop_vlc(); }
    bool isLive() const override { return const_cast<virtual_cap_url*>(this)->is_live(); }
    // run() stays pure -> the facade implements it

    // Bridge the clean CaptureSource string-command interface (what CameraWorker
    // calls) into the legacy json handler. The manager passes a JSON object string
    // carrying at least {camera_serial, value}; parse it and dispatch to the json
    // overload (BaslerCamera::handleCommand), which enqueues a typed ICommand.
    void handleCommand(const std::string& key, const std::string& value) override {
        try {
            nlohmann::json v = value.empty() ? nlohmann::json::object()
                                             : nlohmann::json::parse(value);
            handleCommand(key, v);   // virtual -> BaslerCamera json handler
        } catch (const std::exception&) {
            // Malformed payload: drop it. The json handler also guards internally.
        }
    }
};
