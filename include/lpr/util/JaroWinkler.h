#pragma once
#include <string>

namespace lpr {

constexpr double        JARO_WEIGHT_STRING_A         = 1.0 / 3.0;
constexpr double        JARO_WEIGHT_STRING_B         = 1.0 / 3.0;
constexpr double        JARO_WEIGHT_TRANSPOSITIONS   = 1.0 / 3.0;
constexpr unsigned long JARO_WINKLER_PREFIX_SIZE     = 4;
constexpr double        JARO_WINKLER_SCALING_FACTOR  = 0.1;
constexpr double        JARO_WINKLER_BOOST_THRESHOLD = 0.7;

double jaroDistance(const std::string& a, const std::string& b);
double jaroWinklerDistance(const std::string& a, const std::string& b);

} // namespace lpr
