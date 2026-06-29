#pragma once
// Boost-free time helpers (std::chrono / std::strftime).
#include <chrono>
#include <ctime>
#include <string>

namespace lpr {

inline long long nowEpochMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline long long nowEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Format current local time with a strftime pattern (default "YYYY-MM-DD").
inline std::string formatLocalTime(const char* fmt = "%Y-%m-%d") {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tmv);
    return std::string(buf);
}

} // namespace lpr
