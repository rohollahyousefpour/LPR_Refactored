#pragma once
// CameraKind - dependency-free classification of the configured "type_of_link".
// Single source of truth for "which kind of camera is this". Exact, case-sensitive
// match (preserves the original detection::thr_url() behavior).
#include <string>

namespace lpr {

enum class CameraKind { Video, Rtsp, Gstreamer, Pylon, PointGrey, Unknown };

inline CameraKind parseCameraKind(const std::string& typeOfLink) {
    if (typeOfLink == "video")     return CameraKind::Video;
    if (typeOfLink == "rtsp")      return CameraKind::Rtsp;
    if (typeOfLink == "gstreamer") return CameraKind::Gstreamer;
    if (typeOfLink == "pylon")     return CameraKind::Pylon;
    if (typeOfLink == "grey")      return CameraKind::PointGrey;
    return CameraKind::Unknown;
}

inline const char* toString(CameraKind k) {
    switch (k) {
        case CameraKind::Video:     return "video";
        case CameraKind::Rtsp:      return "rtsp";
        case CameraKind::Gstreamer: return "gstreamer";
        case CameraKind::Pylon:     return "pylon";
        case CameraKind::PointGrey: return "grey";
        default:                    return "unknown";
    }
}

} // namespace lpr
