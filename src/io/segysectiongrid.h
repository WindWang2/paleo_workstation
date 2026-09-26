#pragma once

#include "segyreader.h"

#include <QtGlobal>
#include <cmath>
#include <limits>

// Shared time axis for section previews. Each trace keeps its own sample
// interval and delay; missing portions render as neutral background.
struct SegySectionGrid
{
  double startMs = 0.0;
  double stepMs = 1.0;
  int rows = 1;

  double endMs() const { return startMs + (rows - 1) * stepMs; }

  static SegySectionGrid forTraces(const QVector<SegyTrace> &traces, float fallbackDtUs,
                                   double fallbackStartMs)
  {
    SegySectionGrid grid;
    double first = std::numeric_limits<double>::infinity();
    double last = -std::numeric_limits<double>::infinity();
    double smallestStep = std::numeric_limits<double>::infinity();
    for (const SegyTrace &trace : traces)
    {
      if (trace.samples.isEmpty()) continue;
      const double dt = (trace.sampleIntervalUs > 0 ? trace.sampleIntervalUs : fallbackDtUs) / 1000.0;
      if (dt <= 0 || !std::isfinite(dt)) continue;
      const double start = std::isfinite(trace.startTimeMs) ? trace.startTimeMs : fallbackStartMs;
      first = qMin(first, start);
      last = qMax(last, start + (trace.samples.size() - 1) * dt);
      smallestStep = qMin(smallestStep, dt);
    }
    if (!std::isfinite(first)) return grid;
    grid.startMs = first;
    const double span = last - first;
    const double requiredRows = std::ceil(span / smallestStep) + 1.0;
    grid.rows = qBound(1, static_cast<int>(qMin(8192.0, requiredRows)), 8192);
    grid.stepMs = grid.rows > 1 ? span / (grid.rows - 1) : smallestStep;
    return grid;
  }

  bool sampleAt(const SegyTrace &trace, int row, float fallbackDtUs,
                double fallbackStartMs, float *sample) const
  {
    if (!sample || trace.samples.isEmpty()) return false;
    const double dt = (trace.sampleIntervalUs > 0 ? trace.sampleIntervalUs : fallbackDtUs) / 1000.0;
    if (dt <= 0 || !std::isfinite(dt)) return false;
    const double traceStart = std::isfinite(trace.startTimeMs) ? trace.startTimeMs : fallbackStartMs;
    const double position = (startMs + row * stepMs - traceStart) / dt;
    if (position < 0 || position > trace.samples.size() - 1) return false;
    const int lo = static_cast<int>(position);
    const int hi = qMin(lo + 1, trace.samples.size() - 1);
    *sample = trace.samples.at(lo) * static_cast<float>(1.0 - (position - lo)) +
              trace.samples.at(hi) * static_cast<float>(position - lo);
    return true;
  }
};
