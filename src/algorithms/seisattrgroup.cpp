// 层：数据
// algorithms/seisattrgroup.cpp — 见 seisattrgroup.h。
#include "seisattrgroup.h"

#include "dsp/fft.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::seisattr {
namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

void fillNan(float *out, int n) {
  if (!out || n <= 0)
    return;
  std::fill(out, out + n, kNan);
}

std::int64_t envelopeMinimumBytes(int nSamples) {
  if (nSamples <= 0)
    return 0;
  const int nFft = paleo::dsp::nextPowerOfTwoAtLeast(std::max(2 * nSamples, 1));
  // 输入道 + 解析信号实部/虚部 + 输出，以及至少 2n 的 FFT 草稿。下界故意高于“只输出窗”。
  return static_cast<std::int64_t>(nSamples) * static_cast<std::int64_t>(sizeof(float)) * 4 +
         static_cast<std::int64_t>(nFft) * static_cast<std::int64_t>(sizeof(double)) * 2;
}

bool checkedMul(std::int64_t a, std::int64_t b, std::int64_t *out) {
  if (!out || a < 0 || b < 0)
    return false;
  if (a != 0 && b > std::numeric_limits<std::int64_t>::max() / a)
    return false;
  *out = a * b;
  return true;
}

bool checkedAdd(std::int64_t a, std::int64_t b, std::int64_t *out) {
  if (!out || a < 0 || b < 0)
    return false;
  if (a > std::numeric_limits<std::int64_t>::max() - b)
    return false;
  *out = a + b;
  return true;
}

AttributeBudgetPlan coherenceTooLarge(std::int64_t budgetBytes) {
  AttributeBudgetPlan plan;
  plan.shape = "finite-halo";
  plan.code = "budget_exceeded";
  plan.minimumBytes = 1;
  plan.estimatedBytes = budgetBytes > 0 && budgetBytes < std::numeric_limits<std::int64_t>::max() ? budgetBytes + 1
                                                                                                  : budgetBytes;
  return plan;
}

} // namespace

bool volumeIndexCount(int nIl, int nXl, int nS, int *count) {
  if (!count || nIl <= 0 || nXl <= 0 || nS <= 0)
    return false;
  std::int64_t plane = 0;
  std::int64_t product = 0;
  if (!checkedMul(nIl, nXl, &plane) || !checkedMul(plane, nS, &product))
    return false;
  if (product > static_cast<std::int64_t>(std::numeric_limits<int>::max()))
    return false;
  *count = static_cast<int>(product);
  return true;
}

void computeTraceAttribute(TraceAttribute kind, const float *trace, int n, int halfWindow,
                           double sampleIntervalMs, float *out) {
  if (!out || n <= 0)
    return;
  fillNan(out, n);
  if (!trace)
    return;
  switch (kind) {
  case TraceAttribute::Envelope:
  case TraceAttribute::Phase:
  case TraceAttribute::Frequency:
  case TraceAttribute::Quality:
  case TraceAttribute::Sweetness: {
    const ComplexTraceResult analytic = complexTraceAnalysis(trace, n, sampleIntervalMs);
    if (kind == TraceAttribute::Envelope)
      std::copy(analytic.envelope.begin(), analytic.envelope.end(), out);
    else if (kind == TraceAttribute::Phase)
      std::copy(analytic.phaseDeg.begin(), analytic.phaseDeg.end(), out);
    else if (kind == TraceAttribute::Frequency)
      std::copy(analytic.freqHz.begin(), analytic.freqHz.end(), out);
    else if (kind == TraceAttribute::Quality)
      std::copy(analytic.quality.begin(), analytic.quality.end(), out);
    else
      sweetness(analytic.envelope.data(), analytic.freqHz.data(), n, out);
    return;
  }
  case TraceAttribute::Rms:
    windowedRms(trace, n, halfWindow, out);
    return;
  case TraceAttribute::MaxAbs:
    windowedMaxAbs(trace, n, halfWindow, out);
    return;
  case TraceAttribute::MeanEnergy:
    windowedMeanEnergy(trace, n, halfWindow, out);
    return;
  }
}

TraceGroupResult computeAttributeGroup(const TraceGroupRequest &request) {
  TraceGroupResult result;
  if (request.nTraces < 0 || request.traceLength <= 0 || request.stride < request.traceLength ||
      !request.traces || !(request.sampleIntervalMs > 0.0) || request.rmsHalf < 0) {
    result.code = "invalid_shape";
    return result;
  }
  if ((request.wantEnvelope || request.wantPhase) && !request.fullTrace) {
    result.code = "whole_trace_required";
    return result;
  }
  if (!request.wantRms && !request.wantEnvelope && !request.wantPhase) {
    result.code = "no_attribute";
    return result;
  }
  const int begin = request.outputBegin < 0 ? 0 : request.outputBegin;
  int end = request.outputEnd <= begin ? request.traceLength : request.outputEnd;
  if (begin >= request.traceLength || end > request.traceLength) {
    result.code = "output_window";
    return result;
  }
  const int nOut = end - begin;
  result.nOut = nOut;
  result.rmsHalfApplied = request.wantRms ? request.rmsHalf : 0;
  const int total = request.nTraces * nOut;
  if (request.wantRms)
    result.rms.assign(static_cast<std::size_t>(total), kNan);
  if (request.wantEnvelope)
    result.envelope.assign(static_cast<std::size_t>(total), kNan);
  if (request.wantPhase)
    result.phase.assign(static_cast<std::size_t>(total), kNan);

  std::vector<float> rmsTrace(static_cast<std::size_t>(request.traceLength));
  for (int traceIndex = 0; traceIndex < request.nTraces; ++traceIndex) {
    const float *trace = request.traces + static_cast<std::ptrdiff_t>(traceIndex) * request.stride;
    result.samplesRead += request.traceLength;
    if (request.wantEnvelope || request.wantPhase) {
      const ComplexTraceResult analytic = complexTraceAnalysis(trace, request.traceLength, request.sampleIntervalMs);
      ++result.analyticEvaluations;
      for (int i = 0; i < nOut; ++i) {
        const std::size_t dst = static_cast<std::size_t>(traceIndex * nOut + i);
        const std::size_t src = static_cast<std::size_t>(begin + i);
        if (request.wantEnvelope)
          result.envelope[dst] = analytic.envelope[src];
        if (request.wantPhase)
          result.phase[dst] = analytic.phaseDeg[src];
      }
    }
    if (request.wantRms) {
      windowedRms(trace, request.traceLength, request.rmsHalf, rmsTrace.data());
      for (int i = 0; i < nOut; ++i)
        result.rms[static_cast<std::size_t>(traceIndex * nOut + i)] = rmsTrace[static_cast<std::size_t>(begin + i)];
    }
  }
  result.ok = true;
  result.code = "ok";
  return result;
}

bool rmsInteriorFromHalo(const float *trace, int n, int half, int begin, int end, float *out) {
  if (!trace || !out || n <= 0 || half < 0 || begin < 0 || end < begin || end > n)
    return false;
  if (begin == end)
    return true;
  const int lo = begin - half;
  const int hi = end + half;
  if (lo < 0 || hi > n)
    return false;
  const int len = hi - lo;
  std::vector<float> buffer(static_cast<std::size_t>(len));
  std::vector<float> rms(static_cast<std::size_t>(len));
  std::copy(trace + lo, trace + hi, buffer.begin());
  windowedRms(buffer.data(), len, half, rms.data());
  std::copy(rms.begin() + half, rms.begin() + half + (end - begin), out);
  return true;
}

CoherenceTileResult coherenceInterior(const CoherenceTile &tile, CoherenceWeightMode mode) {
  CoherenceTileResult result;
  if (!tile.volume || tile.nIl <= 0 || tile.nXl <= 0 || tile.nS <= 0 || tile.ilHalf < 0 || tile.xlHalf < 0 ||
      tile.timeHalf < 0) {
    result.code = "invalid_shape";
    return result;
  }
  int count = 0;
  if (!volumeIndexCount(tile.nIl, tile.nXl, tile.nS, &count)) {
    result.code = "invalid_shape";
    return result;
  }
  std::vector<float> full(static_cast<std::size_t>(count), kNan);
  semblanceCoherenceWeighted(tile.volume, tile.nIl, tile.nXl, tile.nS, tile.ilHalf, tile.xlHalf, tile.timeHalf,
                             tile.ilSpacing, tile.xlSpacing, mode, full.data(), {});
  const int i0 = tile.ilHalf;
  const int i1 = tile.nIl - tile.ilHalf;
  const int x0 = tile.xlHalf;
  const int x1 = tile.nXl - tile.xlHalf;
  if (i1 <= i0 || x1 <= x0) {
    result.code = "no_interior";
    result.samplesRead = count;
    return result;
  }
  result.interiorIl = i1 - i0;
  result.interiorXl = x1 - x0;
  result.nS = tile.nS;
  result.interior.resize(static_cast<std::size_t>(result.interiorIl * result.interiorXl * tile.nS));
  int dst = 0;
  for (int il = i0; il < i1; ++il) {
    for (int xl = x0; xl < x1; ++xl) {
      const int src = ((il * tile.nXl) + xl) * tile.nS;
      std::copy(full.begin() + src, full.begin() + src + tile.nS, result.interior.begin() + dst);
      dst += tile.nS;
    }
  }
  result.samplesRead = count;
  result.ok = true;
  result.code = "ok";
  return result;
}

std::vector<CoherenceXlSpan> coherenceXlSpans(int nXl, int xlHalf, int interiorChunk) {
  std::vector<CoherenceXlSpan> spans;
  if (nXl <= 0 || interiorChunk <= 0 || xlHalf < 0)
    return spans;
  const int x0 = xlHalf;
  const int x1 = nXl - xlHalf;
  if (x1 <= x0)
    return spans;
  for (int x = x0; x < x1; x += interiorChunk) {
    CoherenceXlSpan span;
    const int xEnd = std::min(x + interiorChunk, x1);
    span.interiorXl0 = x - x0;
    span.sourceXl0 = x - xlHalf;
    span.sourceXl1 = xEnd + xlHalf;
    spans.push_back(span);
  }
  return spans;
}

CoherenceTileResult computeCoherenceTiled(const CoherenceTile &volume, CoherenceWeightMode mode, int interiorXlChunk,
                                          const std::function<bool()> &cancelled) {
  if (!volume.volume || interiorXlChunk <= 0)
    return coherenceInterior(volume, mode);
  const int i0 = volume.ilHalf;
  const int i1 = volume.nIl - volume.ilHalf;
  const auto spans = coherenceXlSpans(volume.nXl, volume.xlHalf, interiorXlChunk);
  if (i1 <= i0 || spans.empty())
    return coherenceInterior(volume, mode);
  CoherenceTileResult combined;
  combined.interiorIl = i1 - i0;
  combined.interiorXl = volume.nXl - 2 * volume.xlHalf;
  combined.nS = volume.nS;
  combined.interior.assign(static_cast<std::size_t>(combined.interiorIl * combined.interiorXl * volume.nS), kNan);
  for (const CoherenceXlSpan &span : spans) {
    if (cancelled && cancelled()) {
      combined.ok = false;
      combined.code = "cancelled";
      combined.interior.clear();
      return combined;
    }
    const int tileNXl = span.sourceXl1 - span.sourceXl0;
    std::vector<float> slab(static_cast<std::size_t>(volume.nIl * tileNXl * volume.nS));
    for (int il = 0; il < volume.nIl; ++il) {
      for (int xl = span.sourceXl0; xl < span.sourceXl1; ++xl) {
        const int src = ((il * volume.nXl) + xl) * volume.nS;
        const int dst = ((il * tileNXl) + (xl - span.sourceXl0)) * volume.nS;
        std::copy(volume.volume + src, volume.volume + src + volume.nS, slab.begin() + dst);
      }
    }
    CoherenceTile tile = volume;
    tile.volume = slab.data();
    tile.nXl = tileNXl;
    const CoherenceTileResult part = coherenceInterior(tile, mode);
    if (!part.ok) {
      combined.ok = false;
      combined.code = part.code;
      combined.interior.clear();
      return combined;
    }
    for (int il = 0; il < part.interiorIl; ++il) {
      for (int xl = 0; xl < part.interiorXl; ++xl) {
        const int src = ((il * part.interiorXl) + xl) * volume.nS;
        const int dst = ((il * combined.interiorXl) + (span.interiorXl0 + xl)) * volume.nS;
        std::copy(part.interior.begin() + src, part.interior.begin() + src + volume.nS, combined.interior.begin() + dst);
      }
    }
    combined.samplesRead += part.samplesRead;
  }
  combined.ok = true;
  combined.code = "ok";
  return combined;
}

AttributeBudgetPlan planAttributeBudget(int nTraces, int nSamples, int rmsHalf, bool envelope, bool rms,
                                        bool coherence, int nIl, int nXl, int ilHalf, int xlHalf,
                                        std::int64_t budgetBytes, int volumeSamples) {
  AttributeBudgetPlan plan;
  if (budgetBytes <= 0) {
    plan.code = "budget_exceeded";
    plan.minimumBytes = 1;
    return plan;
  }
  AttributeBudgetPlan coherencePlan;
  if (coherence) {
    const int samples = volumeSamples > 0 ? volumeSamples : nSamples;
    int count = 0;
    if (!volumeIndexCount(nIl, nXl, samples, &count))
      return coherenceTooLarge(budgetBytes);
    const int spanIl = std::max(1, 2 * ilHalf + 1);
    const int spanXl = std::max(1, 2 * xlHalf + 1);
    std::int64_t cell = 0;
    std::int64_t window = 0;
    std::int64_t full = 0;
    if (!checkedMul(samples, static_cast<std::int64_t>(sizeof(float)) * 2, &cell) ||
        !checkedMul(cell, spanIl, &window) || !checkedMul(window, spanXl, &window) || !checkedMul(cell, nIl, &full) ||
        !checkedMul(full, nXl, &full))
      return coherenceTooLarge(budgetBytes);
    std::int64_t perXl = 0;
    std::int64_t xlWindow = 0;
    if (!checkedMul(cell, nIl, &perXl) || !checkedMul(perXl, spanXl, &xlWindow))
      return coherenceTooLarge(budgetBytes);
    coherencePlan.shape = "finite-halo";
    coherencePlan.minimumBytes = std::max(window, xlWindow);
    if (coherencePlan.minimumBytes > budgetBytes) {
      coherencePlan.code = "budget_exceeded";
      coherencePlan.estimatedBytes = std::max(coherencePlan.minimumBytes, full);
      return coherencePlan;
    }
    if (full > budgetBytes) {
      const std::int64_t perXlBytes = cell * static_cast<std::int64_t>(std::max(nIl, 1));
      int tileXl = perXlBytes > 0 ? static_cast<int>(std::min<std::int64_t>(nXl, budgetBytes / perXlBytes)) : 0;
      tileXl = std::max(spanXl, tileXl);
      int interiorChunk = std::max(1, tileXl - 2 * std::max(xlHalf, 0));
      const int interiorXl = std::max(0, nXl - 2 * std::max(xlHalf, 0));
      std::int64_t tileBytes = 0;
      if (interiorXl <= 0 || interiorChunk >= interiorXl || !checkedMul(perXlBytes, tileXl, &tileBytes) ||
          tileBytes > budgetBytes) {
        coherencePlan.code = "budget_exceeded";
        coherencePlan.estimatedBytes = tileBytes > 0 ? std::max(coherencePlan.minimumBytes, tileBytes) : full;
        return coherencePlan;
      }
      coherencePlan.xlChunk = interiorChunk;
      coherencePlan.estimatedBytes = std::max(coherencePlan.minimumBytes, tileBytes);
      coherencePlan.code = "split";
      if (!envelope && !rms)
        return coherencePlan;
    } else {
      coherencePlan.xlChunk = std::max(1, nXl - 2 * std::max(xlHalf, 0));
      coherencePlan.estimatedBytes = full;
      coherencePlan.code = "ok";
      if (!envelope && !rms)
        return coherencePlan;
    }
  }
  const auto attachCoherence = [&](AttributeBudgetPlan trace) {
    if (!coherence)
      return trace;
    std::int64_t sum = 0;
    if (!checkedAdd(trace.estimatedBytes, coherencePlan.estimatedBytes, &sum) || sum > budgetBytes) {
      trace.code = "budget_exceeded";
      trace.estimatedBytes = sum > 0 ? sum : coherencePlan.estimatedBytes;
      trace.minimumBytes = std::max(trace.minimumBytes, coherencePlan.minimumBytes);
      return trace;
    }
    trace.estimatedBytes = sum;
    trace.minimumBytes = std::max(trace.minimumBytes, coherencePlan.minimumBytes);
    trace.xlChunk = coherencePlan.xlChunk;
    return trace;
  };
  if (envelope) {
    if (nSamples <= 0)
      return coherence ? coherenceTooLarge(budgetBytes) : AttributeBudgetPlan{"budget_exceeded", "whole-trace", 1, 1, 0, 0};
    plan.shape = "whole-trace";
    const std::int64_t minimum = envelopeMinimumBytes(nSamples);
    plan.minimumBytes = minimum;
    if (minimum > budgetBytes || minimum <= 0) {
      plan.code = "budget_exceeded";
      plan.estimatedBytes = std::max<std::int64_t>(minimum, 1);
      return plan;
    }
    const int batch = static_cast<int>(std::min<std::int64_t>(std::max(nTraces, 0), std::max<std::int64_t>(1, budgetBytes / minimum)));
    plan.traceBatch = std::max(batch, 1);
    plan.estimatedBytes = minimum * static_cast<std::int64_t>(plan.traceBatch);
    plan.code = (nTraces > plan.traceBatch) ? "split" : "ok";
    return attachCoherence(plan);
  }
  if (rms) {
    if (nSamples <= 0)
      return coherence ? coherenceTooLarge(budgetBytes) : AttributeBudgetPlan{"budget_exceeded", "finite-halo", 1, 1, 0, 0};
    plan.shape = "finite-halo";
    const int halo = std::max(0, rmsHalf);
    const std::int64_t minimum = static_cast<std::int64_t>(nSamples + 2 * halo) * static_cast<std::int64_t>(sizeof(float));
    plan.minimumBytes = minimum;
    if (minimum > budgetBytes || minimum <= 0) {
      plan.code = "budget_exceeded";
      plan.estimatedBytes = std::max<std::int64_t>(minimum, 1);
      return plan;
    }
    const int batch = static_cast<int>(std::min<std::int64_t>(std::max(nTraces, 1), budgetBytes / minimum));
    plan.traceBatch = batch;
    plan.estimatedBytes = minimum * static_cast<std::int64_t>(batch);
    plan.code = nTraces > batch ? "split" : "ok";
    return attachCoherence(plan);
  }
  if (coherence)
    return coherencePlan;
  plan.code = "no_attribute";
  return plan;
}

} // namespace paleo::seisattr
