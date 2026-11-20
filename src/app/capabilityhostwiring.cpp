// 层：组装根
#include "capabilityhostwiring.h"
#include "catalog/datacatalog.h"
#include "linkage/selectioncontext.h"
#include "metadata/projectlock.h"
#include "services/capabilityseismicinput.h"
#include "workflow/constraintanalysisidentity.h" // P3：constraint.idw 受信单元冻结
#include "workflow/documentsession.h"
#include "workflow/commanddispatcher.h"
#include "workflow/aichattoolrunner.h"
#include <QCryptographicHash>
#include <QSysInfo>
#include <cmath>
#include <limits>

namespace {
using namespace paleo;

QString freezeInput(capability::ExecuteRequest &request, document::DocumentSession *document,
                   SelectionContext *selectionContext, DataCatalog *catalog, const QString &projectDir) {
  if (!document || !document->isOpen()) return QStringLiteral("document_unavailable");
  if ((request.seismicTrace || request.inputs.contains(QStringLiteral("seismic_selection"))) &&
      request.capabilityId != QLatin1String("seismic.attributes.compute"))
    return QStringLiteral("mixed_seismic_inputs");
  if (request.bindingMode.isEmpty()) request.bindingMode = QStringLiteral("pinned-value");
  if (request.bindingMode != QLatin1String("pinned-value"))
    return QStringLiteral("binding_resolution_required");
  if (request.seismicTrace)
    return capability::bindSeismicTrace(request, *document, request.seismicTrace->selection);
  // P3/W4：constraint.idw 的受信输入冻结——单元取宿主当前选择（DS 实体核验，
  // aggregate 拒绝），请求自带的 subinterval 与之不符即拒绝；points/constraints
  // 引用只把能解析到真实 catalog 版本的记为父版本。全部逻辑在
  // workflow/constraintanalysisidentity（组装根只做指针/目录接线，测试共用实现）。
  if (request.capabilityId == QLatin1String("constraint.idw"))
    return paleo::constraint_identity::freezeConstraintIdwRequest(
        request, projectDir, catalog,
        selectionContext ? selectionContext->activeUnit() : paleo::strat::UnitSelection{});
  if (!request.inputs.contains(QStringLiteral("seismic_selection"))) return {};
  const auto value = QJsonObject::fromVariantMap(request.inputs.value(QStringLiteral("seismic_selection")).toMap());
  capability::SeismicTraceSelection selection;
  if (!document::modelObjectRefFromJson(value.value(QStringLiteral("volume")).toObject(), selection.volume))
    return QStringLiteral("invalid_seismic_input");
  bool valid = false;
  selection.traceIndex = value.value(QStringLiteral("traceIndex")).toString().toLongLong(&valid);
  const auto first = value.value(QStringLiteral("firstSample"));
  const auto count = value.value(QStringLiteral("sampleCount"));
  const auto exactInt = [](const QJsonValue &v) {
    return v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble() &&
      v.toDouble() >= 0 && v.toDouble() <= double(std::numeric_limits<int>::max());
  };
  if (!valid || selection.traceIndex < 0 || !exactInt(first) || !exactInt(count) || count.toInt() < 1)
    return QStringLiteral("invalid_seismic_input");
  selection.firstSample = first.toInt(); selection.sampleCount = count.toInt();
  request.inputs.remove(QStringLiteral("seismic_selection"));
  if (!request.inputs.isEmpty()) return QStringLiteral("mixed_seismic_inputs");
  return capability::bindSeismicTrace(request, *document, selection);
}

QString validateInput(const capability::ExecuteRequest &request, document::DocumentSession *document) {
  if (!document || !document->isOpen()) return QStringLiteral("document_unavailable");
  if (request.bindingMode != QLatin1String("pinned-value"))
    return QStringLiteral("binding_resolution_required");
  if (!request.seismicTrace) return {};
  const auto &frozen = *request.seismicTrace;
  const auto live = document->source(frozen.source.ref);
  if (live.ref != frozen.source.ref || live.versionId != frozen.source.versionId ||
      live.modelDirectory != frozen.source.modelDirectory)
    return QStringLiteral("source_revision_changed");
  // SDK read independently checks object revision, source identity and sample axis.
  return {};
}
} // namespace

paleo::capability::HostBinding captureCapabilityBinding(
    paleo::document::DocumentSession *document, DataCatalog *catalog,
    ProjectDirLock *writeLock, SelectionContext *selection,AiChatToolRunner *chatRunner) {
  using namespace paleo;
  capability::HostBinding binding;
  auto &ctx = binding.context;
  if (!document || !catalog || !document->isOpen() || !catalog->isOpen()) return binding;
  ctx.open = true; ctx.documentId = document->documentId(); ctx.epoch = document->epoch();
  ctx.catalogRevision = catalog->catalogRevision(); ctx.mutationSequence = catalog->mutationSeq();
  ctx.persistentIdentity = document->hasPersistentIdentity(); ctx.readOnly = document->readOnly();
  ctx.recoveryRequired = document->recoveryRequired();
  binding.writeClaim = writeLock ? writeLock->claim() : nullptr;
  ctx.ownsWrites = binding.writeClaim && binding.writeClaim->isHeldFor(catalog->projectDir());
  ctx.allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
  if(chatRunner && chatRunner->externalTransferConfigured())ctx.allowedEffects|=host::ExternalTransfer;
  if(chatRunner && chatRunner->pythonExecutionAllowed())ctx.allowedEffects|=host::LocalExecute;
  if (selection) {
    ctx.selectionIds = selection->selectedIds();
    ctx.horizon = selection->activeHorizon();
    ctx.unitId = strat::layerToken(selection->activeUnit());
  }
  binding.catalog = catalog; binding.projectDir = catalog->projectDir();
  binding.operationDir = binding.projectDir + QStringLiteral("/.paleo/capability-ops");
  const QPointer<document::DocumentSession> guard(document);
  const QPointer<SelectionContext> selectionGuard(selection);
  const QPointer<DataCatalog> catalogGuard(catalog);
  const QString projectDir = binding.projectDir;
  binding.freeze = [guard, selectionGuard, catalogGuard, projectDir](capability::ExecuteRequest &request) {
    return freezeInput(request, guard, selectionGuard, catalogGuard, projectDir);
  };
  binding.validateInputs = [guard](const capability::ExecuteRequest &request) { return validateInput(request, guard); };
  return binding;
}

void configureCapabilityCallers(paleo::capability::CapabilityHost *service) {
  if (!service) return;
  const auto principal = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")).toUtf8();
  const auto id = QString::fromLatin1(QCryptographicHash::hash(
      QSysInfo::machineUniqueId() + QByteArray(1, '\0') + principal, QCryptographicHash::Sha256).toHex().left(24));
  service->setCallerScope(paleo::host::Caller::Ui, QStringLiteral("ui:local:") + id);
  service->setCallerScope(paleo::host::Caller::Batch, QStringLiteral("batch:local:") + id);
  service->setCallerScope(paleo::host::Caller::Agent, QStringLiteral("agent:local:") + id);
}

void bindCapabilityDispatcher(paleo::command::CommandDispatcher *dispatcher,
                             paleo::capability::CapabilityHost *service) {
  if (dispatcher) dispatcher->setCapabilityHost(service);
}
