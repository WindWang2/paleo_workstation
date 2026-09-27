#include "domain/seismic/nicestep.h"

#include <cmath>
#include <cstdio>
#include <algorithm>

namespace seismic {

double NiceStep::Calculate(double range, int targetTicks) {
    if (range <= 0.0 || targetTicks <= 0)
        return 1.0;

    const double rawStep = range / static_cast<double>(targetTicks);
    const double exponent = std::floor(std::log10(rawStep));
    const double powerOf10 = std::pow(10.0, exponent);
    const double fraction = rawStep / powerOf10;

    double niceFraction = 1.0;
    if (fraction < 1.5) {
        niceFraction = 1.0;
    } else if (fraction < 3.0) {
        niceFraction = 2.0;
    } else if (fraction < 7.0) {
        niceFraction = 5.0;
    } else {
        niceFraction = 10.0;
    }

    return niceFraction * powerOf10;
}

std::vector<AxisTick> NiceStep::GenerateTicks(
    double minVal, double maxVal,
    double pixelMin, double pixelMax,
    int targetTicks,
    const QString &format,
    bool includeMinor)
{
    std::vector<AxisTick> ticks;
    if (minVal >= maxVal || std::abs(pixelMax - pixelMin) < 1.0)
        return ticks;

    const double range = maxVal - minVal;
    const double majorStep = Calculate(range, targetTicks);
    if (majorStep <= 0.0)
        return ticks;

    const double firstMajor = std::floor(minVal / majorStep) * majorStep;
    const double lastMajor = std::ceil(maxVal / majorStep) * majorStep;
    const double valToPix = (pixelMax - pixelMin) / range;
    const double minorStep = majorStep / 5.0;

    const int stepCount = static_cast<int>(std::round((lastMajor - firstMajor) / minorStep)) + 1;
    for (int i = 0; i <= stepCount; ++i) {
        const double v = firstMajor + i * minorStep;
        if (v < minVal - 1e-9 || v > maxVal + 1e-9)
            continue;

        const double distFromMajor = std::abs(v - std::round(v / majorStep) * majorStep);
        const bool isMajor = (distFromMajor < minorStep * 0.25);

        if (!isMajor && !includeMinor)
            continue;

        AxisTick tick;
        tick.value = v;
        tick.pixelPos = pixelMin + (v - minVal) * valToPix;
        tick.isMajor = isMajor;
        if (isMajor) {
            char buf[64];
            snprintf(buf, sizeof(buf), format.toUtf8().constData(), std::abs(v) < 1e-9 ? 0.0 : v);
            tick.label = QString::fromUtf8(buf);
        }
        ticks.push_back(tick);
    }

    return ticks;
}

} // namespace seismic
