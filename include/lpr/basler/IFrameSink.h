#pragma once
//
// IFrameSink  (Observer / output port)
// ------------------------------------
// Where finished frames go. The CapturePipeline produces frames and pushes them
// to a sink; it does not know or care what happens next (encode, stream, write
// to disk, forward to an LPR engine). This decouples capture from delivery --
// the old code called send_frame() directly, hard-wiring the pipeline to one
// consumer.
//
// A pair instance delivers (mono, color); a lone RGB instance delivers
// (rgb, rgb). onCaptureError signals a fatal capture condition (the old
// safe_send_cap(-1)), so the consumer can react once, not per glitch.
//
#include <ctime>
#include <opencv2/core.hpp>

class IFrameSink {
public:
    virtual ~IFrameSink() = default;

    // left  = mono (or rgb when no pair), right = rgb. timestamp is capture time.
    virtual void onFramePair(const cv::Mat& left, const cv::Mat& right, std::time_t ts) = 0;

    // Optional: the pair was incomplete this cycle (e.g. slave mid-reconnect).
    virtual void onIncompletePair(const cv::Mat& available, std::time_t ts) { (void)available; (void)ts; }

    // Fatal capture error -- emitted once, not for recoverable per-frame hiccups.
    virtual void onCaptureError(int code) { (void)code; }
};
