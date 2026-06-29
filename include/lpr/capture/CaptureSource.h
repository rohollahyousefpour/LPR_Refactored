#pragma once
// CaptureSource - abstract interface for every camera/video source (was
// virtual_cap_url). OpenCV-free at the header level (cv::Mat is forward-declared
// and only ever passed by const-ref), and boost-free: it uses std::function
// callbacks instead of boost::signals2.
#include <functional>
#include <string>

namespace cv { class Mat; }   // forward declaration -> no OpenCV include needed here

namespace lpr {

class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    using FrameCallback = std::function<void(const cv::Mat& frame, const cv::Mat& mono, long timestamp)>;
    using ErrorCallback = std::function<void()>;

    void onFrame(FrameCallback cb) { frameCb_ = std::move(cb); }
    void onError(ErrorCallback cb) { errorCb_ = std::move(cb); }

    virtual void setAddress(const std::string& address, int delayMs) = 0;
    virtual void setMonoAddress(const std::string& /*address*/, int /*delayMs*/) {}

    virtual void run()  = 0;          // capture loop, blocking until stop()
    virtual void stop() = 0;
    virtual bool isLive() const = 0;

    virtual void handleCommand(const std::string& /*key*/, const std::string& /*value*/) {}

protected:
    void emitFrame(const cv::Mat& f, const cv::Mat& m, long t) { if (frameCb_) frameCb_(f, m, t); }
    void emitError() { if (errorCb_) errorCb_(); }

    FrameCallback frameCb_;
    ErrorCallback errorCb_;
};

} // namespace lpr
