#pragma once
//
// CapturePipeline
// ---------------
// Owns the grab loop. Each cycle it: drains queued commands, snapshots the
// connected devices, grabs one frame from each, applies that camera's exposure
// strategy, pairs the results, and pushes them to the sink. On a grab
// timeout/failure it asks the supervisor (via the onFault callback) to recycle
// that camera -- it does not do reconnect itself.
//
// This is the relocation of the old BaslerCapture.cpp::captureLoop/processFrame,
// with the locking and fault handling made explicit.
//
#include <functional>
#include <string>
#include "CameraContext.h"

class CapturePipeline {
public:
    // onFault(serial) is invoked on the capture thread when a camera fails to
    // deliver a frame; the supervisor removes + schedules a reconnect.
    CapturePipeline(CameraContext& ctx, std::function<void(const std::string&)> onFault);

    void run();    // blocks until ctx.live becomes false
    void stop();   // sets ctx.live = false

private:
    void deliver(std::vector<cv::Mat>& frames, std::vector<int>& isColor, std::time_t ts);

    CameraContext& ctx_;
    std::function<void(const std::string&)> onFault_;
};
