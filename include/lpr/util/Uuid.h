#pragma once
// Boost-free random (RFC-4122 version-4) UUID generator.
// Replaces the boost::uuids dependency from the old PathAndUUIDGenerator.
#include <cstdio>
#include <random>
#include <string>

namespace lpr {

inline std::string generateUuidV4() {
    static thread_local std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(0, 255);
    unsigned char b[16];
    for (auto& x : b) x = static_cast<unsigned char>(dist(rng));
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant 10xx
    char s[37];
    std::snprintf(s, sizeof(s),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(s);
}

} // namespace lpr
