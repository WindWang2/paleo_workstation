#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace seismic {

// One survey axis (inline, xline or sample).
//
// A regular axis is stored as origin + step + count (this is what a step-2
// survey needs); an irregular or sparse axis stores its sorted unique values.
//
// ExactIndexOf never snaps: a missing coordinate is reported as missing.
// NearestIndexOf exists only for explicit UI snapping (e.g. placing a well) and
// must never be used by data reads.
struct AxisDescriptor {
    enum class Encoding {
        Uniform = 0,  // origin + step * index
        Explicit = 1, // sorted unique values
    };

    Encoding encoding = Encoding::Uniform;
    int count = 0;
    int origin = 0;
    int step = 0;
    std::vector<int> values;

    static AxisDescriptor Uniform(int originValue, int stepValue, int countValue) {
        AxisDescriptor axis;
        axis.encoding = Encoding::Uniform;
        axis.origin = originValue;
        axis.step = stepValue;
        axis.count = std::max(0, countValue);
        return axis;
    }

    static AxisDescriptor Explicit(std::vector<int> sortedUnique) {
        AxisDescriptor axis;
        axis.encoding = Encoding::Explicit;
        axis.values = std::move(sortedUnique);
        std::sort(axis.values.begin(), axis.values.end());
        axis.values.erase(std::unique(axis.values.begin(), axis.values.end()), axis.values.end());
        axis.count = static_cast<int>(axis.values.size());
        if(!axis.values.empty()) {
            axis.origin = axis.values.front();
            axis.step = axis.count > 1 ? axis.values[1] - axis.values[0] : 0;
        }
        return axis;
    }

    // Sorted unique values; when they form a uniform progression the compact
    // origin/step/count form is used (a step-2 survey stays a uniform axis).
    static AxisDescriptor FromValues(std::vector<int> sortedUnique) {
        AxisDescriptor axis = Explicit(std::move(sortedUnique));
        if(axis.count <= 1 || axis.values.size() <= 1) {
            return axis;
        }
        const int candidateStep = axis.values[1] - axis.values[0];
        if(candidateStep == 0) {
            return axis;
        }
        for(std::size_t i = 2; i < axis.values.size(); ++i) {
            if(axis.values[i] - axis.values[i - 1] != candidateStep) {
                return axis;
            }
        }
        return Uniform(axis.values.front(), candidateStep, axis.count);
    }

    bool Empty() const { return count <= 0; }

    bool Validate() const {
        if(count < 0) {
            return false;
        }
        if(encoding == Encoding::Uniform) {
            return count == 0 || step != 0;
        }
        if(static_cast<int>(values.size()) != count) {
            return false;
        }
        return std::is_sorted(values.begin(), values.end()) &&
               std::adjacent_find(values.begin(), values.end()) == values.end();
    }

    // True when the axis can be addressed as origin + step * index.
    bool HasUniformStep() const {
        if(encoding == Encoding::Uniform) {
            return step != 0;
        }
        if(count <= 1) {
            return true;
        }
        const int first = values[1] - values[0];
        if(first == 0) {
            return false;
        }
        for(std::size_t i = 2; i < values.size(); ++i) {
            if(values[i] - values[i - 1] != first) {
                return false;
            }
        }
        return true;
    }

    int ValueAt(int index) const {
        if(index < 0 || index >= count) {
            return 0;
        }
        if(encoding == Encoding::Uniform) {
            return origin + index * step;
        }
        return values[static_cast<std::size_t>(index)];
    }

    // Exact membership. Returns false when the value is not a real axis element;
    // the caller must treat that as "missing", never as the neighbour.
    bool ExactIndexOf(int value, int& index) const {
        if(encoding == Encoding::Uniform) {
            if(step == 0) {
                return false;
            }
            const int offset = value - origin;
            if(offset % step != 0) {
                return false;
            }
            const int position = offset / step;
            if(position < 0 || position >= count) {
                return false;
            }
            index = position;
            return true;
        }
        const auto it = std::lower_bound(values.begin(), values.end(), value);
        if(it == values.end() || *it != value) {
            return false;
        }
        index = static_cast<int>(it - values.begin());
        return true;
    }

    // Nearest element. Explicit opt-in only (UI snapping); data reads must use
    // ExactIndexOf so a missing coordinate can never be silently replaced.
    bool NearestIndexOf(int value, int& index) const {
        if(count <= 0) {
            return false;
        }
        if(encoding == Encoding::Uniform) {
            if(step == 0) {
                return false;
            }
            // Same tie rule as the explicit branch: on an exact half-step the
            // lower neighbour wins, so snapping is deterministic.
            const double ratio = static_cast<double>(value - origin) / static_cast<double>(step);
            const int lower = static_cast<int>(std::floor(ratio));
            const int upper = lower + 1;
            const int clampedLower = std::clamp(lower, 0, count - 1);
            const int clampedUpper = std::clamp(upper, 0, count - 1);
            const long long lowerDistance = std::llabs(static_cast<long long>(value) -
                                                       (origin + clampedLower * step));
            const long long upperDistance = std::llabs(static_cast<long long>(value) -
                                                       (origin + clampedUpper * step));
            index = lowerDistance <= upperDistance ? clampedLower : clampedUpper;
            return true;
        }
        const auto it = std::lower_bound(values.begin(), values.end(), value);
        if(it == values.begin()) {
            index = 0;
            return true;
        }
        if(it == values.end()) {
            index = count - 1;
            return true;
        }
        const int upper = static_cast<int>(it - values.begin());
        const int lower = upper - 1;
        index = (value - values[static_cast<std::size_t>(lower)]) <=
                        (values[static_cast<std::size_t>(upper)] - value)
                    ? lower
                    : upper;
        return true;
    }

    std::string Describe() const {
        if(encoding == Encoding::Uniform) {
            return "uniform origin=" + std::to_string(origin) + " step=" + std::to_string(step) +
                   " count=" + std::to_string(count);
        }
        return "explicit count=" + std::to_string(count) +
               (values.empty() ? std::string()
                               : " [" + std::to_string(values.front()) + ".." +
                                     std::to_string(values.back()) + "]");
    }
};

} // namespace seismic
