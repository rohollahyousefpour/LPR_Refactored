#pragma once
//
// GigEBandwidthCoordinator
// ------------------------
// Coordinates packet/bandwidth settings across ALL BaslerCamera instances that
// share one or more GigE network interfaces.
//
// Why this must be global (a singleton):
//   You create one BaslerCamera per camera (or per RGB+Mono pair). Each object
//   only knows about its own camera(s). But every camera on the same NIC draws
//   from the SAME ~110 MB/s pipe. Bandwidth therefore cannot be decided per
//   instance -- it must be decided once, with knowledge of every camera on the
//   link. This class is that single source of truth.
//
// What it gives each camera:
//   * a STABLE 0-based index within its NIC group (keyed by serial number, the
//     only identity that is stable across reconnects), used to stagger frame
//     transmission so cameras don't all blast the wire at the same instant;
//   * a SAFE per-camera frame rate cap so the SUM of all cameras fits the link;
//   * a serialized enumeration lock so multiple instances don't fire GigE
//     discovery broadcasts simultaneously (a real cause of reconnect storms).
//
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

class GigEBandwidthCoordinator {
public:
    struct LinkConfig {
        // Usable bandwidth of ONE NIC. ~110 for 1 GigE, ~1100 for 10 GigE.
        // Usable gigabit throughput in MB/s. Theoretical is 125; documented
        // realistic max is 80-100. Measured ~97 on this rig with 5 cameras, so
        // 90 leaves headroom for resends/jitter (critical at 10 cameras where
        // utilization is otherwise ~97%). Lower further if BadFrame drops appear.
        double linkMBytesPerSec = 90.0;
        // 1500 for standard frames, 9000 only if jumbo frames are enabled
        // end-to-end (camera + switch + NIC). Affects packet count, not the cap.
        int    mtuBytes = 1500;
        // Frame rate you would *like*; the coordinator lowers it if the link
        // can't carry every camera at this rate.
        double targetFps = 10.0;
        // Protocol overhead per packet (GigE Vision), bytes. Basler uses ~36.
        int    packetOverhead = 36;
    };

    static GigEBandwidthCoordinator& instance();

    void       setLinkConfig(const LinkConfig& cfg);
    LinkConfig linkConfig();

    // Register a camera on a NIC group. nicGroup is any stable label for the
    // physical interface the camera is cabled to ("nic0", "eth1", or the NIC IP).
    // Idempotent: calling again with the same serial returns the same index.
    int  registerCamera(const std::string& serial, const std::string& nicGroup);
    void unregisterCamera(const std::string& serial);

    int  indexInGroup(const std::string& serial);   // -1 if unknown
    int  countInGroup(const std::string& nicGroup); // cameras currently on that NIC

    // Safe per-camera fps so that countInGroup * fps * bytesPerFrame <= link.
    // Returns min(targetFps, computedCap). bytesPerFrame is W*H at 8-bit
    // (Bayer color and mono are both 1 byte/pixel on the wire).
    double safeFpsForGroup(const std::string& nicGroup,
                           int64_t bytesPerFrame);

    // Serialize EnumerateDevices() across instances. Hold the returned lock for
    // the duration of the enumeration, then let it drop.
    std::unique_lock<std::mutex> acquireEnumerationLock();

private:
    GigEBandwidthCoordinator() = default;

    struct Entry { int index = 0; std::string nicGroup; };

    std::mutex mtx_;
    std::mutex enumMtx_;
    LinkConfig cfg_;
    std::unordered_map<std::string, Entry> cams_;       // serial -> entry
    std::unordered_map<std::string, int>   nextIndex_;  // nicGroup -> next idx
};
