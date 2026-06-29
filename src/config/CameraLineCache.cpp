#include "lpr/config/CameraLineCache.h"
#include <cmath>
#include "lpr/Log.h"   // log via your Boost.Log logger

namespace lpr {

void CameraLineCache::cacheLines(
    const std::string& camId,
    const std::unordered_map<int, std::vector<cv::Point2f>>& normLines,
    int width,
    int height
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    /*LOGD() << "[CameraLineCache] cacheLines cam='" << camId << "'"
           << " incomingLines=" << normLines.size()
           << " frame=" << width << "x" << height;*/

    auto& cacheForCam = pixelLines_[camId];
    std::size_t added = 0;
    for (auto& [idx, normPoly] : normLines) {
        if (cacheForCam.count(idx) && !cacheForCam[idx].empty()) continue;
        std::vector<cv::Point> pix;
        pix.reserve(normPoly.size());
        for (auto& p : normPoly) {
            int x = std::max(0, int(std::round(p.x * width)));
            int y = std::max(0, int(std::round(p.y * height)));
            pix.emplace_back(
                std::min(x, width),
                std::min(y, height)
            );
        }
        cacheForCam[idx] = std::move(pix);
        ++added;
    }

    /*LOGD() << "[CameraLineCache] cam='" << camId << "' cached " << added
           << " new line(s), total=" << cacheForCam.size();*/
}

std::optional<std::vector<cv::Point>> CameraLineCache::getLine(
    const std::string& camId,
    int lineIndex
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto itCam = pixelLines_.find(camId);
    if (itCam == pixelLines_.end()) {
        LOGT() << "[CameraLineCache] getLine cam='" << camId << "' not found";
        return std::nullopt;
    }
    auto itLine = itCam->second.find(lineIndex);
    if (itLine == itCam->second.end()) {
        LOGT() << "[CameraLineCache] getLine cam='" << camId
               << "' lineIndex=" << lineIndex << " not found";
        return std::nullopt;
    }
    return itLine->second;
}

} // namespace lpr