// 层：功能
// workflow/capabilitygateway.cpp — 试点能力的准备与执行。发布复用 commitSet。
#include "capabilitygateway.h"
#include "capabilityseismicjob.h"

#include "../algorithms/depthbatch.h"
#include "../algorithms/seisattrgroup.h"
#include "../catalog/datacatalog.h"
#include "../qgis/factorcontour.h"
#include "algorithmpublish.h"
#include "derivedassets.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace paleo::capability {
namespace {

QString fingerprint(const QString &capabilityId, int version, const QVariantMap &effective,
                    const QJsonObject &frozen) {
  QJsonObject object;
  object.insert(QStringLiteral("capability"), capabilityId);
  object.insert(QStringLiteral("version"), version);
  object.insert(QStringLiteral("params"), QJsonObject::fromVariantMap(effective));
  object.insert(QStringLiteral("frozen"), frozen);
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString shaBytes(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool forbiddenAuthority(const QVariantMap &map, QString *field) {
  const QStringList banned = {QStringLiteral("actor"), QStringLiteral("approved"), QStringLiteral("caller_scope"),
                              QStringLiteral("sql"), QStringLiteral("shell"), QStringLiteral("command")};
  for (const QString &key : banned) {
    if (map.contains(key)) {
      if (field)
        *field = key;
      return true;
    }
  }
  return false;
}

ExecuteResult rejected(const QString &code, const QString &field = QString()) {
  ExecuteResult result;
  result.ok = false;
  result.code = code;
  result.execution = QStringLiteral("failed");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  if (!field.isEmpty()) {
    ContractIssue issue;
    issue.code = code;
    issue.field = field;
    result.issues.append(issue);
  }
  return result;
}

struct BudgetHold {
  qint64 bytes = 0;
  bool held = false;
  ~BudgetHold() {
    if (held)
      ProcessBudgetLedger::instance().release(bytes);
  }
};

QJsonObject storedObject(const OperationLog::Record &record) {
  return QJsonDocument::fromJson(record.summaryJson).object();
}

bool notePublishing(OperationLog *log, OperationLog::Record *record) {
  auto publishing = *record;
  publishing.commitState = QStringLiteral("publishing");
  publishing.code = QStringLiteral("publishing");
  OperationLog::Record current;
  if (!log->save(publishing, &current))
    return false;
  *record = current;
  return current.commitState == QLatin1String("publishing");
}

void setResponse(ExecuteResult &result, QJsonObject summary, int computeCount) {
  summary.insert(QStringLiteral("ok"), result.ok);
  summary.insert(QStringLiteral("code"), result.code);
  summary.insert(QStringLiteral("operationId"), result.operationId);
  summary.insert(QStringLiteral("execution"), result.execution);
  summary.insert(QStringLiteral("cacheHit"), result.cacheHit);
  summary.insert(QStringLiteral("commitState"), result.commitState);
  summary.insert(QStringLiteral("presentation"), result.presentation);
  summary.insert(QStringLiteral("computeCount"), computeCount);
  result.summary = summary.toVariantMap();
  result.responseJson = boundToolResponse(summary);
}

ExecuteResult fromRecord(const OperationLog::Record &record, const QString &acceptCode) {
  ExecuteResult result;
  const QJsonObject summary = storedObject(record);
  result.ok = summary.value(QStringLiteral("ok")).toBool();
  result.code = acceptCode == QLatin1String("replay") ? QStringLiteral("replay") : record.code;
  result.operationId = record.operationId;
  result.execution = record.execution;
  result.commitState = record.commitState;
  result.presentation = record.presentation;
  result.requestHash = record.requestHash;
  result.cacheHit = summary.value(QStringLiteral("cacheHit")).toBool();
  result.recomputed = false;
  setResponse(result, summary, record.computeCount);
  return result;
}

bool bindingOk(const ExecuteRequest &request, ExecuteResult *result) {
  const QString mode = request.bindingMode;
  if (mode != QLatin1String("pinned-value") && mode != QLatin1String("follow-object-head") &&
      mode != QLatin1String("follow-selection")) {
    *result = rejected(QStringLiteral("binding_mode"));
    return false;
  }
  if (mode == QLatin1String("follow-selection") && request.bindingId.isEmpty()) {
    *result = rejected(QStringLiteral("binding_not_frozen"), QStringLiteral("bindingId"));
    return false;
  }
  if (mode != QLatin1String("pinned-value") && request.frozenHead.isEmpty()) {
    *result = rejected(QStringLiteral("binding_not_frozen"), QStringLiteral("frozenHead"));
    return false;
  }
  const bool hasValues = !request.inputs.value(QStringLiteral("samples")).toList().isEmpty() ||
                         !request.inputs.value(QStringLiteral("traces")).toList().isEmpty() ||
                         !request.inputs.value(QStringLiteral("volume")).toList().isEmpty() ||
                         !request.inputs.value(QStringLiteral("points_ref")).toString().isEmpty();
  if (mode != QLatin1String("pinned-value") && !hasValues) {
    *result = rejected(QStringLiteral("revision_not_a_value_store"));
    return false;
  }
  return true;
}

QString publishFiles(DataCatalog *catalog, const QString &projectDir, AlgorithmSpec spec,
                     const QVector<AlgorithmOutputArtifact> &artifacts, const QStringList &externalParents,
                     QString *receiptCode) {
  DerivedAssetRegistrar registrar(catalog, projectDir);
  if (!registrar.isBound())
    return QStringLiteral("publish_unavailable");
  AlgorithmRunResult run;
  run.ok = true;
  QVector<AlgorithmOutputTarget> targets;
  for (const AlgorithmOutputArtifact &artifact : artifacts) {
    const DerivedStaging staging =
        registrar.stage(QStringLiteral("algorithm_output"), artifact.portId, artifact.portId + QStringLiteral(".bin"));
    if (!staging.isValid())
      return QStringLiteral("stage_failed");
    QFile file(staging.absolutePath);
    const QByteArray payload = artifact.extra.value(QStringLiteral("bytes")).toByteArray();
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.flush())
      return QStringLiteral("stage_failed");
    file.close();
    AlgorithmOutputArtifact stored = artifact;
    stored.absolutePath = staging.absolutePath;
    stored.extra.remove(QStringLiteral("bytes"));
    run.artifacts.append(stored);
    targets.append({artifact.portId, staging});
  }
  DerivedPreparedSet prepared;
  QString error;
  if (!prepareAlgorithmOutputSet(spec, run, targets, prepared, &error)) {
    if (receiptCode)
      *receiptCode = error;
    return QStringLiteral("output_set_rejected");
  }
  DerivedOutputSetReceipt receipt;
  if (!registrar.commitSet(spec, prepared, externalParents, spec.id, {}, receipt) || !receipt.ready) {
    if (receiptCode)
      *receiptCode = receipt.code.isEmpty() ? receipt.error : receipt.code;
    return receipt.code.isEmpty() ? QStringLiteral("commit_failed") : receipt.code;
  }
  if (receiptCode)
    *receiptCode = receipt.id;
  return QStringLiteral("committed");
}

ExecuteResult finish(OperationLog *log, OperationLog::Record record, ExecuteResult result, bool cacheable,
                     const QByteArray &cacheBody) {
  result.operationId = record.operationId;
  result.requestHash = record.requestHash;
  if (result.code == QLatin1String("cancelled"))
    result.execution = QStringLiteral("cancelled");
  else if (!result.ok && result.execution == QLatin1String("succeeded"))
    result.execution = QStringLiteral("failed");
  else if (result.execution.isEmpty())
    result.execution = result.ok ? QStringLiteral("succeeded") : QStringLiteral("failed");
  if (record.commitState == QLatin1String("publishing") && result.commitState == QLatin1String("not_requested"))
    result.commitState = QStringLiteral("failed");
  record.execution = result.execution;
  record.commitState = result.commitState;
  record.presentation = result.presentation;
  record.code = result.code;
  setResponse(result, QJsonObject::fromVariantMap(result.summary), record.computeCount);
  record.summaryJson = QJsonDocument(QJsonObject::fromVariantMap(result.summary)).toJson(QJsonDocument::Compact);
  OperationLog::Record current;
  if (!log->save(record, &current)) {
    if (current.execution == QLatin1String("cancelled") && result.commitState != QLatin1String("committed"))
      return fromRecord(current, QString());
    result.ok = false;
    result.code = QStringLiteral("receipt_persist_failed");
    setResponse(result, QJsonObject::fromVariantMap(result.summary), record.computeCount);
    return result;
  }
  if (cacheable && result.ok)
    log->cachePut(record.requestHash, cacheBody.isEmpty() ? record.summaryJson : cacheBody);
  return result;
}

ExecuteResult executeDepth(const ExecuteRequest &request, const ValidatedParams &params, OperationLog *log,
                           OperationLog::Record record) {
  QJsonObject modelObject = QJsonObject::fromVariantMap(request.inputs.value(QStringLiteral("model")).toMap());
  QString modelError;
  const paleo::velmodel::VelocityModel model = paleo::velmodel::VelocityModel::fromJson(modelObject, &modelError);
  paleo::depthbatch::BatchRequest batch;
  const QString timeName = params.effective.value(QStringLiteral("TIME_SEMANTIC")).toString();
  const QString depthName = params.effective.value(QStringLiteral("DEPTH_SEMANTIC")).toString();
  const QString extraName = params.effective.value(QStringLiteral("EXTRAPOLATION")).toString();
  batch.modelRevision = params.effective.value(QStringLiteral("MODEL_REVISION")).toString().toStdString();
  batch.owtPolicy = params.effective.value(QStringLiteral("OWT_POLICY")).toString().toStdString();
  batch.mergeDiscontinuous = request.inputs.value(QStringLiteral("merge_discontinuous")).toBool();
  if (timeName == QLatin1String("TWT"))
    batch.time = paleo::depthbatch::TimeSemantic::TwtMs;
  else if (timeName == QLatin1String("OWT"))
    batch.time = paleo::depthbatch::TimeSemantic::OwtMs;
  if (depthName == QLatin1String("TVD"))
    batch.depth = paleo::depthbatch::DepthSemantic::TvdM;
  else if (depthName == QLatin1String("MD"))
    batch.depth = paleo::depthbatch::DepthSemantic::MdM;
  else if (depthName == QLatin1String("TVDSS"))
    batch.depth = paleo::depthbatch::DepthSemantic::TvdssM;
  if (extraName == QLatin1String("reject_outside"))
    batch.extrapolation = paleo::depthbatch::Extrapolation::RejectOutside;
  else if (extraName == QLatin1String("interval_extended_v1"))
    batch.extrapolation = paleo::depthbatch::Extrapolation::IntervalExtendedV1;
  const QVariantList intervals = request.inputs.value(QStringLiteral("intervals")).toList();
  for (const QVariant &value : intervals) {
    const QVariantMap row = value.toMap();
    paleo::depthbatch::IntervalRef interval;
    interval.intervalId = row.value(QStringLiteral("intervalId")).toString().toStdString();
    interval.occurrenceId = row.value(QStringLiteral("occurrenceId")).toString().toStdString();
    interval.kind = row.value(QStringLiteral("kind"), QStringLiteral("interval")).toString().toStdString();
    batch.intervals.push_back(interval);
  }
  const QVariantList samples = request.inputs.value(QStringLiteral("samples")).toList();
  if ((request.bindingMode == QLatin1String("follow-object-head") ||
       request.bindingMode == QLatin1String("follow-selection")) &&
      samples.isEmpty())
    return finish(log, record, rejected(QStringLiteral("revision_not_a_value_store")), false, {});
  for (const QVariant &value : samples) {
    const QVariantMap row = value.toMap();
    paleo::depthbatch::SampleRef sample;
    sample.sampleId = row.value(QStringLiteral("sampleId")).toString().toStdString();
    sample.wellId = row.value(QStringLiteral("wellId")).toString().toStdString();
    sample.occurrenceId = row.value(QStringLiteral("occurrenceId")).toString().toStdString();
    const QVariant timeValue = row.value(QStringLiteral("time"));
    const bool timeIsNumber = timeValue.userType() == QMetaType::Int || timeValue.userType() == QMetaType::Double ||
                              timeValue.userType() == QMetaType::LongLong;
    sample.time = row.contains(QStringLiteral("time")) && timeIsNumber && !timeValue.isNull()
                      ? timeValue.toDouble()
                      : std::numeric_limits<double>::quiet_NaN();
    if (!row.contains(QStringLiteral("quality")) || row.value(QStringLiteral("quality")).isNull()) {
      sample.hasQuality = row.contains(QStringLiteral("quality")) ? false : true;
      sample.quality = sample.hasQuality ? 1.0 : 0.0;
    } else {
      sample.hasQuality = true;
      sample.quality = row.value(QStringLiteral("quality")).toDouble();
    }
    batch.samples.push_back(sample);
  }
  const qint64 bytes = std::max<qint64>(1, static_cast<qint64>(batch.samples.size()) * 64 + 4096);
  BudgetHold hold;
  hold.bytes = bytes;
  hold.held = ProcessBudgetLedger::instance().tryReserve(bytes);
  if (!hold.held)
    return finish(log, record, rejected(QStringLiteral("budget_exceeded")), false, {});
  if (request.cancelled && request.cancelled())
    return finish(log, record, rejected(QStringLiteral("cancelled")), false, {});
  const paleo::depthbatch::BatchResult converted =
      paleo::depthbatch::convertWellBatch(model, batch, request.cancelled);
  ExecuteResult result;
  result.recomputed = true;
  result.code = QString::fromStdString(converted.code);
  result.ok = converted.ok;
  result.execution = converted.ok ? QStringLiteral("succeeded") : QStringLiteral("failed");
  if (converted.code == "cancelled")
    result.execution = QStringLiteral("cancelled");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  QVariantList rows;
  for (const paleo::depthbatch::SampleOut &sample : converted.samples) {
    QVariantMap row;
    row.insert(QStringLiteral("sampleId"), QString::fromStdString(sample.sampleId));
    row.insert(QStringLiteral("wellId"), QString::fromStdString(sample.wellId));
    row.insert(QStringLiteral("occurrenceId"), QString::fromStdString(sample.occurrenceId));
    row.insert(QStringLiteral("code"), QString::fromStdString(sample.code));
    if (sample.valid)
      row.insert(QStringLiteral("depthM"), sample.depthM);
    rows.append(row);
  }
  result.summary.insert(QStringLiteral("converted"), converted.converted);
  result.summary.insert(QStringLiteral("outside"), converted.outside);
  result.summary.insert(QStringLiteral("invalid"), converted.invalid);
  result.summary.insert(QStringLiteral("samples"), rows);
  result.summary.insert(QStringLiteral("extrapolation"), QString::fromStdString(converted.extrapolationUsed));
  QStringList occurrenceIds;
  for (const std::string &id : converted.occurrenceIds)
    occurrenceIds.append(QString::fromStdString(id));
  result.summary.insert(QStringLiteral("occurrenceIds"), occurrenceIds);
  if (!modelError.isEmpty())
    result.summary.insert(QStringLiteral("modelError"), modelError);
  if (!converted.ok)
    return finish(log, record, result, false, {});
  if (request.stageBytes) {
    const auto bytes = QJsonDocument(QJsonArray::fromVariantList(rows)).toJson(QJsonDocument::Compact);
    const auto code = request.stageBytes(QStringLiteral("DEPTH"), QByteArrayView(bytes));
    if (!code.isEmpty()) return finish(log, record, rejected(code), false, {});
  }
  const bool publish = request.publishIntent == QLatin1String("publish");
  if (!publish)
    return finish(log, record, result, true, {});
  if (request.cancelled && request.cancelled()) {
    result.ok = false;
    result.code = QStringLiteral("cancelled");
    result.execution = QStringLiteral("cancelled");
    result.commitState = QStringLiteral("not_requested");
    return finish(log, record, result, false, {});
  }
  if (request.currentHead != request.frozenHead) {
    result.ok = false;
    result.code = QStringLiteral("stale_revision");
    result.commitState = QStringLiteral("failed");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, true, {});
  }
  if (!request.catalog) {
    result.ok = false;
    result.code = QStringLiteral("publish_unavailable");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, true, {});
  }
  if (!notePublishing(log, &record)) {
    result.ok = false;
    result.code = QStringLiteral("persist_failed");
    result.commitState = QStringLiteral("not_requested");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, false, {});
  }
  AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion(QStringLiteral("native:depth_batch_convert"), 1);
  AlgorithmOutputArtifact artifact;
  artifact.portId = QStringLiteral("DEPTH");
  artifact.extra.insert(QStringLiteral("bytes"), QJsonDocument(QJsonArray::fromVariantList(rows)).toJson(QJsonDocument::Compact));
  QString receipt;
  const QString committed = publishFiles(request.catalog, request.projectDir, spec, {artifact}, {}, &receipt);
  result.commitState = committed == QLatin1String("committed") ? QStringLiteral("committed") : QStringLiteral("failed");
  result.ok = result.commitState == QLatin1String("committed");
  result.code = result.ok ? QStringLiteral("committed") : committed;
  result.summary.insert(QStringLiteral("receipt"), receipt);
  if (result.ok && !request.presentOk) {
    result.presentation = QStringLiteral("pending");
    result.code = QStringLiteral("presentation_pending");
  } else if (result.ok) {
    result.presentation = QStringLiteral("done");
  }
  result.execution = QStringLiteral("succeeded");
  return finish(log, record, result, true, {});
}

ExecuteResult executeSeismic(const ExecuteRequest &request, const ValidatedParams &params, OperationLog *log,
                             OperationLog::Record record) {
  const QStringList attributes = params.effective.value(QStringLiteral("ATTRIBUTES")).toStringList();
  const bool wantRms = attributes.contains(QStringLiteral("rms"));
  const bool wantEnvelope = attributes.contains(QStringLiteral("envelope")) || attributes.contains(QStringLiteral("phase"));
  const bool wantPhase = attributes.contains(QStringLiteral("phase"));
  const bool wantCoherence = attributes.contains(QStringLiteral("coherence"));
  const QString weight = params.effective.value(QStringLiteral("COHERENCE_WEIGHT")).toString();
  if (wantCoherence && weight != QLatin1String("equal") && weight != QLatin1String("inverse"))
    return finish(log, record, rejected(QStringLiteral("out_of_range"), QStringLiteral("COHERENCE_WEIGHT")), false, {});
  if (wantCoherence && weight == QLatin1String("inverse") &&
      !(params.effective.value(QStringLiteral("IL_SPACING")).toDouble() > 0.0 &&
        params.effective.value(QStringLiteral("XL_SPACING")).toDouble() > 0.0))
    return finish(log, record, rejected(QStringLiteral("missing_required"), QStringLiteral("IL_SPACING")), false, {});
  const QVariantList traces = request.inputs.value(QStringLiteral("traces")).toList();
  if ((wantRms || wantEnvelope) && traces.isEmpty())
    return finish(log, record, rejected(QStringLiteral("missing_required"), QStringLiteral("traces")), false, {});
  int length = -1;
  for (const QVariant &trace : traces) {
    const int n = static_cast<int>(trace.toList().size());
    if (length < 0)
      length = n;
    else if (n != length)
      return finish(log, record, rejected(QStringLiteral("ragged_trace")), false, {});
  }
  if (length < 0)
    length = 0;
  const int nIl = request.inputs.value(QStringLiteral("nIl")).toInt();
  const int nXl = request.inputs.value(QStringLiteral("nXl")).toInt();
  const int volumeSamples = request.inputs.value(QStringLiteral("nS")).toInt();
  int coherenceCount = 0;
  if (wantCoherence) {
    const QVariantList volume = request.inputs.value(QStringLiteral("volume")).toList();
    if (!paleo::seisattr::volumeIndexCount(nIl, nXl, volumeSamples, &coherenceCount) ||
        volume.size() != static_cast<qsizetype>(coherenceCount))
      return finish(log, record, rejected(QStringLiteral("invalid_shape"), QStringLiteral("volume")), false, {});
  }
  const int planSamples = length > 0 ? length : volumeSamples;
  const auto plan = paleo::seisattr::planAttributeBudget(
      static_cast<int>(traces.size()), planSamples, params.effective.value(QStringLiteral("WINDOW_HALF")).toInt(),
      wantEnvelope, wantRms && !wantEnvelope, wantCoherence, nIl, nXl,
      params.effective.value(QStringLiteral("COHERENCE_IL_HALF")).toInt(),
      params.effective.value(QStringLiteral("COHERENCE_XL_HALF")).toInt(), ProcessBudgetLedger::instance().limit(),
      volumeSamples);
  if (plan.code == "budget_exceeded")
    return finish(log, record, rejected(QStringLiteral("budget_exceeded")), false, {});
  BudgetHold hold;
  hold.bytes = std::max<qint64>(plan.estimatedBytes, 1);
  hold.held = ProcessBudgetLedger::instance().tryReserve(hold.bytes);
  if (!hold.held)
    return finish(log, record, rejected(QStringLiteral("budget_exceeded")), false, {});
  const QByteArray cached = request.publishIntent == QLatin1String("publish") || request.stageBytes
                                ? QByteArray()
                                : log->cacheGet(record.requestHash);
  if (!cached.isEmpty()) {
    ExecuteResult result;
    result.ok = true;
    result.code = QStringLiteral("cache_hit");
    result.cacheHit = true;
    result.recomputed = false;
    result.execution = QStringLiteral("succeeded");
    result.commitState = QStringLiteral("not_requested");
    result.presentation = QStringLiteral("not_requested");
    result.summary = QJsonDocument::fromJson(cached).object().toVariantMap();
    result.summary.insert(QStringLiteral("cacheHit"), true);
    result.summary.insert(QStringLiteral("analyticEvaluationsAdded"), 0);
    return finish(log, record, result, false, {});
  }
  paleo::seisattr::TraceGroupResult computed;
  computed.ok = true;
  computed.code = "ok";
  ExecuteResult result;
  const int traceCount = static_cast<int>(traces.size());
  const int batch = plan.traceBatch > 0 ? plan.traceBatch : std::max(traceCount, 1);
  if ((wantRms || wantEnvelope || wantPhase) && traceCount > 0 && length > 0) {
    for (int start = 0; start < traceCount; start += batch) {
      if (cancelRequested(record.operationId) || (request.cancelled && request.cancelled())) {
        result.ok = false;
        result.code = QStringLiteral("cancelled");
        result.execution = QStringLiteral("cancelled");
        result.commitState = QStringLiteral("not_requested");
        return finish(log, record, result, false, {});
      }
      const int count = std::min(batch, traceCount - start);
      std::vector<float> buffer(static_cast<std::size_t>(count * length), std::numeric_limits<float>::quiet_NaN());
      for (int i = 0; i < count; ++i) {
        const QVariantList row = traces.at(start + i).toList();
        for (int s = 0; s < row.size() && s < length; ++s) {
          if (row.at(s).isNull())
            continue;
          bool ok = false;
          const double number = row.at(s).toDouble(&ok);
          buffer[static_cast<std::size_t>(i * length + s)] =
              ok ? static_cast<float>(number) : std::numeric_limits<float>::quiet_NaN();
        }
      }
      paleo::seisattr::TraceGroupRequest group;
      group.traces = buffer.data();
      group.nTraces = count;
      group.traceLength = length;
      group.stride = length;
      group.sampleIntervalMs = params.effective.value(QStringLiteral("SAMPLE_INTERVAL_MS")).toDouble();
      group.rmsHalf = params.effective.value(QStringLiteral("WINDOW_HALF")).toInt();
      group.wantRms = wantRms;
      group.wantEnvelope = attributes.contains(QStringLiteral("envelope"));
      group.wantPhase = wantPhase;
      group.fullTrace = request.inputs.value(QStringLiteral("full_trace"), true).toBool();
      const auto part = paleo::seisattr::computeAttributeGroup(group);
      if (!part.ok) {
        computed = part;
        break;
      }
      computed.nOut = part.nOut;
      computed.analyticEvaluations += part.analyticEvaluations;
      computed.samplesRead += part.samplesRead;
      computed.rmsHalfApplied = part.rmsHalfApplied;
      computed.rms.insert(computed.rms.end(), part.rms.begin(), part.rms.end());
      computed.envelope.insert(computed.envelope.end(), part.envelope.begin(), part.envelope.end());
      computed.phase.insert(computed.phase.end(), part.phase.begin(), part.phase.end());
    }
  }
  const bool groupWantEnvelope = attributes.contains(QStringLiteral("envelope"));
  result.recomputed = true;
  if ((wantRms || groupWantEnvelope || wantPhase) && !computed.ok) {
    result.ok = false;
    result.code = QString::fromStdString(computed.code);
    result.execution = QStringLiteral("failed");
    result.commitState = QStringLiteral("not_requested");
    result.presentation = QStringLiteral("not_requested");
    return finish(log, record, result, false, {});
  }
  auto hashFloats = [](const std::vector<float> &values) {
    if (values.empty())
      return QString();
    const QByteArray bytes(reinterpret_cast<const char *>(values.data()),
                           static_cast<int>(values.size() * sizeof(float)));
    return shaBytes(bytes);
  };
  result.ok = true;
  result.code = QStringLiteral("computed");
  result.execution = QStringLiteral("succeeded");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  result.summary.insert(QStringLiteral("analyticEvaluations"), computed.analyticEvaluations);
  result.summary.insert(QStringLiteral("samplesRead"), computed.samplesRead);
  result.summary.insert(QStringLiteral("rmsHalf"), computed.rmsHalfApplied);
  result.summary.insert(QStringLiteral("plan"), QString::fromStdString(plan.code));
  result.summary.insert(QStringLiteral("estimatedBytes"), static_cast<qlonglong>(plan.estimatedBytes));
  result.summary.insert(QStringLiteral("minimumBytes"), static_cast<qlonglong>(plan.minimumBytes));
  if (wantRms)
    result.summary.insert(QStringLiteral("rmsSha256"), hashFloats(computed.rms));
  if (groupWantEnvelope)
    result.summary.insert(QStringLiteral("envelopeSha256"), hashFloats(computed.envelope));
  if (wantPhase)
    result.summary.insert(QStringLiteral("phaseSha256"), hashFloats(computed.phase));
  std::vector<float> coherenceValues;
  QString coherencePath;
  if (wantCoherence) {
    const QVariantList values = request.inputs.value(QStringLiteral("volume")).toList();
    if (values.size() != static_cast<qsizetype>(coherenceCount))
      return finish(log, record, rejected(QStringLiteral("invalid_shape"), QStringLiteral("volume")), false, {});
    const auto stopped = [&] {
      return cancelRequested(record.operationId) || (request.cancelled && request.cancelled());
    };
    const auto sampleAt = [&](int index) {
      const QVariant value = values.at(index);
      if (value.isNull())
        return std::numeric_limits<float>::quiet_NaN();
      bool ok = false;
      const double number = value.toDouble(&ok);
      return ok ? static_cast<float>(number) : std::numeric_limits<float>::quiet_NaN();
    };
    const int ilHalf = params.effective.value(QStringLiteral("COHERENCE_IL_HALF")).toInt();
    const int xlHalf = params.effective.value(QStringLiteral("COHERENCE_XL_HALF")).toInt();
    const auto mode = weight == QLatin1String("inverse") ? paleo::seisattr::CoherenceWeightMode::InverseDistance
                                                         : paleo::seisattr::CoherenceWeightMode::Equal;
    const int interiorXl = std::max(0, nXl - 2 * std::max(xlHalf, 0));
    // split 也可能只是包络按道分批。只有 xlChunk 小于整幅内区时才沿 XL 切相干。
    if (plan.xlChunk > 0 && interiorXl > plan.xlChunk) {
      const auto spans = paleo::seisattr::coherenceXlSpans(nXl, xlHalf, plan.xlChunk);
      if (spans.empty())
        return finish(log, record, rejected(QStringLiteral("budget_exceeded")), false, {});
      QDir().mkpath(request.operationDir + QStringLiteral("/results"));
      coherencePath = request.operationDir + QStringLiteral("/results/") + record.operationId +
                      QStringLiteral("-coherence.f32");
      QFile out(coherencePath);
      if (!out.open(QIODevice::ReadWrite | QIODevice::Truncate))
        return finish(log, record, rejected(QStringLiteral("result_write_failed")), false, {});
      const int interiorIl = std::max(0, nIl - 2 * std::max(ilHalf, 0));
      const qint64 sampleCount64 = static_cast<qint64>(interiorIl) * interiorXl * volumeSamples;
      QCryptographicHash hasher(QCryptographicHash::Sha256);
      int samplesRead = 0;
      if (sampleCount64 <= 0 ||
          sampleCount64 > std::numeric_limits<qint64>::max() / static_cast<qint64>(sizeof(float))) {
        out.remove();
        return finish(log, record, rejected(QStringLiteral("result_write_failed")), false, {});
      }
      const qint64 byteCount = sampleCount64 * static_cast<qint64>(sizeof(float));
      if (!out.resize(byteCount)) {
        out.remove();
        return finish(log, record, rejected(QStringLiteral("result_write_failed")), false, {});
      }
      for (const paleo::seisattr::CoherenceXlSpan &span : spans) {
        if (stopped()) {
          out.remove();
          result.ok = false;
          result.code = QStringLiteral("cancelled");
          result.execution = QStringLiteral("cancelled");
          return finish(log, record, result, false, {});
        }
        const int tileNXl = span.sourceXl1 - span.sourceXl0;
        std::vector<float> slab(static_cast<std::size_t>(nIl * tileNXl * volumeSamples));
        for (int il = 0; il < nIl; ++il) {
          for (int xl = span.sourceXl0; xl < span.sourceXl1; ++xl) {
            for (int s = 0; s < volumeSamples; ++s) {
              const int src = ((il * nXl) + xl) * volumeSamples + s;
              const int dst = ((il * tileNXl) + (xl - span.sourceXl0)) * volumeSamples + s;
              slab[static_cast<std::size_t>(dst)] = sampleAt(src);
            }
          }
        }
        paleo::seisattr::CoherenceTile tile;
        tile.volume = slab.data();
        tile.nIl = nIl;
        tile.nXl = tileNXl;
        tile.nS = volumeSamples;
        tile.ilHalf = ilHalf;
        tile.xlHalf = xlHalf;
        tile.timeHalf = params.effective.value(QStringLiteral("COHERENCE_TIME_HALF")).toInt();
        tile.ilSpacing = params.effective.value(QStringLiteral("IL_SPACING")).toDouble();
        tile.xlSpacing = params.effective.value(QStringLiteral("XL_SPACING")).toDouble();
        const auto part = paleo::seisattr::coherenceInterior(tile, mode);
        if (!part.ok) {
          out.remove();
          result.ok = false;
          result.code = QString::fromStdString(part.code);
          result.execution = QStringLiteral("failed");
          return finish(log, record, result, false, {});
        }
        const int rowSamples = part.interiorXl * volumeSamples;
        const std::size_t need = static_cast<std::size_t>(part.interiorIl) * static_cast<std::size_t>(std::max(rowSamples, 0));
        if (part.interiorIl != interiorIl || rowSamples <= 0 || part.interior.size() < need) {
          out.remove();
          return finish(log, record, rejected(QStringLiteral("invalid_shape"), QStringLiteral("volume")), false, {});
        }
        for (int il = 0; il < part.interiorIl; ++il) {
          const qint64 offset = (static_cast<qint64>(il) * interiorXl + span.interiorXl0) * volumeSamples *
                                static_cast<qint64>(sizeof(float));
          const char *src = reinterpret_cast<const char *>(
              part.interior.data() + static_cast<std::size_t>(il) * static_cast<std::size_t>(rowSamples));
          const qint64 nbytes = static_cast<qint64>(rowSamples) * static_cast<qint64>(sizeof(float));
          if (!out.seek(offset) || out.write(src, nbytes) != nbytes) {
            out.remove();
            return finish(log, record, rejected(QStringLiteral("short_write")), false, {});
          }
        }
        samplesRead += part.samplesRead;
      }
      if (!out.flush() || !out.seek(0)) {
        out.remove();
        return finish(log, record, rejected(QStringLiteral("short_write")), false, {});
      }
      const QByteArray written = out.readAll();
      if (written.size() != byteCount) {
        out.remove();
        return finish(log, record, rejected(QStringLiteral("short_write")), false, {});
      }
      noteIssuedResult(request.documentId, coherencePath);
      hasher.addData(written);
      result.summary.insert(QStringLiteral("result_path"), coherencePath);
      result.summary.insert(QStringLiteral("coherenceSha256"), QString::fromLatin1(hasher.result().toHex()));
      result.summary.insert(QStringLiteral("coherenceSamplesRead"), samplesRead);
      result.summary.insert(QStringLiteral("coherenceSamples"), static_cast<int>(sampleCount64));
      result.summary.insert(QStringLiteral("tiled"), true);
    } else {
      std::vector<float> volume(static_cast<std::size_t>(coherenceCount));
      for (int i = 0; i < coherenceCount; ++i)
        volume[static_cast<std::size_t>(i)] = sampleAt(i);
      paleo::seisattr::CoherenceTile tile;
      tile.volume = volume.data();
      tile.nIl = nIl;
      tile.nXl = nXl;
      tile.nS = volumeSamples;
      tile.ilHalf = ilHalf;
      tile.xlHalf = xlHalf;
      tile.timeHalf = params.effective.value(QStringLiteral("COHERENCE_TIME_HALF")).toInt();
      tile.ilSpacing = params.effective.value(QStringLiteral("IL_SPACING")).toDouble();
      tile.xlSpacing = params.effective.value(QStringLiteral("XL_SPACING")).toDouble();
      const int chunk = plan.xlChunk > 0 ? plan.xlChunk : tile.nXl;
      const auto interior = paleo::seisattr::computeCoherenceTiled(tile, mode, chunk, stopped);
      if (!interior.ok) {
        result.ok = false;
        result.code = QString::fromStdString(interior.code);
        result.execution = interior.code == "cancelled" ? QStringLiteral("cancelled") : QStringLiteral("failed");
        return finish(log, record, result, false, {});
      }
      coherenceValues = interior.interior;
      result.summary.insert(QStringLiteral("coherenceSha256"), hashFloats(interior.interior));
      result.summary.insert(QStringLiteral("coherenceSamplesRead"), interior.samplesRead);
    }
  }
  if (request.stageBytes) {
    const auto emitPort = [&](const QString &port, const std::vector<float> &values) {
      if (values.empty()) return QString();
      return request.stageBytes(port, QByteArrayView(reinterpret_cast<const char *>(values.data()),
                                                     qsizetype(values.size() * sizeof(float))));
    };
    QString code;
    if (wantRms) code = emitPort(QStringLiteral("RMS"), computed.rms);
    if (code.isEmpty() && groupWantEnvelope) code = emitPort(QStringLiteral("ENVELOPE"), computed.envelope);
    if (code.isEmpty() && wantPhase) code = emitPort(QStringLiteral("PHASE"), computed.phase);
    if (code.isEmpty() && wantCoherence && !coherenceValues.empty())
      code = emitPort(QStringLiteral("COHERENCE"), coherenceValues);
    if (!code.isEmpty()) return finish(log, record, rejected(code), false, {});
  }
  if (request.publishIntent != QLatin1String("publish"))
    return finish(log, record, result, true, QJsonDocument(QJsonObject::fromVariantMap(result.summary)).toJson(QJsonDocument::Compact));
  if (request.cancelled && request.cancelled()) {
    result.ok = false;
    result.code = QStringLiteral("cancelled");
    result.execution = QStringLiteral("cancelled");
    result.commitState = QStringLiteral("not_requested");
    return finish(log, record, result, false, {});
  }
  if (request.currentHead != request.frozenHead) {
    result.ok = false;
    result.code = QStringLiteral("stale_revision");
    result.commitState = QStringLiteral("failed");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, true, {});
  }
  if (!request.catalog)
    return finish(log, record, rejected(QStringLiteral("publish_unavailable")), true, {});
  if (!notePublishing(log, &record)) {
    result.ok = false;
    result.code = QStringLiteral("persist_failed");
    result.commitState = QStringLiteral("not_requested");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, false, {});
  }
  AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion(QStringLiteral("native:seismic_attribute_group"), 1);
  const QVariantMap parents = request.inputs.value(QStringLiteral("parents")).toMap();
  QVector<AlgorithmOutputArtifact> artifacts;
  auto floatBytes = [](const std::vector<float> &values) {
    return QByteArray(reinterpret_cast<const char *>(values.data()),
                      static_cast<int>(values.size() * sizeof(float)));
  };
  auto add = [&](const QString &port, const QByteArray &bytes) {
    if (bytes.isEmpty())
      return;
    for (AlgoPort &output : spec.outputs) {
      if (output.id == port)
        output.required = true;
    }
    AlgorithmOutputArtifact artifact;
    artifact.portId = port;
    artifact.extra.insert(QStringLiteral("bytes"), bytes);
    const QStringList parentPorts = parents.value(port).toStringList();
    artifact.parentPortIds = parentPorts;
    if (request.inputs.value(QStringLiteral("drop_output_port")).toString() == port)
      return;
    artifacts.append(artifact);
  };
  if (wantRms)
    add(QStringLiteral("RMS"), floatBytes(computed.rms));
  if (groupWantEnvelope)
    add(QStringLiteral("ENVELOPE"), floatBytes(computed.envelope));
  if (wantPhase)
    add(QStringLiteral("PHASE"), floatBytes(computed.phase));
  if (wantCoherence && coherenceValues.empty() && !coherencePath.isEmpty()) {
    QFile tiled(coherencePath);
    if (tiled.open(QIODevice::ReadOnly)) {
      const QByteArray bytes = tiled.readAll();
      if (bytes.size() == tiled.size() && !bytes.isEmpty())
        add(QStringLiteral("COHERENCE"), bytes);
    }
  } else if (wantCoherence) {
    add(QStringLiteral("COHERENCE"), floatBytes(coherenceValues));
  }
  QString receipt;
  const QString committed = publishFiles(request.catalog, request.projectDir, spec, artifacts, {}, &receipt);
  result.commitState = committed == QLatin1String("committed") ? QStringLiteral("committed") : QStringLiteral("failed");
  result.ok = result.commitState == QLatin1String("committed");
  result.code = result.ok ? QStringLiteral("committed") : committed;
  result.summary.insert(QStringLiteral("receipt"), receipt);
  if (result.ok && !request.presentOk) {
    result.presentation = QStringLiteral("pending");
    result.code = QStringLiteral("presentation_pending");
  } else if (result.ok) {
    result.presentation = QStringLiteral("done");
  }
  return finish(log, record, result, true, {});
}

ExecuteResult executeContour(const ExecuteRequest &request, const ValidatedParams &params, OperationLog *log,
                             OperationLog::Record record) {
  const QString raster = request.inputs.value(QStringLiteral("raster_path")).toString();
  if (!isIssuedResult(request.documentId, raster))
    return finish(log, record, rejected(QStringLiteral("untrusted_result_ref"), QStringLiteral("raster_path")), false, {});
  const double level = params.effective.value(QStringLiteral("CONTOUR_LEVEL")).toDouble();
  if (!std::isfinite(level))
    return finish(log, record, rejected(QStringLiteral("missing_required"), QStringLiteral("CONTOUR_LEVEL")), false, {});
  QDir().mkpath(request.operationDir + QStringLiteral("/results"));
  const QString gpkg = request.operationDir + QStringLiteral("/results/") + record.operationId + QStringLiteral("-contours.gpkg");
  QString error;
  const bool drew = FactorContourService::generateFixedContours(
      raster, gpkg, QVector<double>{level}, &error,
      [&] { return cancelRequested(record.operationId) || (request.cancelled && request.cancelled()); });
  if (!drew) {
    ExecuteResult failed;
    failed.ok = false;
    failed.code = error.isEmpty() ? QStringLiteral("contour_failed") : error;
    failed.execution = QStringLiteral("failed");
    failed.commitState = QStringLiteral("not_requested");
    failed.presentation = QStringLiteral("not_requested");
    return finish(log, record, failed, false, {});
  }
  QFile file(gpkg);
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0) {
    ExecuteResult failed;
    failed.ok = false;
    failed.code = QStringLiteral("contour_unreadable");
    failed.execution = QStringLiteral("failed");
    return finish(log, record, failed, false, {});
  }
  noteIssuedResult(request.documentId, gpkg);
  ExecuteResult result;
  result.ok = true;
  result.recomputed = true;
  result.code = QStringLiteral("computed");
  result.execution = QStringLiteral("succeeded");
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  result.summary.insert(QStringLiteral("result_path"), gpkg);
  result.summary.insert(QStringLiteral("bytes"), static_cast<qlonglong>(file.size()));
  return finish(log, record, result, true, {});
}

ExecuteResult executeConstraint(const ExecuteRequest &request, const ValidatedParams &params, OperationLog *log,
                                OperationLog::Record record) {
  const QString pointsRef = request.inputs.value(QStringLiteral("points_ref")).toString();
  const QString subinterval = request.inputs.value(QStringLiteral("subinterval_id")).toString();
  const QVariantList pointRows = request.inputs.value(QStringLiteral("points")).toList();
  if (pointsRef.isEmpty() && pointRows.isEmpty())
    return finish(log, record, rejected(QStringLiteral("missing_required"), QStringLiteral("points_ref")), false, {});
  if (subinterval.isEmpty())
    return finish(log, record, rejected(QStringLiteral("missing_required"), QStringLiteral("subinterval_id")), false, {});
  if (request.inputs.value(QStringLiteral("ignore_barriers")).toBool())
    return finish(log, record, rejected(QStringLiteral("barrier_downgrade_refused")), false, {});
  const QString intent = request.publishIntent.isEmpty() ? QStringLiteral("prepare") : request.publishIntent;
  ExecuteResult result;
  result.summary.insert(QStringLiteral("effective"), params.effective);
  result.summary.insert(QStringLiteral("points_ref"), pointsRef);
  result.summary.insert(QStringLiteral("subinterval_id"), subinterval);
  result.summary.insert(QStringLiteral("constraints_ref"), request.inputs.value(QStringLiteral("constraints_ref")).toString());
  result.summary.insert(QStringLiteral("kernel"), QStringLiteral("ConstraintIDWAlgorithm::processAlgorithm"));
  result.commitState = QStringLiteral("not_requested");
  result.presentation = QStringLiteral("not_requested");
  if (intent == QLatin1String("prepare")) {
    result.ok = true;
    result.recomputed = false;
    result.code = QStringLiteral("prepared");
    result.execution = QStringLiteral("prepared");
    return finish(log, record, result, false, {});
  }
  if (intent != QLatin1String("compute-only") && intent != QLatin1String("publish"))
    return finish(log, record, rejected(QStringLiteral("unsupported_intent")), false, {});
  if (pointRows.isEmpty())
    return finish(log, record, rejected(QStringLiteral("revision_not_a_value_store"), QStringLiteral("points")), false, {});
  QDir().mkpath(request.operationDir + QStringLiteral("/results"));
  FrozenIdwJob job;
  job.points = pointRows;
  job.lines = request.inputs.value(QStringLiteral("lines")).toList();
  job.field = params.effective.value(QStringLiteral("FIELD")).toString();
  job.cellSize = params.effective.value(QStringLiteral("CELL_SIZE")).toDouble();
  job.aniso = params.effective.value(QStringLiteral("ANISO_RATIO")).toDouble();
  job.outputPath = request.operationDir + QStringLiteral("/results/") + record.operationId + QStringLiteral("-idw.tif");
  job.gridWkt = request.inputs.value(QStringLiteral("grid_wkt")).toString();
  if (job.gridWkt.isEmpty())
    job.gridWkt = DataCatalog::localGridCrsWkt();
  job.cancelled = [&] { return cancelRequested(record.operationId) || (request.cancelled && request.cancelled()); };
  const FrozenIdwResult idw = runFrozenConstraintIdw(job);
  if (!idw.ok) {
    result.ok = false;
    result.recomputed = false;
    result.code = idw.code;
    const bool stopped = idw.code.contains(QStringLiteral("Canceled")) || idw.code.contains(QStringLiteral("取消"));
    result.execution = stopped ? QStringLiteral("cancelled") : QStringLiteral("failed");
    if (stopped)
      result.code = QStringLiteral("cancelled");
    return finish(log, record, result, false, {});
  }
  noteIssuedResult(request.documentId, idw.rasterPath);
  result.ok = true;
  result.recomputed = true;
  result.code = QStringLiteral("computed");
  result.execution = QStringLiteral("succeeded");
  result.summary.insert(QStringLiteral("result_path"), idw.rasterPath);
  result.summary.insert(QStringLiteral("width"), idw.width);
  result.summary.insert(QStringLiteral("height"), idw.height);
  result.summary.insert(QStringLiteral("constraint_receipts"), idw.receipts);
  if (intent != QLatin1String("publish"))
    return finish(log, record, result, true, {});
  if (request.currentHead != request.frozenHead) {
    result.ok = false;
    result.code = QStringLiteral("stale_revision");
    result.commitState = QStringLiteral("failed");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, false, {});
  }
  if (cancelRequested(record.operationId) || (request.cancelled && request.cancelled())) {
    result.ok = false;
    result.code = QStringLiteral("cancelled");
    result.execution = QStringLiteral("cancelled");
    return finish(log, record, result, false, {});
  }
  if (!notePublishing(log, &record)) {
    result.ok = false;
    result.code = QStringLiteral("persist_failed");
    result.commitState = QStringLiteral("not_requested");
    result.execution = QStringLiteral("failed");
    return finish(log, record, result, false, {});
  }
  QFile raster(idw.rasterPath);
  if (!raster.open(QIODevice::ReadOnly))
    return finish(log, record, rejected(QStringLiteral("raster_unreadable")), false, {});
  const QByteArray bytes = raster.readAll();
  if (bytes.size() != raster.size() || bytes.isEmpty())
    return finish(log, record, rejected(QStringLiteral("short_write")), false, {});
  if (!request.catalog)
    return finish(log, record, rejected(QStringLiteral("publish_unavailable")), true, {});
  AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion(QStringLiteral("paleo:paleo_constraint_idw"), 1);
  AlgorithmOutputArtifact artifact;
  artifact.portId = QStringLiteral("OUTPUT");
  artifact.extra.insert(QStringLiteral("bytes"), bytes);
  artifact.extra.insert(QStringLiteral("operationId"), record.operationId);
  QString receipt;
  const QStringList parents = request.inputs.value(QStringLiteral("parent_version_ids")).toStringList();
  const QString committed = publishFiles(request.catalog, request.projectDir, spec, {artifact}, parents, &receipt);
  result.commitState = committed == QLatin1String("committed") ? QStringLiteral("committed") : QStringLiteral("failed");
  result.ok = result.commitState == QLatin1String("committed");
  result.code = result.ok ? QStringLiteral("committed") : committed;
  result.summary.insert(QStringLiteral("receipt"), receipt);
  return finish(log, record, result, false, {});
}

} // namespace

QVariantMap effectiveScalarParams(const QString &algorithmId, int contractVersion, const QVariantMap &user,
                                  QString *code) {
  const ValidatedParams validated = validateAndFillParams(algorithmId, contractVersion, user);
  if (code)
    *code = validated.code;
  return validated.ok ? validated.effective : QVariantMap();
}


ExecuteResult execute(const ExecuteRequest &request) {
  QString field;
  if (forbiddenAuthority(request.parameters, &field) || forbiddenAuthority(request.inputs, &field))
    return rejected(QStringLiteral("untrusted_authority"), field);
  if (request.operationDir.isEmpty() || request.documentId.isEmpty() || request.callerScope.isEmpty() ||
      request.clientRequestId.isEmpty())
    return rejected(QStringLiteral("operation_identity_required"));
  QString aliasCode;
  const QString capabilityId = request.capabilityId.contains(QLatin1Char('.')) &&
                                       CapabilityRegistry::spec(request.capabilityId, request.contractVersion).capabilityId.isEmpty()
                                   ? CapabilityRegistry::resolveAlias(request.capabilityId, &aliasCode)
                                   : request.capabilityId;
  if (!aliasCode.isEmpty() && capabilityId.isEmpty())
    return rejected(aliasCode);
  const CapabilitySpec capability = CapabilityRegistry::spec(capabilityId, request.contractVersion);
  if (capability.capabilityId.isEmpty())
    return rejected(CapabilityRegistry::spec(capabilityId, 1).capabilityId.isEmpty() ? QStringLiteral("unknown_capability")
                                                                                     : QStringLiteral("unsupported_version"));
  if (capability.exposure == CapabilityExposure::Unavailable)
    return rejected(QStringLiteral("unavailable"));
  if (capability.exposure == CapabilityExposure::Internal)
    return rejected(QStringLiteral("not_callable"));
  ExecuteResult bindingResult;
  if (!bindingOk(request, &bindingResult))
    return bindingResult;
  auto parameters = request.parameters;
  if (request.seismicTrace) {
    if (capability.capabilityId != QLatin1String("seismic.attributes.compute") || !request.inputs.isEmpty())
      return rejected(QStringLiteral("mixed_seismic_inputs"));
    if (request.publishIntent != QLatin1String("prepare") && request.publishIntent != QLatin1String("compute-only"))
      return rejected(QStringLiteral("unsupported_intent"));
    // The numerical contract requires this field. The resolved volume supplies
    // its authoritative value before the request fingerprint is calculated.
    if (!parameters.contains(QStringLiteral("SAMPLE_INTERVAL_MS")))
      parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 1.0);
  }
  ValidatedParams params =
      capability.algorithmId.isEmpty()
          ? ValidatedParams{}
          : validateAndFillParams(capability.algorithmId, capability.algorithmContractVersion, parameters);
  if (!capability.algorithmId.isEmpty() && !params.ok) {
    ExecuteResult result = rejected(params.code, params.issues.isEmpty() ? QString() : params.issues.first().field);
    result.issues = params.issues;
    return result;
  }
  SeismicTraceExecutionPlan tracePlan;
  if (request.seismicTrace) {
    const auto error = prepareSeismicTraceJob(request, params, tracePlan);
    if (!error.isEmpty())
      return rejected(error);
  }
  QJsonObject frozen = QJsonObject::fromVariantMap(request.inputs);
  frozen.remove(QStringLiteral("path"));
  frozen.insert(QStringLiteral("bindingMode"), request.bindingMode);
  frozen.insert(QStringLiteral("frozenHead"), request.frozenHead);
  frozen.insert(QStringLiteral("publishIntent"), request.publishIntent);
  if (request.seismicTrace)
    frozen.insert(QStringLiteral("seismicTrace"), seismicTraceIdentity(*request.seismicTrace));
  const QString hash = fingerprint(capability.capabilityId, capability.contractVersion, params.effective, frozen);
  OperationLog log(request.operationDir);
  const OperationLog::Accept accepted = log.accept(request.documentId, request.callerScope, request.clientRequestId, hash);
  if (accepted.code == QLatin1String("conflict"))
    return rejected(QStringLiteral("conflict"));
  if (accepted.code == QLatin1String("replay")) {
    if (accepted.record.commitState == QLatin1String("publishing")) {
      ExecuteResult uncertain = fromRecord(accepted.record, QStringLiteral("replay"));
      uncertain.ok = false;
      uncertain.code = QStringLiteral("commit_uncertain");
      uncertain.commitState = QStringLiteral("publishing");
      setResponse(uncertain, storedObject(accepted.record), accepted.record.computeCount);
      return uncertain;
    }
    auto replay = fromRecord(accepted.record, QStringLiteral("replay"));
    if (request.seismicTrace && replay.ok && replay.execution == QLatin1String("succeeded")) {
      if (!restoreSeismicTraceResult(request, replay)) {
        replay.ok = false;
        replay.code = request.cancelled && request.cancelled() ? QStringLiteral("cancelled")
                                                               : QStringLiteral("result_unavailable");
        replay.summary.remove(QStringLiteral("result_path"));
        replay.summary.remove(QStringLiteral("resultRef"));
      }
      setResponse(replay, QJsonObject::fromVariantMap(replay.summary), accepted.record.computeCount);
    }
    return replay;
  }
  if (accepted.code != QLatin1String("created"))
    return rejected(accepted.code.isEmpty() ? QStringLiteral("persist_failed") : accepted.code);
  OperationLog::Record record = accepted.record;
  record.execution = QStringLiteral("running");
  record.computeCount += 1;
  OperationLog::Record current;
  if (!log.save(record, &current)) {
    if (current.execution == QLatin1String("cancelled"))
      return fromRecord(current, QString());
    if (current.execution == QLatin1String("queued"))
      return finish(&log, current, rejected(QStringLiteral("persist_failed")), false, {});
    ExecuteResult result = fromRecord(accepted.record, QString());
    result.code = QStringLiteral("persist_failed");
    setResponse(result, {}, accepted.record.computeCount);
    return result;
  }
  if (request.onRunning)
    request.onRunning(record.operationId);
  if (cancelRequested(record.operationId) || (request.cancelled && request.cancelled()))
    return finish(&log, record, rejected(QStringLiteral("cancelled")), false, {});
  if (request.seismicTrace) {
    const auto stop = [&] { return cancelRequested(record.operationId) || (request.cancelled && request.cancelled()); };
    auto staged = executeSeismicTraceJob(request, tracePlan, record.operationId, stop);
    const auto path = staged.summary.value(QStringLiteral("result_path")).toString();
    if (staged.ok && stop())
      staged = rejected(QStringLiteral("cancelled"));
    auto result = finish(&log, record, std::move(staged), false, {});
    if (!result.ok && !path.isEmpty())
      QFile::remove(path);
    // A transient final write failure must not leave an operation marked as
    // running with no worker or record a reference to the discarded stage.
    if (result.code == QLatin1String("receipt_persist_failed"))
      return finish(&log, record, rejected(result.code), false, {});
    return result;
  }
  if (capability.capabilityId == QLatin1String("depth.convert_batch"))
    return executeDepth(request, params, &log, record);
  if (capability.capabilityId == QLatin1String("seismic.attributes.compute"))
    return executeSeismic(request, params, &log, record);
  if (capability.capabilityId == QLatin1String("constraint.idw"))
    return executeConstraint(request, params, &log, record);
  if (capability.capabilityId == QLatin1String("singlefactor.contours"))
    return executeContour(request, params, &log, record);
  return rejected(QStringLiteral("not_callable"));
}

ExecuteResult executePlan(const ExecuteRequest &host, const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges) {
  const auto plan = validatePlan(nodes, edges);
  if (!plan.ok) {
    auto result = rejected(plan.issues.first().code, plan.issues.first().field);
    result.issues = plan.issues;
    return result;
  }
  if (host.documentId.isEmpty() || host.callerScope.isEmpty() || host.clientRequestId.isEmpty() ||
      host.operationDir.isEmpty())
    return rejected(QStringLiteral("operation_identity_required"));
  // Check every literal reference before creating the first child operation.
  for (const auto &entry : plan.nodes) {
    const auto raster = entry.node.inputs.value(QStringLiteral("raster_path")).toString();
    if (!raster.isEmpty() && !isIssuedResult(host.documentId, raster))
      return rejected(QStringLiteral("untrusted_result_ref"), QStringLiteral("raster_path"));
  }
  QHash<QString, int> byId;
  for (int i = 0; i < plan.nodes.size(); ++i)
    byId.insert(plan.nodes[i].node.id, i);
  QVector<QString> produced(plan.nodes.size());
  QVariantList children;
  ExecuteResult last;
  for (const auto &id : plan.order) {
    const int index = byId.value(id);
    const auto &node = plan.nodes[index].node;
    ExecuteRequest request = host;
    request.capabilityId = node.capabilityId;
    request.contractVersion = node.contractVersion;
    const auto identity = QJsonDocument(QJsonArray{host.clientRequestId, node.id}).toJson(QJsonDocument::Compact);
    request.clientRequestId = QStringLiteral("plan-node:") + shaBytes(identity);
    request.publishIntent = node.intent;
    request.parameters = node.parameters;
    request.inputs = node.inputs;
    request.onRunning = {};
    for (const auto &edge : plan.edges) {
      if (edge.toNode != index)
        continue;
      if (produced[edge.fromNode].isEmpty())
        return rejected(QStringLiteral("missing_edge_value"), edge.toPort.id);
      request.inputs.insert(edge.inputKey, produced[edge.fromNode]);
    }
    last = execute(request);
    if (!last.ok)
      return last;
    produced[index] = last.summary.value(QStringLiteral("result_path")).toString();
    children.append(QVariantMap{{"nodeId",node.id},{"operationId",last.operationId},{"requestHash",last.requestHash}});
  }
  last.summary.insert(QStringLiteral("plan_order"), plan.order);
  last.summary.insert(QStringLiteral("child_operations"), children);
  last.responseJson = boundToolResponse(QJsonObject::fromVariantMap(last.summary));
  return last;
}


} // namespace paleo::capability
