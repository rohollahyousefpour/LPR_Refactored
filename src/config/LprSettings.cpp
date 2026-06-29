#include "lpr/config/JsonAbi.h"   // FIRST: pin json ABI before json.hpp
#include "lpr/config/LprSettings.h"

namespace lpr {

void LprSettings::load(const json& settingsArray) {
    std::lock_guard<std::mutex> lock(mutex_);
    settingsMap_.clear();

    if (!settingsArray.is_array())
        return;

    for (const auto& setting : settingsArray) {
        if (setting.contains("name") && setting.contains("value"))
            settingsMap_[setting["name"].get<std::string>()] = setting["value"];
    }
}

} // namespace lpr
