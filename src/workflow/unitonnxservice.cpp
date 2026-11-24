// 层：功能
#include "unitonnxservice.h"
#include "ai/onnxpredictionservice.h"
#include "catalog/datacatalog.h"
#include "services/algorithmspec.h"
#include "services/unitonnxcapabilityspec.h"
#include "capabilitysourcepublication.h"
#include "unitonnxcontract.h"
#include "unitonnxoperation.h"
#include <QFileInfo>

namespace paleo {
namespace {
bool hex64(const QString &value) {
  if (value.size() != 64) return false;
  for (const auto &ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return false;
  return true;
}

AlgorithmSpec outputSpec() {
  return AlgorithmSpecRegistry::specAtVersion(QLatin1String(kUnitOnnxAlgorithmId), kUnitOnnxContractVersion);
}

// 规范意图（幂等键的来源）：v1 只支持 pinned-value，省略 binding_mode 与显式
// 写出是同一业务意图——不因表示不同把重放判成 idempotency_conflict。其它
// 取值已在工具表面与 capture 拒绝，这里原样保留不覆盖。
QJsonObject canonicalIntent(const capability::ExecuteRequest &request) {
  auto intent = capability::capabilityIntent(request);
  if (intent.value("bindingMode").toString().isEmpty())
    intent.insert("bindingMode", QStringLiteral("pinned-value"));
  return intent;
}

// 恢复期 worker：只消费冻结计划（frozenRequest.plan），重启后同一计划给出
// 同一输入快照——不重读地震原件、不依赖全局活动模型。
capability::OperationWorker unitWorker(const QPointer<PaleoOnnxService> &onnx) {
  return [onnx](const capability::OperationWorkContext &work) {
    capability::OperationWorkResult result;
    FrozenUnitOnnxPlan plan;
    QString error;
    if (!onnx) { result.code = "ort_provider_unavailable"; return result; }
    if (!FrozenUnitOnnxPlan::fromJson(work.frozenPlan, plan, &error)) { result.code = error; return result; }
    result.outputs = computeUnitOnnxPlan(onnx, plan, work.privateDir, result.diagnostics, result.summary,
                                         work.cancelled);
    result.ok = result.outputs.ok;
    result.code = result.outputs.error;
    return result;
  };
}

capability::OperationInputValidator unitInputValidator() {
  return [](const capability::OperationWorkContext &work) {
    FrozenUnitOnnxPlan plan;
    QString error;
    return FrozenUnitOnnxPlan::fromJson(work.frozenPlan, plan, &error) ? QString() : error;
  };
}

struct PrepareInputs {
  QString model, seismicVersionId, error;
  paleo::strat::UnitSelection unit;
};

PrepareInputs decodePrepareInputs(const QVariantMap &inputs) {
  PrepareInputs out;
  const auto fail = [&out](const QString &code) { out.error = code; return out; };
  if (inputs.size() != 3 || !inputs.contains(QStringLiteral("model")) ||
      !inputs.contains(QStringLiteral("seismic_version_id")) ||
      !inputs.contains(QStringLiteral("unit_selection")))
    return fail("unit_required_inputs");
  out.model = inputs.value(QStringLiteral("model")).toString();
  out.seismicVersionId = inputs.value(QStringLiteral("seismic_version_id")).toString();
  if (out.model.isEmpty() || out.model.size() > 256 || out.seismicVersionId.isEmpty() ||
      out.seismicVersionId.size() > 256)
    return fail("unit_required_inputs");
  const auto unit = inputs.value(QStringLiteral("unit_selection")).toMap();
  out.unit = paleo::strat::fromMap(unit);
  if (!out.unit.isValid() || out.unit.aggregate) return fail("unit_selection_invalid");
  return out;
}

QString stagingToken(const FrozenUnitOnnxPlan &plan) {
  auto token = paleo::strat::layerToken(paleo::strat::fromMap(plan.unit.toVariantMap()));
  if (token.isEmpty()) token = plan.unitKey.left(48);
  return token;
}
} // namespace

capability::CapabilityExecutionAdapter unitOnnxExecutionAdapter(PaleoOnnxService *onnx,
                                                                const QPointer<DataCatalog> &catalog) {
  capability::CapabilityExecutionAdapter adapter;
  const QPointer<PaleoOnnxService> owner(onnx);
  adapter.intent = [](const capability::ExecuteRequest &request) { return canonicalIntent(request); };
  adapter.capture = [owner, catalog](const capability::ExecuteRequest &request, const QJsonObject &intent,
                                     QString *error) {
    const auto fail = [error](const QString &code) {
      if (error) *error = code;
      return capability::CapabilityPreparation{};
    };
    if (request.bindingMode != QLatin1String("pinned-value"))
      return fail("binding_resolution_required");
    const auto phase = request.publishIntent;
    if (phase == QLatin1String("compute-only")) {
      const auto source = request.inputs.value(QStringLiteral("source_operation_id")).toString();
      if (source.isEmpty() || source.size() > 256) return fail("source_operation_required");
      if (!request.parameters.isEmpty() || request.inputs.contains(QStringLiteral("model")) ||
          request.inputs.contains(QStringLiteral("seismic_version_id")) ||
          request.inputs.contains(QStringLiteral("unit_selection")))
        return fail("inherited_unit_inputs_are_immutable");
      capability::FrozenOperation op;
      op.capabilityId = QLatin1String(kUnitOnnxCapabilityId);
      op.contractVersion = kUnitOnnxContractVersion;
      op.implementationVersion = QLatin1String(kUnitOnnxImplementationVersion);
      op.callerScope = request.callerScope;
      op.clientRequestId = request.clientRequestId;
      op.intent = intent;
      op.phase = phase;
      op.sourceOperationId = source;
      op.outputSpec = outputSpec();
      op.outputPorts = {QStringLiteral("OUTPUT")};
      op.worker = unitWorker(owner);
      op.inputValidator = unitInputValidator();
      capability::CapabilityPreparation preparation;
      preparation.worker = [op](const std::function<bool()> &, QString *) { return op; };
      return preparation;
    }
    if (phase != QLatin1String("prepare")) return fail("source_operation_required");
#if !PALEO_HAVE_ORT
    Q_UNUSED(catalog);
    return fail("ort_unavailable");
#else
    // owner 线程轻查询：注册表身份 + catalog 版本。重活全部下放 worker。
    if (!owner) return fail("ort_provider_unavailable");
    const auto inputs = decodePrepareInputs(request.inputs);
    if (!inputs.error.isEmpty()) return fail(inputs.error);
    if (!catalog || !catalog->isOpen()) return fail("catalog_unavailable");
    const auto entry = owner->registryEntryFor(inputs.model);
    if (entry.status != ModelRegistryEntry::Status::Ok || !hex64(entry.sha256Pinned))
      return fail("model_unavailable");
    const QString modelPinned = entry.sha256Pinned;
    const auto version = catalog->versionById(inputs.seismicVersionId);
    if (version.id != inputs.seismicVersionId) return fail("seismic_version_missing");
    if (!hex64(version.sha256)) return fail("seismic_version_fingerprint_required");
    // 引用必须是地震体版本（生产导入登记 seismic；测试/历史可用 seismic_volume）。
    const auto asset = catalog->assetById(version.assetId);
    if (asset.id != version.assetId ||
        (asset.type != QLatin1String("seismic") && asset.type != QLatin1String("seismic_volume")))
      return fail("seismic_version_type_invalid");
    UnitOnnxPrepareSource source;
    source.projectDir = request.projectDir;
    source.model = inputs.model;
    source.seismicVersionId = inputs.seismicVersionId;
    source.seismicSha256 = version.sha256;
    source.unit = inputs.unit;
    source.seismicPath = QFileInfo(DataCatalog::resolvedVersionPath(catalog->projectDir(), version)).canonicalFilePath();
    if (source.seismicPath.isEmpty()) return fail("seismic_source_unavailable");
    const QFileInfo file(source.seismicPath);
    if (!file.isFile()) return fail("seismic_source_unavailable");
    source.seismicSize = file.size();
    source.seismicMtimeMs = file.lastModified().toMSecsSinceEpoch();
    if (source.projectDir.isEmpty()) return fail("document_unavailable");
    capability::CapabilityPreparation preparation;
    preparation.worker = [owner, source, intent, request](const std::function<bool()> &cancelled,
                                                          QString *workerError) {
      FrozenUnitOnnxPlan plan;
      if (!prepareUnitOnnxPlan(owner, source, plan, cancelled, workerError))
        return capability::FrozenOperation{};
      capability::FrozenOperation op;
      op.capabilityId = QLatin1String(kUnitOnnxCapabilityId);
      op.contractVersion = kUnitOnnxContractVersion;
      op.implementationVersion = QLatin1String(kUnitOnnxImplementationVersion);
      op.callerScope = request.callerScope;
      op.clientRequestId = request.clientRequestId;
      op.intent = intent;
      op.phase = QStringLiteral("prepare");
      op.outputSpec = outputSpec();
      op.outputPorts = {QStringLiteral("OUTPUT")};
      op.parentVersionIds = {plan.seismicVersionId};
      op.targetNames.insert(QStringLiteral("OUTPUT"),
                            QStringLiteral("unit_%1_%2_OUTPUT").arg(stagingToken(plan), plan.model));
      op.frozenPlan = plan.toJson();
      return op;
    };
    // 物化完成后的 owner 复核：注册表钉值与 catalog 版本仍是冻结时的身份。
    preparation.validate = [owner, catalog, source, modelPinned] {
      if (!owner) return QStringLiteral("ort_provider_unavailable");
      const auto entry = owner->registryEntryFor(source.model);
      if (entry.status != ModelRegistryEntry::Status::Ok ||
          entry.sha256Pinned.compare(modelPinned, Qt::CaseInsensitive) != 0)
        return QStringLiteral("model_registry_changed");
      if (!catalog || !catalog->isOpen()) return QStringLiteral("catalog_unavailable");
      const auto version = catalog->versionById(source.seismicVersionId);
      if (version.id != source.seismicVersionId) return QStringLiteral("seismic_version_missing");
      if (version.sha256.compare(source.seismicSha256, Qt::CaseInsensitive) != 0)
        return QStringLiteral("seismic_version_changed");
      return QString();
    };
    return preparation;
#endif
  };
  adapter.validateFrozen = [catalog](const capability::OperationLog::Record &record) {
    if (record.capabilityId != QLatin1String(kUnitOnnxCapabilityId)) return QString();
    FrozenUnitOnnxPlan plan;
    QString error;
    if (!FrozenUnitOnnxPlan::fromJson(record.frozenRequest.value("plan").toObject(), plan, &error))
      return error;
    // W9：compute 的重算要能再次证明源版本身份；publish/replay 只消费已认证
    // 输出，不要求当前 ORT/模型仍可用（不在此加模型门）。
    if (record.frozenRequest.value("phase").toString() != QLatin1String("compute-only")) return QString();
    if (!catalog || !catalog->isOpen()) return QStringLiteral("catalog_unavailable");
    const auto version = catalog->versionById(plan.seismicVersionId);
    if (version.id != plan.seismicVersionId) return QStringLiteral("seismic_version_missing");
    if (version.sha256.compare(plan.seismicSha256, Qt::CaseInsensitive) != 0)
      return QStringLiteral("seismic_version_changed");
    return QString();
  };
  adapter.restoreInputValidator = [](const capability::OperationLog::Record &record, QString *error) {
    if (record.capabilityId != QLatin1String(kUnitOnnxCapabilityId)) { if (error) error->clear(); return capability::OperationInputValidator{}; }
    FrozenUnitOnnxPlan plan;
    QString invalid;
    if (!FrozenUnitOnnxPlan::fromJson(record.frozenRequest.value("plan").toObject(), plan, &invalid)) {
      if (error) *error = invalid;
      return capability::OperationInputValidator{};
    }
    return unitInputValidator();
  };
  adapter.restoreWorker = [owner](const capability::OperationLog::Record &record, QString *error) {
    if (record.capabilityId != QLatin1String(kUnitOnnxCapabilityId)) { if (error) error->clear(); return capability::OperationWorker{}; }
    if (record.frozenRequest.value("phase").toString() != QLatin1String("compute-only")) {
      // publish 走 copySource 路径（消费源操作工件，不新推理）；其余阶段
      // 不应有恢复 worker。
      if (error) *error = QStringLiteral("unit_operation_phase_unsupported");
      return capability::OperationWorker{};
    }
#if !PALEO_HAVE_ORT
    if (error) *error = QStringLiteral("ort_unavailable");
    return capability::OperationWorker{};
#else
    if (!owner) { if (error) *error = QStringLiteral("ort_provider_unavailable"); return capability::OperationWorker{}; }
    return unitWorker(owner);
#endif
  };
  return adapter;
}
} // namespace paleo
