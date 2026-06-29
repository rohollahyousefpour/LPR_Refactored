#include "GigEBandwidthCoordinator.h"
#include "AppLogger.h"
#include <algorithm>

GigEBandwidthCoordinator& GigEBandwidthCoordinator::instance() {
    static GigEBandwidthCoordinator s; // thread-safe init since C++11
    return s;
}

void GigEBandwidthCoordinator::setLinkConfig(const LinkConfig& cfg) {
    std::lock_guard<std::mutex> lk(mtx_);
    cfg_ = cfg;
}

GigEBandwidthCoordinator::LinkConfig GigEBandwidthCoordinator::linkConfig() {
    std::lock_guard<std::mutex> lk(mtx_);
    return cfg_;
}

int GigEBandwidthCoordinator::registerCamera(const std::string& serial,
                                             const std::string& nicGroup) {
    std::lock_guard<std::mutex> lk(mtx_);

    auto it = cams_.find(serial);
    if (it != cams_.end()) {
        // Already known. If the NIC group changed (recabled), keep the original
        // index for the *new* group only if free; otherwise reassign.
        if (it->second.nicGroup == nicGroup)
            return it->second.index;
        // Re-home to the new group.
        it->second.nicGroup = nicGroup;
        it->second.index    = nextIndex_[nicGroup]++;
        return it->second.index;
    }

    Entry e;
    e.nicGroup = nicGroup;
    e.index    = nextIndex_[nicGroup]++;
    cams_.emplace(serial, e);
    return e.index;
}

void GigEBandwidthCoordinator::unregisterCamera(const std::string& serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    // We intentionally DO NOT recycle the index. A camera that drops and
    // reconnects must keep its slot so its stagger offset stays stable and it
    // never collides with a peer. Slots are cheap; collisions are not.
    cams_.erase(serial);
}

int GigEBandwidthCoordinator::indexInGroup(const std::string& serial) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = cams_.find(serial);
    return (it == cams_.end()) ? -1 : it->second.index;
}

int GigEBandwidthCoordinator::countInGroup(const std::string& nicGroup) {
    std::lock_guard<std::mutex> lk(mtx_);
    int n = 0;
    for (auto& kv : cams_)
        if (kv.second.nicGroup == nicGroup) ++n;
    return std::max(1, n);
}

double GigEBandwidthCoordinator::safeFpsForGroup(const std::string& nicGroup,
                                                 int64_t bytesPerFrame) {
    LinkConfig cfg;
    int n;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        cfg = cfg_;
        n = 0;
        for (auto& kv : cams_)
            if (kv.second.nicGroup == nicGroup) ++n;
        n = std::max(1, n);
    }
    if (bytesPerFrame <= 0) return cfg.targetFps;

    const double linkBytesPerSec = cfg.linkMBytesPerSec * 1e6;
    const double perCamBytesPerSec = linkBytesPerSec / static_cast<double>(n);
    const double cap = perCamBytesPerSec / static_cast<double>(bytesPerFrame);

    // Overload signal: if too many cameras share one link, the per-camera cap
    // falls to a marginal frame rate. Warn loudly so it's an explicit decision,
    // not silent degradation. ~3 fps is still usable for slow (parking) traffic;
    // below that, the link is oversubscribed and cameras should be split across
    // NICs. The aggregate sitting near link capacity also risks BadFrame drops.
    constexpr double kFpsFloor = 3.0;
    const double aggregateMBs = (n * bytesPerFrame * std::min(cfg.targetFps, cap)) / 1e6;
    if (cap < kFpsFloor) {
        LOGW() << "[bw-coord] NIC '" << nicGroup << "' has " << n
               << " cameras -> only " << cap << " fps each (floor " << kFpsFloor
               << "). Link oversubscribed; consider splitting cameras across NICs.";
    } else if (aggregateMBs > cfg.linkMBytesPerSec * 0.95) {
        LOGW() << "[bw-coord] NIC '" << nicGroup << "' near capacity: ~"
               << aggregateMBs << " MB/s of " << cfg.linkMBytesPerSec
               << " usable with " << n << " cameras -- BadFrame drops possible.";
    }

    // Leave the targetFps as an upper bound; never raise fps above what the
    // user asked for just because the link is fast.
    return std::max(0.1, std::min(cfg.targetFps, cap));
}

std::unique_lock<std::mutex> GigEBandwidthCoordinator::acquireEnumerationLock() {
    return std::unique_lock<std::mutex>(enumMtx_);
}
