// 层：数据
#pragma once
#include "documentrelationresolver.h"
#include <QVector>
#include <functional>

namespace paleo::tracking {

// Immutable document/model values. Construction and use do not retain widgets,
// DocumentSession, SQL connections, or QGIS objects across worker boundaries.
struct ReadPlan {
  document::InterpretationSelection interpretation;
  int maxWindowSamples = 4096;
};
struct Window {
  bool ok = false;
  QString code, detail, timeReference;
  double firstTimeMs = 0, sampleIntervalMs = 0;
  qlonglong traceOrdinal = -1;
  QVector<float> samples;
};

Window readWindow(const ReadPlan &plan, int inlineNumber, int crosslineNumber,
                  int firstSample, int sampleCount,
                  const std::function<bool()> &cancelled = {});

} // namespace paleo::tracking
