#include "lpr/capture/VlcCaptureSource.h"
#include "lpr/Log.h"

#include <opencv2/opencv.hpp>
#include <cstring>
#include <ctime>

namespace lpr {

namespace {
constexpr int kFrameTimeoutSec = 5;

void*    lockTr(void* d, void** p)                 { return static_cast<VlcCaptureSource*>(d)->lock(p); }
void     unlockTr(void* d, void* id, void* const* p){ static_cast<VlcCaptureSource*>(d)->unlock(id, p); }
void     displayTr(void* d, void* id)              { static_cast<VlcCaptureSource*>(d)->display(id); }
unsigned formatTr(void** opaque, char* chroma, unsigned* w, unsigned* h, unsigned* pitches, unsigned* lines) {
    return static_cast<VlcCaptureSource*>(*opaque)->setupFormat(chroma, w, h, pitches, lines);
}
void     cleanupTr(void* /*opaque*/) {}
void     eventTr(const libvlc_event_t* e, void* d) { static_cast<VlcCaptureSource*>(d)->handleEvent(e); }
} // namespace

VlcCaptureSource::~VlcCaptureSource() { stop(); }

void VlcCaptureSource::setAddress(const std::string& address, int delayMs) {
    address_ = address;
    delayMs_ = delayMs;
}

// VLC reports the real resolution here -> allocate the render buffer; no probe needed.
unsigned VlcCaptureSource::setupFormat(char* chroma, unsigned* w, unsigned* h, unsigned* pitches, unsigned* lines) {
    std::memcpy(chroma, "RV24", 4);
    size_ = cv::Size(static_cast<int>(*w), static_cast<int>(*h));
    { std::lock_guard<std::mutex> lk(frameMtx_); frameBuf_.create(size_, CV_8UC3); pixels_ = frameBuf_.data; }
    pitches[0] = (*w) * 3;
    lines[0]   = *h;
    LOGI() << "VlcCaptureSource: format " << *w << "x" << *h << " (RV24) for " << address_;
    return 1;   // one plane
}

void VlcCaptureSource::run() {
    done_ = 0;
    lastPixel_ = 0;

    const char* argv[] = { "--no-audio", "--no-video-title-show" };
    vlc_ = libvlc_new(2, argv);
    if (!vlc_) { LOGE() << "VlcCaptureSource: libvlc_new failed"; emitError(); return; }

    media_  = libvlc_media_new_location(vlc_, address_.c_str());
    // For RTSP, force TCP transport (UDP loses packets on busy links) and a small network cache.
    if (media_ && address_.rfind("rtsp://", 0) == 0) {
        libvlc_media_add_option(media_, ":rtsp-tcp");
        libvlc_media_add_option(media_, ":network-caching=300");
    }
    player_ = libvlc_media_player_new_from_media(media_);
    libvlc_media_release(media_);
    media_ = nullptr;

    libvlc_event_manager_t* em = libvlc_media_player_event_manager(player_);
    libvlc_event_attach(em, libvlc_MediaPlayerEndReached,       eventTr, this);
    libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, eventTr, this);

    libvlc_video_set_callbacks(player_, lockTr, unlockTr, displayTr, this);
    libvlc_video_set_format_callbacks(player_, formatTr, cleanupTr);
    libvlc_media_player_play(player_);
    LOGI() << "VlcCaptureSource: playing " << address_;

    { std::unique_lock<std::mutex> lk(condMtx_); cond_.wait(lk, [this] { return done_.load() != 0; }); }

    const bool wasError = (done_.load() == 1);
    stop();                      // idempotent release
    if (wasError) emitError();   // error/eos -> let CameraWorker reconnect
}

void* VlcCaptureSource::lock(void** pPixels) {
    std::lock_guard<std::mutex> lk(frameMtx_);
    *pPixels = pixels_;
    return nullptr;
}

void VlcCaptureSource::unlock(void* /*id*/, void* const* /*pPixels*/) {}

// A frame has just been rendered into frameBuf_; emit an owning copy. VLC "RV24"
// delivers RGB byte order, so convert to OpenCV's BGR. (If a build/camera ever
// shows inverted colors, replace the cvtColor with a plain frameBuf_.copyTo(out).)
void VlcCaptureSource::display(void* /*id*/) {
    cv::Mat out;
    {
        std::lock_guard<std::mutex> lk(frameMtx_);
        if (frameBuf_.empty()) return;
        cv::cvtColor(frameBuf_, out, cv::COLOR_RGB2BGR);
    }
    lastPixel_ = static_cast<long>(std::time(nullptr));
    emitFrame(out, cv::Mat(), lastPixel_.load());
}

void VlcCaptureSource::handleEvent(const libvlc_event_t* e) {
    switch (e->type) {
        case libvlc_MediaPlayerEncounteredError:
        case libvlc_MediaPlayerEndReached: {
            int expected = 0;
            done_.compare_exchange_strong(expected, 1);
            std::lock_guard<std::mutex> lk(condMtx_);
            cond_.notify_all();
            break;
        }
        default: break;
    }
}

bool VlcCaptureSource::isLive() const {
    long last = lastPixel_.load();
    if (last == 0) return true;                            // not started yet
    return (std::time(nullptr) - last) <= kFrameTimeoutSec;
}

void VlcCaptureSource::stop() {
    int expected = 0;
    done_.compare_exchange_strong(expected, 2);            // mark stop if still running
    { std::lock_guard<std::mutex> lk(condMtx_); cond_.notify_all(); }

    std::lock_guard<std::mutex> lk(stopMtx_);              // idempotent release
    if (player_) { libvlc_media_player_stop(player_); libvlc_media_player_release(player_); player_ = nullptr; }
    if (vlc_)    { libvlc_release(vlc_); vlc_ = nullptr; }
}

} // namespace lpr
