// 层：功能
#include "capabilityseismicjob.h"

#include "algorithms/seisattrgroup.h"
#include "documentsession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QThread>
#include <QUuid>
#include <bit>
#include <cmath>

namespace paleo::capability {
namespace {

constexpr int MaxTraceReadSamples = 1000000;

ExecuteResult failure(const QString &code) {
  ExecuteResult result;
  result.code = code;
  result.execution = code == QLatin1String("cancelled") ? QStringLiteral("cancelled") : QStringLiteral("failed");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  return result;
}

struct BudgetReservation {
  qint64 bytes;
  ~BudgetReservation() { ProcessBudgetLedger::instance().release(bytes); }
};

QString traceResultPath(const QString &operationDir, const QString &operationId) {
  return QDir(operationDir).absoluteFilePath(QStringLiteral("results/") + operationId + QStringLiteral("-trace.f32"));
}

bool stageTraceAttributes(const QString &path, const seisattr::TraceGroupResult &data, QVariantList &ports,
                          const std::function<bool()> &cancelled) {
  static_assert(sizeof(float) == 4 && std::endian::native == std::endian::little);
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  const auto append = [&](const QString &id, const std::vector<float> &values) {
    if (values.empty())
      return true;
    if (cancelled && cancelled())
      return false;
    const QByteArrayView bytes(reinterpret_cast<const char *>(values.data()), qsizetype(values.size() * sizeof(float)));
    const auto offset = file.pos();
    if (file.write(bytes.data(), bytes.size()) != bytes.size())
      return false;
    ports.append(QVariantMap{{QStringLiteral("portId"), id}, {QStringLiteral("byteOffset"), offset},
                             {QStringLiteral("sampleCount"), data.nOut},
                             {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
    return true;
  };
  return append(QStringLiteral("RMS"), data.rms) && append(QStringLiteral("ENVELOPE"), data.envelope) &&
         append(QStringLiteral("PHASE"), data.phase) && !(cancelled && cancelled()) && file.commit();
}

} // namespace

QString bindSeismicTrace(ExecuteRequest &request, const document::DocumentSession &document,
                        const SeismicTraceSelection &selection) {
  const auto pinned = selection;
  request.seismicTrace.reset();
  const auto &ref = pinned.volume;
  if (QThread::currentThread() != document.thread() || !document.isOpen() || !ref.valid() ||
      ref.kind != document::ObjectKind::SeismicVolume || ref.source.documentId != document.documentId())
    return QStringLiteral("invalid_seismic_input");
  const auto source = document.source(ref.source);
  if (source.ref != ref.source || source.modelDirectory.isEmpty())
    return QStringLiteral("source_revision_changed");
  request.capabilityId = QStringLiteral("seismic.attributes.compute");
  request.documentId = document.documentId();
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = ref.revision;
  request.seismicTrace = std::make_shared<const ResolvedSeismicTraceInput>(ResolvedSeismicTraceInput{source, pinned});
  return {};
}

QJsonObject seismicTraceIdentity(const ResolvedSeismicTraceInput &input) {
  const auto &selection = input.selection;
  return {{QStringLiteral("volume"), modelObjectRefJson(selection.volume)},
          {QStringLiteral("traceIndex"), QString::number(selection.traceIndex)},
          {QStringLiteral("firstSample"), selection.firstSample}, {QStringLiteral("sampleCount"), selection.sampleCount}};
}

bool restoreSeismicTraceResult(const ExecuteRequest &request, ExecuteResult &result) {
  const QString &id = result.operationId;
  const QUuid uuid(id.mid(3));
  if (uuid.isNull() || id != QStringLiteral("op-") + uuid.toString(QUuid::WithoutBraces))
    return false;
  const auto ref = QJsonObject::fromVariantMap(result.summary.value(QStringLiteral("resultRef")).toMap());
  const auto ports = ref.value(QStringLiteral("ports")).toArray();
  if (ref.value(QStringLiteral("operationId")).toString() != id ||
      ref.value(QStringLiteral("encoding")).toString() != QLatin1String("float32le") || ports.isEmpty())
    return false;
  const int count = request.seismicTrace->selection.sampleCount;
  QStringList remaining{QStringLiteral("RMS"), QStringLiteral("ENVELOPE"), QStringLiteral("PHASE")};
  qint64 bytes = 0;
  for (const auto &value : ports) {
    const auto port = value.toObject();
    if (!remaining.removeOne(port.value(QStringLiteral("portId")).toString()) ||
        port.value(QStringLiteral("byteOffset")).toInteger(-1) != bytes ||
        port.value(QStringLiteral("sampleCount")).toInteger(-1) != count)
      return false;
    bytes += qint64(count) * sizeof(float);
  }
  const QFileInfo file(traceResultPath(request.operationDir, id));
  if (!file.isFile() || file.isSymLink() || file.size() != bytes)
    return false;
  result.summary.insert(QStringLiteral("result_path"), file.absoluteFilePath());
  return true;
}

QString prepareSeismicTraceJob(const ExecuteRequest &request, ValidatedParams &params,
                              SeismicTraceExecutionPlan &plan) {
  const auto &source = *request.seismicTrace;
  if (source.selection.volume.source.documentId != request.documentId ||
      request.bindingMode != QLatin1String("pinned-value"))
    return QStringLiteral("invalid_seismic_input");
  plan.attributes = params.effective.value(QStringLiteral("ATTRIBUTES")).toStringList();
  if (plan.attributes.contains(QStringLiteral("coherence")))
    return QStringLiteral("volume_window_required");
  const bool rms = plan.attributes.contains(QStringLiteral("rms"));
  const bool analytic = plan.attributes.contains(QStringLiteral("envelope")) || plan.attributes.contains(QStringLiteral("phase"));
  plan.rmsHalf = rms ? params.effective.value(QStringLiteral("WINDOW_HALF")).toInt() : 0;
  plan.input = planSeismicTraceInput(source.source, source.selection, plan.rmsHalf, analytic,
                                     MaxTraceReadSamples, request.cancelled);
  if (!plan.input.ok)
    return plan.input.code;
  const auto interval = request.parameters.constFind(QStringLiteral("SAMPLE_INTERVAL_MS"));
  if (interval != request.parameters.cend() && std::abs(interval->toDouble() - plan.input.sampleIntervalMs) > 1e-9)
    return QStringLiteral("seismic_axis_changed");
  params.effective.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), plan.input.sampleIntervalMs);
  const auto budget = seisattr::planAttributeBudget(1, plan.input.readSampleCount, plan.rmsHalf, analytic, rms,
      false, 0, 0, 0, 0, ProcessBudgetLedger::instance().limit());
  if (budget.code == "budget_exceeded")
    return QStringLiteral("budget_exceeded");
  // Add the read buffers, numerical scratch and staged output ports to the
  // kernel's minimum estimate. Analytic attributes also hold FFT work buffers.
  plan.estimatedBytes = budget.estimatedBytes + 4096 + (analytic ? 48LL : 16LL) * plan.input.readSampleCount +
      qint64(sizeof(float)) * source.selection.sampleCount * plan.attributes.size();
  return plan.estimatedBytes <= ProcessBudgetLedger::instance().limit() ? QString() : QStringLiteral("budget_exceeded");
}

ExecuteResult executeSeismicTraceJob(const ExecuteRequest &request, const SeismicTraceExecutionPlan &plan,
                                     const QString &operationId, const std::function<bool()> &cancelled) {
  const auto &source = *request.seismicTrace;
  const auto &input = plan.input;
  ExecuteResult result;
  result.ok = true;
  result.code = QStringLiteral("prepared");
  result.execution = QStringLiteral("prepared");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  result.summary = {{QStringLiteral("selection"), seismicTraceIdentity(source).toVariantMap()},
                    {QStringLiteral("firstReadSample"), input.firstReadSample}, {QStringLiteral("readSampleCount"), input.readSampleCount},
                    {QStringLiteral("sampleIntervalMs"), input.sampleIntervalMs}, {QStringLiteral("fullTrace"), input.fullTrace},
                    {QStringLiteral("estimatedBytes"), plan.estimatedBytes}};
  if (request.publishIntent == QLatin1String("prepare"))
    return result;
  if (!ProcessBudgetLedger::instance().tryReserve(plan.estimatedBytes))
    return failure(QStringLiteral("budget_exceeded"));
  const BudgetReservation reservation{plan.estimatedBytes};
  std::vector<float> samples;
  const auto readError = readSeismicTraceInput(source.source, source.selection, input, samples, cancelled);
  if (!readError.isEmpty())
    return failure(readError);
  if (cancelled && cancelled())
    return failure(QStringLiteral("cancelled"));
  seisattr::TraceGroupRequest group;
  group.traces = samples.data();
  group.nTraces = 1;
  group.traceLength = input.readSampleCount;
  group.stride = input.readSampleCount;
  group.sampleIntervalMs = input.sampleIntervalMs;
  group.rmsHalf = plan.rmsHalf;
  group.outputBegin = input.outputBegin;
  group.outputEnd = input.outputEnd;
  group.wantRms = plan.attributes.contains(QStringLiteral("rms"));
  group.wantEnvelope = plan.attributes.contains(QStringLiteral("envelope"));
  group.wantPhase = plan.attributes.contains(QStringLiteral("phase"));
  group.fullTrace = input.fullTrace;
  const auto attributes = seisattr::computeAttributeGroup(group);
  if (!attributes.ok)
    return failure(QString::fromStdString(attributes.code));
  const QDir results(QDir(request.operationDir).filePath(QStringLiteral("results")));
  if (!QDir().mkpath(results.path()))
    return failure(QStringLiteral("stage_failed"));
  const auto path = traceResultPath(request.operationDir, operationId);
  QVariantList ports;
  if (!stageTraceAttributes(path, attributes, ports, cancelled))
    return failure(cancelled && cancelled() ? QStringLiteral("cancelled") : QStringLiteral("stage_failed"));
  result.recomputed = true;
  result.code = QStringLiteral("computed");
  result.execution = QStringLiteral("succeeded");
  result.summary.insert(QStringLiteral("samplesRead"), attributes.samplesRead);
  result.summary.insert(QStringLiteral("analyticEvaluations"), attributes.analyticEvaluations);
  result.summary.insert(QStringLiteral("result_path"), path);
  result.summary.insert(QStringLiteral("resultRef"), QVariantMap{{QStringLiteral("operationId"), operationId},
      {QStringLiteral("encoding"), QStringLiteral("float32le")}, {QStringLiteral("ports"), ports}});
  return result;
}

} // namespace paleo::capability
