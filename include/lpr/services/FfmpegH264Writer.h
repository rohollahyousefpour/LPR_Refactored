#pragma once
// FfmpegH264Writer — encodes BGR frames to H.264 in an .mp4 container using FFmpeg's Windows
// Media Foundation encoder (h264_mf). cv::VideoWriter cannot SELECT that encoder (it takes the
// first H.264 encoder FFmpeg registers — the d3d12 hardware one here — which rejects CPU frames),
// so we drive libavcodec directly. h264_mf is built into the vcpkg FFmpeg (mfplat is linked) and
// needs NO extra runtime DLL. open() returns false when FFmpeg / h264_mf is unavailable so the
// caller can fall back to cv::VideoWriter (mp4v).
#include <opencv2/core.hpp>
#include <string>

namespace lpr {

class FfmpegH264Writer {
public:
    FfmpegH264Writer() = default;
    ~FfmpegH264Writer();
    FfmpegH264Writer(const FfmpegH264Writer&) = delete;
    FfmpegH264Writer& operator=(const FfmpegH264Writer&) = delete;

    // Open an .mp4 at `path` for H.264 encoding. `crf` nudges the target bitrate (MF has no CRF).
    bool open(const std::string& path, double fps, cv::Size frameSize, int crf);
    // Encode one BGR frame (resized to frameSize if needed). Returns false on a fatal error.
    bool write(const cv::Mat& bgr);
    // Flush the encoder, write the trailer, and release everything. Safe to call repeatedly.
    void close();
    bool isOpened() const { return opened_; }

private:
    struct Impl;          // opaque -> the header stays free of FFmpeg includes
    Impl* impl_ = nullptr;
    bool  opened_ = false;
};

} // namespace lpr
