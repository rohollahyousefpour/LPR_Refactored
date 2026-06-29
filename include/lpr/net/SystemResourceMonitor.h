#pragma once
#include "lpr/net/HeartbeatMonitor.h"   // ResourceStats
#include <string>

namespace lpr {

// Lightweight, dependency-free CPU / RAM / disk sampler (no WMI/COM).
//   cpuPercent      : overall CPU utilisation 0..100 (delta between successive samples)
//   ramPercent      : physical RAM used 0..100
//   freeDiskPercent : free space on the chosen path 0..100
// Construct once and call sample() repeatedly; CPU needs the previous sample to compute a delta,
// so the first call returns 0 for CPU and subsequent calls return the usage since the last call.
class SystemResourceMonitor {
public:
    explicit SystemResourceMonitor(std::string diskPath = "");  // empty -> exe drive / "/"
    ResourceStats sample();

private:
    std::string diskPath_;
    // CPU delta state
    unsigned long long prevIdle_  = 0;
    unsigned long long prevTotal_ = 0;
    bool               havePrev_  = false;

    double sampleCpu();
    double sampleRam() const;
    double sampleFreeDisk() const;
};

} // namespace lpr
