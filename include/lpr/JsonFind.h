#pragma once
#include <nlohmann/json.hpp>
#include <initializer_list>

namespace lpr {

// Recursively search a JSON value for the first node whose key equals `key`.
// Used to tolerate differing nesting/casing in backend command payloads.
inline const nlohmann::json* findKeyDeep(const nlohmann::json& j, const std::string& key) {
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end()) return &(*it);
        for (auto& item : j.items()) {
            if (const nlohmann::json* r = findKeyDeep(item.value(), key)) return r;
        }
    } else if (j.is_array()) {
        for (const auto& v : j) {
            if (const nlohmann::json* r = findKeyDeep(v, key)) return r;
        }
    }
    return nullptr;
}

// Try several candidate keys, returning the first that exists anywhere in the payload.
inline const nlohmann::json* findAnyKeyDeep(const nlohmann::json& j,
                                            std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (const nlohmann::json* r = findKeyDeep(j, k)) return r;
    }
    return nullptr;
}

} // namespace lpr
