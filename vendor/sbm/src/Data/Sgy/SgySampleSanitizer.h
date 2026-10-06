#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace seismic {
// Paleo P13: preserve all finite amplitudes and the existing NaN missing-data
// convention. The caller owns quality accounting and reporting.
inline std::uint64_t SanitizeSgySamples(float* samples, std::size_t count) {
    std::uint64_t sanitized = 0;
    for(std::size_t i = 0; i < count; ++i) {
        if(!std::isfinite(samples[i])) {
            samples[i] = std::numeric_limits<float>::quiet_NaN();
            ++sanitized;
        }
    }
    return sanitized;
}
} // namespace seismic
