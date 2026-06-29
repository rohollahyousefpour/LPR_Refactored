#include "lpr/net/SystemResourceMonitor.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sys/statvfs.h>
  #include <fstream>
  #include <string>
  #include <unistd.h>
#endif

namespace lpr {

SystemResourceMonitor::SystemResourceMonitor(std::string diskPath)
    : diskPath_(std::move(diskPath)) {}

ResourceStats SystemResourceMonitor::sample() {
    ResourceStats s;
    s.cpuPercent      = sampleCpu();
    s.ramPercent      = sampleRam();
    s.freeDiskPercent = sampleFreeDisk();
    return s;
}

#if defined(_WIN32)

static unsigned long long ftToU64(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

double SystemResourceMonitor::sampleCpu() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
    // kernel time includes idle time; total busy = (kernel+user) - idle.
    unsigned long long idleT  = ftToU64(idle);
    unsigned long long total  = ftToU64(kernel) + ftToU64(user);
    if (!havePrev_) { prevIdle_ = idleT; prevTotal_ = total; havePrev_ = true; return 0.0; }
    unsigned long long dIdle  = idleT - prevIdle_;
    unsigned long long dTotal = total - prevTotal_;
    prevIdle_ = idleT; prevTotal_ = total;
    if (dTotal == 0) return 0.0;
    double usage = 100.0 * (double)(dTotal - dIdle) / (double)dTotal;
    if (usage < 0.0) usage = 0.0; if (usage > 100.0) usage = 100.0;
    return usage;
}

double SystemResourceMonitor::sampleRam() const {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0.0;
    return (double)ms.dwMemoryLoad;   // already a 0..100 used-percentage
}

double SystemResourceMonitor::sampleFreeDisk() const {
    std::string path = diskPath_;
    if (path.empty()) {
        char buf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 3) {
            path.assign(buf, 3);     // e.g. "E:\"
        } else {
            path = "C:\\";
        }
    }
    ULARGE_INTEGER freeAvail, totalBytes, totalFree;
    if (!GetDiskFreeSpaceExA(path.c_str(), &freeAvail, &totalBytes, &totalFree)) return 0.0;
    if (totalBytes.QuadPart == 0) return 0.0;
    return 100.0 * (double)totalFree.QuadPart / (double)totalBytes.QuadPart;
}

#else  // POSIX / Linux

double SystemResourceMonitor::sampleCpu() {
    std::ifstream f("/proc/stat");
    if (!f) return 0.0;
    std::string cpu;
    unsigned long long user=0, nice=0, sys=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
    f >> cpu >> user >> nice >> sys >> idle >> iowait >> irq >> softirq >> steal;
    unsigned long long idleAll = idle + iowait;
    unsigned long long total   = user + nice + sys + idle + iowait + irq + softirq + steal;
    if (!havePrev_) { prevIdle_ = idleAll; prevTotal_ = total; havePrev_ = true; return 0.0; }
    unsigned long long dIdle  = idleAll - prevIdle_;
    unsigned long long dTotal = total   - prevTotal_;
    prevIdle_ = idleAll; prevTotal_ = total;
    if (dTotal == 0) return 0.0;
    double usage = 100.0 * (double)(dTotal - dIdle) / (double)dTotal;
    if (usage < 0.0) usage = 0.0; if (usage > 100.0) usage = 100.0;
    return usage;
}

double SystemResourceMonitor::sampleRam() const {
    std::ifstream f("/proc/meminfo");
    if (!f) return 0.0;
    std::string key; unsigned long long val; std::string unit;
    unsigned long long total = 0, avail = 0;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:")     total = val;
        else if (key == "MemAvailable:") { avail = val; break; }
    }
    if (total == 0) return 0.0;
    return 100.0 * (double)(total - avail) / (double)total;
}

double SystemResourceMonitor::sampleFreeDisk() const {
    const std::string path = diskPath_.empty() ? std::string("/") : diskPath_;
    struct statvfs s;
    if (statvfs(path.c_str(), &s) != 0 && statvfs("/", &s) != 0) return 0.0;
    if (s.f_blocks == 0) return 0.0;
    return 100.0 * (double)s.f_bavail / (double)s.f_blocks;
}

#endif

} // namespace lpr
