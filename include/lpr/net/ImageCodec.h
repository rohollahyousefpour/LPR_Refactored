#pragma once
// Shared image -> JSON encoding for the messaging layer (previously duplicated inside
// PlateSender). Encodes a cv::Mat as JPEG and serializes it either as a JSON byte array
// (what the original backend expects) or as a base64 string.
#include "lpr/util/Base64.h"

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <vector>

namespace lpr {

enum class ImageEncoding { ByteArray, Base64 };

inline nlohmann::json encodeJpeg(const cv::Mat& img, int quality, ImageEncoding enc) {
    if (img.empty()) return nlohmann::json::array();
    std::vector<uchar> buf;
    cv::imencode(".jpg", img, buf, {cv::IMWRITE_JPEG_QUALITY, quality});
    if (enc == ImageEncoding::Base64) return base64_encode(buf.data(), buf.size());
    return nlohmann::json(buf);   // std::vector<uchar> -> JSON array of bytes
}

} // namespace lpr
