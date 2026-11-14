// 层：数据
#include "trackinginput.h"
#include <paleo_data/service.h>
#include <cmath>

namespace paleo::tracking {
namespace {
double milliseconds(const paleo::data::SeismicSampling &axis) {
  return axis.timeUnit == "ms" ? 1.0 : axis.timeUnit == "s" ? 1000.0 : qQNaN();
}
}

Window readWindow(const ReadPlan &plan, int il, int xl, int firstSample, int sampleCount,
                  const std::function<bool()> &cancelled) {
  Window result;
  const auto fail = [&](const QString &code, const QString &detail = {}) {
    result.code = code; result.detail = detail; result.samples.clear(); return result;
  };
  if (cancelled && cancelled()) return fail(QStringLiteral("cancelled"));
  if (plan.maxWindowSamples < 1 || plan.maxWindowSamples > 4096 || firstSample < 0 ||
      sampleCount < 1 || sampleCount > plan.maxWindowSamples)
    return fail(QStringLiteral("tracking_window_budget"));
  const auto &selection = plan.interpretation;
  if (!selection.context || !selection.context->targetVolume || !selection.context->valid())
    return fail(QStringLiteral("tracking_volume_required"));
  const auto &volumeRef = *selection.context->targetVolume;
  const auto &source = selection.volume;
  if (source.ref != volumeRef.source || source.modelDirectory.isEmpty())
    return fail(QStringLiteral("source_revision_changed"));
  QString reference;
  const QString invalid = document::validateInterpretationSelection(source.modelDirectory, selection, &reference);
  if (!invalid.isEmpty()) return fail(QStringLiteral("tracking_context_invalid"), invalid);
  paleo::data::DataService service; std::string detail;
  if (!service.openProjectReadOnly(source.modelDirectory.toUtf8().toStdString(), detail))
    return fail(QStringLiteral("tracking_model_unavailable"), QString::fromStdString(detail));
  paleo::data::SeismicVolume volume;
  if (!service.readVolume(volumeRef.id, volume, detail))
    return fail(QStringLiteral("tracking_volume_unavailable"), QString::fromStdString(detail));
  const double scale = milliseconds(volume.sampling);
  if (!std::isfinite(scale) || !std::isfinite(volume.sampling.firstTime) ||
      !std::isfinite(volume.sampling.sampleInterval) || volume.sampling.sampleInterval <= 0 ||
      volume.sampling.sampleCount <= firstSample || sampleCount > volume.sampling.sampleCount - firstSample)
    return fail(QStringLiteral("tracking_axis_invalid"));
  paleo::data::SeismicTraceReadOptions options;
  options.firstSample = firstSample; options.sampleCount = sampleCount; options.maxSamples = plan.maxWindowSamples;
  paleo::data::SeismicTraceResult trace;
  if (!service.readSeismicTraceAtGrid(volumeRef.id, il, xl, options, trace, detail, cancelled))
    return fail(QStringLiteral("tracking_trace_unavailable"), QString::fromStdString(detail));
  const double interval = volume.sampling.sampleInterval * scale;
  const double expectedFirst = (volume.sampling.firstTime + firstSample * volume.sampling.sampleInterval) * scale;
  if (trace.timeUnit != "ms" || trace.inlineNumber != il || trace.crosslineNumber != xl ||
      trace.amplitudes.size() != size_t(sampleCount) || !std::isfinite(trace.firstTime) ||
      !std::isfinite(trace.sampleInterval) || std::abs(trace.firstTime - expectedFirst) > 1e-8 ||
      std::abs(trace.sampleInterval - interval) > 1e-8)
    return fail(QStringLiteral("tracking_axis_changed"));
  if (cancelled && cancelled()) return fail(QStringLiteral("cancelled"));
  const auto changed = document::validateInterpretationSelection(source.modelDirectory, selection, nullptr);
  if (!changed.isEmpty()) return fail(QStringLiteral("tracking_context_changed"), changed);
  result.samples = QVector<float>(trace.amplitudes.begin(), trace.amplitudes.end());
  result.traceOrdinal = trace.traceIndex; result.firstTimeMs = trace.firstTime;
  result.sampleIntervalMs = interval; result.timeReference = reference;
  result.ok = true; result.code = QStringLiteral("ok"); return result;
}

} // namespace paleo::tracking
