// 层：数据
#include "capabilityseismicinput.h"
#include "io/sourcefingerprint.h"

#include <paleo_data/object_descriptor.h>
#include <paleo_data/service.h>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <utility>

namespace paleo::capability {
namespace {

constexpr int kMaxTraceSamples = 1'000'000;

QString checkRevision(const document::ModelObjectRef &ref, data::DataService &service) {
  std::string error;
  data::SourceIndexEntry source;
  if (!service.readSourceIndexEntry(ref.source.sourceId, source, error) ||
      !source.active || QString::fromStdString(source.sha256) != ref.source.fileRevisionId)
    return QStringLiteral("source_revision_changed");
  data::DataObjectDescriptor object;
  if (!service.describeDataObject(data::DataObjectKind::SeismicVolume, ref.id, object, error) ||
      object.sourceId != source.id || object.wellId != 0)
    return QStringLiteral("object_revision_changed");
  if (!object.revisionKnown)
    return QStringLiteral("object_revision_upgrade_required");
  return QString::fromStdString(object.revision) == ref.revision
             ? QString() : QStringLiteral("object_revision_changed");
}

QString resolve(const document::SourceRecord &source, const SeismicTraceSelection &selection,
                data::DataService &service, data::SeismicVolume &volume) {
  const auto &ref = selection.volume;
  if (!ref.valid() || ref.kind != document::ObjectKind::SeismicVolume ||
      ref.source.modelStoreId != QLatin1String("area-model") || source.modelDirectory.isEmpty() ||
      selection.traceIndex < 0 || selection.firstSample < 0 || selection.sampleCount <= 0)
    return QStringLiteral("invalid_seismic_input");
  if (source.ref != ref.source || source.fingerprint.stored() != ref.source.fileRevisionId)
    return QStringLiteral("source_revision_changed");
  // The SDK verifies the source before reading a trace. A legacy full-file
  // digest would turn a bounded window into an unbounded verification scan.
  if (source.fingerprint.kind != document::FingerprintKind::SegyQuickV1)
    return QStringLiteral("source_verification_budget_exceeded");
  std::string error;
  if (!service.openProjectReadOnly(source.modelDirectory.toUtf8().toStdString(), error))
    return QStringLiteral("model_unavailable");
  const auto revisionError = checkRevision(ref, service);
  if (!revisionError.isEmpty())
    return revisionError;
  if (!service.readVolume(ref.id, volume, error))
    return QStringLiteral("seismic_sampling_unavailable");
  if (volume.geometry.traceCount >= 0 && selection.traceIndex >= volume.geometry.traceCount)
    return QStringLiteral("seismic_trace_out_of_range");
  return {};
}

double timeScale(const data::SeismicSampling &axis) {
  if (axis.timeUnit == "ms")
    return 1.0;
  if (axis.timeUnit == "s")
    return 1000.0;
  return 0.0;
}

bool sameTime(double a, double b) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-6;
}

bool matchesPlan(const SeismicTraceSelection &selection, const SeismicTraceInputPlan &plan,
                 const data::SeismicSampling &axis) {
  const qint64 readEnd = qint64(plan.firstReadSample) + plan.readSampleCount;
  const double scale = timeScale(axis);
  return plan.ok && plan.firstReadSample >= 0 && plan.readSampleCount > 0 &&
      plan.readSampleCount <= kMaxTraceSamples && readEnd <= axis.sampleCount &&
      plan.nativeSampleCount == axis.sampleCount && plan.outputBegin >= 0 &&
      plan.outputEnd <= plan.readSampleCount && plan.outputEnd > plan.outputBegin &&
      qint64(plan.firstReadSample) + plan.outputBegin == selection.firstSample &&
      plan.outputEnd - plan.outputBegin == selection.sampleCount &&
      plan.fullTrace == (plan.firstReadSample == 0 && readEnd == axis.sampleCount) &&
      scale > 0 && plan.sampleIntervalMs > 0 &&
      sameTime(plan.sampleIntervalMs, axis.sampleInterval * scale) &&
      sameTime(plan.firstReadTimeMs, axis.firstTime * scale + plan.firstReadSample * plan.sampleIntervalMs);
}

} // namespace

SeismicTraceInputPlan planSeismicTraceInput(const document::SourceRecord &source,
                                           const SeismicTraceSelection &selection,
                                           int rmsHalf, bool requiresWholeTrace, int maxReadSamples,
                                           const std::function<bool()> &cancelled) {
  SeismicTraceInputPlan plan;
  const auto fail = [&](const QString &code) { plan.code = code; return plan; };
  if (cancelled && cancelled())
    return fail(QStringLiteral("cancelled"));
  if (rmsHalf < 0)
    return fail(QStringLiteral("invalid_seismic_input"));
  if (maxReadSamples <= 0)
    return fail(QStringLiteral("budget_exceeded"));
  data::DataService service;
  data::SeismicVolume volume;
  const auto error = resolve(source, selection, service, volume);
  if (!error.isEmpty())
    return fail(error);
  if (cancelled && cancelled())
    return fail(QStringLiteral("cancelled"));
  const auto &axis = volume.sampling;
  const double scale = timeScale(axis);
  const double interval = axis.sampleInterval * scale;
  if (axis.sampleCount <= 0 || scale <= 0 || !std::isfinite(interval) || interval <= 0 ||
      !std::isfinite(axis.firstTime))
    return fail(QStringLiteral("seismic_sampling_unavailable"));
  const qint64 outputEnd = qint64(selection.firstSample) + selection.sampleCount;
  if (outputEnd > axis.sampleCount)
    return fail(QStringLiteral("seismic_window_out_of_range"));

  const auto readBegin = requiresWholeTrace ? 0 : std::max(0, selection.firstSample - rmsHalf);
  const auto readEnd = requiresWholeTrace ? axis.sampleCount
      : std::min<qint64>(axis.sampleCount, outputEnd + rmsHalf);
  const auto readCount = readEnd - readBegin;
  if (readCount > std::min(maxReadSamples, kMaxTraceSamples))
    return fail(QStringLiteral("budget_exceeded"));
  plan.firstReadSample = readBegin;
  plan.readSampleCount = int(readCount);
  plan.outputBegin = selection.firstSample - readBegin;
  plan.outputEnd = plan.outputBegin + selection.sampleCount;
  plan.fullTrace = readBegin == 0 && readEnd == axis.sampleCount;
  plan.nativeSampleCount = axis.sampleCount;
  plan.firstReadTimeMs = axis.firstTime * scale + readBegin * interval;
  plan.sampleIntervalMs = interval;
  if (!std::isfinite(plan.firstReadTimeMs))
    return fail(QStringLiteral("seismic_sampling_unavailable"));
  plan.ok = true;
  return plan;
}

QString readSeismicTraceInput(const document::SourceRecord &source,
                              const SeismicTraceSelection &selection,
                              const SeismicTraceInputPlan &plan, std::vector<float> &out,
                              const std::function<bool()> &cancelled) {
  out.clear();
  if (cancelled && cancelled())
    return QStringLiteral("cancelled");
  data::DataService service;
  data::SeismicVolume volume;
  const auto error = resolve(source, selection, service, volume);
  if (!error.isEmpty())
    return error;
  if (!matchesPlan(selection, plan, volume.sampling))
    return QStringLiteral("invalid_seismic_input");

  std::string registeredPath, detail;
  if (!service.sourcePath(selection.volume.source.sourceId, registeredPath, detail))
    return QStringLiteral("source_revision_changed");
  const auto path = QFileInfo(QString::fromStdString(registeredPath)).canonicalFilePath();
  if (path.isEmpty() || path != QFileInfo(QString::fromStdString(volume.dataFile)).canonicalFilePath())
    return QStringLiteral("source_revision_changed");
  const auto before = document::sourceFileStamp(path);
  if (cancelled && cancelled())
    return QStringLiteral("cancelled");

  data::SeismicTraceReadOptions options;
  options.firstSample = plan.firstReadSample;
  options.maxSamples = plan.readSampleCount;
  // A window ending at the declared trace end must also reach the actual end.
  // SDK count=0 means through EOF of this trace, with the exact cap enforced.
  options.sampleCount = plan.firstReadSample + plan.readSampleCount == plan.nativeSampleCount
      ? 0 : plan.readSampleCount;
  data::SeismicTraceResult trace;
  const bool read = service.readSeismicTrace(selection.volume.id, selection.traceIndex, options,
                                             trace, detail, cancelled);
  if (cancelled && cancelled())
    return QStringLiteral("cancelled");
  if (!read)
    return QStringLiteral("seismic_read_failed");
  if (document::sourceFileStamp(path) != before)
    return QStringLiteral("source_revision_changed");
  if (trace.traceIndex != selection.traceIndex || trace.timeUnit != "ms" ||
      trace.amplitudes.size() != std::size_t(plan.readSampleCount) ||
      !sameTime(trace.firstTime, plan.firstReadTimeMs) || !sameTime(trace.sampleInterval, plan.sampleIntervalMs))
    return QStringLiteral("seismic_axis_changed");
  const auto revisionError = checkRevision(selection.volume, service);
  if (!revisionError.isEmpty())
    return revisionError;
  if (cancelled && cancelled())
    return QStringLiteral("cancelled");
  out = std::move(trace.amplitudes);
  return {};
}

} // namespace paleo::capability
