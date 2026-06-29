#pragma once
// JsonAbi.h (was JsonConfig.h) - single source of truth for nlohmann::json ABI macros.
// Include BEFORE <nlohmann/json.hpp> in every TU that touches json, OR (preferred)
// set these as project-wide compile definitions in CMake. Mismatched values make
// json (and any type containing one) a different size/layout per TU -> memory
// corruption (classic symptom: a crash when locking a std::mutex member).
#ifndef JSON_DIAGNOSTICS
#define JSON_DIAGNOSTICS 0
#endif
#ifndef JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON
#define JSON_USE_LEGACY_DISCARDED_VALUE_COMPARISON 0
#endif
