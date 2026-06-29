#pragma once
// LprSettings - the LPR-wide settings received as a JSON array of {name, value}.
// (Cleaned: removed the ABI-fingerprint / mutex-offset debug probes.)
#include "lpr/config/JsonAbi.h"   // pin json ABI macros before json.hpp
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <mutex>

namespace lpr {

class LprSettings {
public:
    using json = nlohmann::json;

    // Load settings from a JSON array of objects each having "name" and "value".
    void load(const json& settingsArray);

    // Access a setting by key; std::nullopt if missing or the cast fails.
    template <typename T>
    std::optional<T> get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = settingsMap_.find(key);
        if (it == settingsMap_.end())
            return std::nullopt;
        try {
            return it.value().template get<T>();
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    json settingsMap_;            // key -> value
    mutable std::mutex mutex_;
};

} // namespace lpr
