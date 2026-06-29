#include "lpr/detect/CtcDecoder.h"

#include <algorithm>
#include <cmath>

namespace lpr {

std::string ctcGreedyDecode(const std::vector<float>& data, int timesteps, int numClasses,
                            const std::string& alphabet, int blankIndex, double* confidence) {
    // class index -> character (or -1 for blank). Supports blank-first/last/middle.
    auto classToChar = [&](int c) -> int {
        if (c == blankIndex) return -1;
        int idx = (c < blankIndex) ? c : c - 1;
        return (idx >= 0 && idx < static_cast<int>(alphabet.size())) ? alphabet[idx] : -1;
    };

    std::string out;
    int    prev    = -1;
    double logConf = 0.0;
    int    kept    = 0;

    for (int t = 0; t < timesteps; ++t) {
        const float* row = &data[static_cast<size_t>(t) * numClasses];

        int   amax = 0;
        float mx   = row[0];
        for (int c = 1; c < numClasses; ++c)
            if (row[c] > mx) { mx = row[c]; amax = c; }

        if (amax != blankIndex && amax != prev) {
            int ch = classToChar(amax);
            if (ch >= 0) {
                out += static_cast<char>(ch);
                double sum = 0.0;
                for (int c = 0; c < numClasses; ++c) sum += std::exp(row[c] - mx);
                logConf += std::log(std::max(1.0 / sum, 1e-9));
                ++kept;
            }
        }
        prev = amax;
    }

    if (confidence) *confidence = (kept > 0) ? std::exp(logConf / kept) : 0.0;
    return out;
}

} // namespace lpr
