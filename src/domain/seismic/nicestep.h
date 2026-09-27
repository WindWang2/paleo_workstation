#pragma once

#include <QString>
#include <vector>

namespace seismic {

struct AxisTick {
    double value = 0.0;     // World value (e.g. meters, ms, trace number)
    double pixelPos = 0.0;  // Pixel coordinate on canvas/ruler
    bool isMajor = true;    // Major tick (with label) or minor tick
    QString label;          // Text formatted in JetBrains Mono
};

// Pure algorithmic helper for computing "nice" human-readable axis intervals (1, 2, 5 * 10^n)
// and generating ticks within a visible viewport range.
class NiceStep {
public:
    // Calculates a "nice" step interval for the given range and target tick count.
    static double Calculate(double range, int targetTicks = 8);

    // Generates major and minor ticks for [minVal, maxVal] mapped linearly to [pixelMin, pixelMax].
    static std::vector<AxisTick> GenerateTicks(
        double minVal, double maxVal,
        double pixelMin, double pixelMax,
        int targetTicks = 8,
        const QString &format = QStringLiteral("%.0f"),
        bool includeMinor = true);
};

} // namespace seismic
