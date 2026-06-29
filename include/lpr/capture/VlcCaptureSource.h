#pragma once
// VlcCaptureSource (was vlc_url) - libVLC RTSP capture. Uses libVLC video FORMAT
// callbacks (so VLC tells us the resolution -> no separate probe instance) and
// emits each frame from the DISPLAY callback (event-driven, no polling). Boost-free.
#include "lpr/capture/CaptureSource.h"
#include <opencv2/core.hpp>
#include <vlc/vlc.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace lpr {

class VlcCaptureSource : public CaptureSource {
public:
    VlcCaptureSource() = default;
    ~VlcCaptureSource() override;

    void setAddress(const std::string& address, int delayMs) override;
    void run()  override;
    void stop() override;
    bool isLive() const override;

    // libVLC C-callback hooks (public so the trampolines can reach them):
    void*    lock(void** pPixels);
    void     unlock(void* id, void* const* pPixels);
    void     display(void* id);
    unsigned setupFormat(char* chroma, unsigned* w, unsigned* h, unsigned* pitches, unsigned* lines);
    void     handleEvent(const libvlc_event_t* e);

private:
    std::string address_;
    int         delayMs_ = 0;
    cv::Size    size_{0, 0};

    libvlc_instance_t*     vlc_    = nullptr;
    libvlc_media_t*        media_  = nullptr;
    libvlc_media_player_t* player_ = nullptr;

    cv::Mat        frameBuf_;            // RV24 render target (allocated in setupFormat)
    std::mutex     frameMtx_;
    unsigned char* pixels_ = nullptr;

    std::mutex              condMtx_;
    std::condition_variable cond_;
    std::atomic<int>        done_{0};       // 0 running, 1 error/eos, 2 external stop
    std::atomic<long>       lastPixel_{0};  // time of last delivered frame
    std::mutex              stopMtx_;       // makes stop() idempotent/thread-safe
};

} // namespace lpr
