// 层：数据
#pragma once

#include "domain/documentrelation.h"

#include <functional>
#include <vector>

namespace paleo::capability {

struct SeismicTraceSelection {
  document::ModelObjectRef volume;
  qlonglong traceIndex = -1; // File ordinal, not the SEG-Y trace number.
  int firstSample = 0;
  int sampleCount = 0;
};

struct ResolvedSeismicTraceInput {
  document::SourceRecord source;
  SeismicTraceSelection selection;
};

struct SeismicTraceInputPlan {
  bool ok = false;
  QString code;
  int firstReadSample = 0;
  int readSampleCount = 0;
  int outputBegin = 0, outputEnd = 0; // Half-open window in the read buffer.
  bool fullTrace = false;
  int nativeSampleCount = 0;
  double firstReadTimeMs = 0;
  double sampleIntervalMs = 0;
};

// Reads only model metadata. The host can reserve memory using the exact read
// shape before opening the source. Run with a thread-local SDK connection.
SeismicTraceInputPlan planSeismicTraceInput(const document::SourceRecord &source,
                                           const SeismicTraceSelection &selection,
                                           int rmsHalf, bool requiresWholeTrace,
                                           int maxReadSamples,
                                           const std::function<bool()> &cancelled = {});

// Returns an empty code on success. No samples escape if the source, object
// revision, or actual sample axis changed. Run after reserving the budget.
QString readSeismicTraceInput(const document::SourceRecord &source,
                              const SeismicTraceSelection &selection,
                              const SeismicTraceInputPlan &plan, std::vector<float> &out,
                              const std::function<bool()> &cancelled = {});

} // namespace paleo::capability
