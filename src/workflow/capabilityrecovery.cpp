// 层：功能
// workflow/capabilityrecovery.cpp — 先查持久身份，再冻结/分配/运行。
#include "capabilityrecovery_internal.h"
#include "services/operationretention.h"
#include "capabilitypublication_internal.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include "services/authenticatedartifactreader.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QThread>
#include <QUuid>
namespace paleo::capability {
namespace recovery_detail {
namespace { QMutex mutex; QHash<QString, std::weak_ptr<ActiveAttempt>> attempts;
QString key(const QString &root, const QString &id) { return QFileInfo(root).canonicalFilePath() + '/' + id; } }
std::shared_ptr<ActiveAttempt> acquire(const QString &root, const QString &id) {
  QMutexLocker lock(&mutex); const auto k = key(root,id);
  if (!attempts.value(k).expired()) return {}; auto value=std::make_shared<ActiveAttempt>(); attempts.insert(k,value); return value;
}
bool active(const QString &root,const QString &id) { QMutexLocker lock(&mutex); return !attempts.value(key(root,id)).expired(); }
void cancelActive(const QString &root,const QString &id) {
  QMutexLocker lock(&mutex);if(auto value=attempts.value(key(root,id)).lock())value->cancelled=true;
}
void release(const QString &root,const QString &id) { QMutexLocker lock(&mutex); attempts.remove(key(root,id)); }
OperationLog::Record authorized(OperationLog &log,const RecoveryHost &host,const QString &scope,const QString &id) {
  if (!host.context.open || scope.isEmpty()) return {};
  auto r=log.recordById(id);
  if (r.schemaVersion!=2 || r.documentId!=host.context.documentId || r.callerScope!=scope) return {}; return r;
}
QSet<QString> managedTargets(const OperationLog::Record &record,const QString &root) {
  QSet<QString> paths; QVector<AlgorithmOutputTarget> targets;
  if (publication_detail::targetsFromJson(root,record.stagePlan,targets))
    for(const auto &target:targets) paths.insert(QFileInfo(target.staging.absolutePath).canonicalFilePath());
  return paths;
}
QString store(OperationLog &log,OperationLog::Record &r,OperationLog::Transition why) {
  return log.saveDurable(r,r.stateRevision,r.ownerAttempt,&r,why);
}
ExecuteResult result(const OperationLog::Record &r) {
  ExecuteResult out; out.operationId=r.operationId; out.requestHash=r.requestHash; out.execution=r.execution;
  out.commitState=r.commitState; out.presentation=r.presentation; out.code=r.code;
  out.ok=r.execution==QLatin1String("succeeded") || (r.execution==QLatin1String("prepared") &&
      (r.frozenRequest.value("phase").toString()==QLatin1String("prepare") || r.code==QLatin1String("computed")));
  out.summary=QJsonDocument::fromJson(r.summaryJson).object().toVariantMap();
  out.summary.insert("contract",QString::fromLatin1(OperationContract));
  out.responseJson=QJsonDocument(publicOperation(r)).toJson(QJsonDocument::Compact); return out;
}
}
QString operationDigest(const QJsonObject &value) {
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QJsonObject publicOperation(const OperationLog::Record &r) {
  if(r.operationId.isEmpty()) return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","operation_not_found"}};
  QJsonObject out{{"contract",QLatin1String(OperationContract)},{"ok",true},{"code",r.code},{"operationId",r.operationId},{"execution",r.execution},
    {"commitState",r.commitState},{"presentation",r.presentation},{"revision",QString::number(r.stateRevision)},
    {"computeCount",r.computeCount},{"cancelRequested",r.cancelRequested},{"requestHash",r.requestHash},
    {"resultRef",QJsonObject{{"kind","operation-result-v2"},{"operationId",r.operationId}}}};
  const PublicationReceipt receipt{QJsonDocument::fromJson(r.summaryJson).object().value("receipt").toObject()};
  if(r.commitState==QLatin1String("committed") && receipt.valid() && receipt.operationId()==r.operationId && receipt.requestHash()==r.requestHash)
    out.insert("commitReceiptRef",receipt.outputSetId());
  return out;
}
CapabilityRecovery::CapabilityRecovery(QObject *parent):QObject(parent) {}
CapabilityRecovery::~CapabilityRecovery() {
  if(m_resultReader)m_resultReader->invalidate();
}
void CapabilityRecovery::setHostProvider(HostProvider provider){invalidateRuntimeHooks();m_host=std::move(provider);}
void CapabilityRecovery::invalidateRuntimeHooks() {
  if(m_resultReader)m_resultReader->invalidate();
  m_resultReader.reset();
  m_runtimeHooks.clear();m_runtimeDocument.clear();m_runtimeProject.clear();m_runtimeEpoch=0;
}
void CapabilityRecovery::setTaskService(PaleoTaskService *tasks){m_tasks=tasks;}
void CapabilityRecovery::setCheckpointHook(std::function<void(const QString&)> hook){m_checkpoint=std::move(hook);}
RecoveryHost CapabilityRecovery::live() {
  if(QThread::currentThread()!=thread())return {};
  auto h=m_host?m_host():RecoveryHost{};
  const auto document=h.context.open?h.context.documentId:QString();
  const auto project=h.context.open?QFileInfo(h.projectDir).canonicalFilePath():QString();
  if(document!=m_runtimeDocument || project!=m_runtimeProject || h.context.epoch!=m_runtimeEpoch) {
    invalidateRuntimeHooks();m_runtimeDocument=document;m_runtimeProject=project;m_runtimeEpoch=h.context.epoch;
  }
  return h;
}
OperationLog::Accept CapabilityRecovery::lookup(const QString &scope,const QString &request) {
  const auto h=live(); if(!h.context.open)return {"document_unavailable",{}};
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim); return log.lookup(h.context.documentId,scope,request);
}
CapabilityRecovery::Accepted CapabilityRecovery::accept(const FrozenOperation &input) {
  auto plan=input;
  const auto h=live(); OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto prior=log.lookup(h.context.documentId,plan.callerScope,plan.clientRequestId);
  const auto hash=operationDigest(plan.intent);
  if(prior.code==QLatin1String("found")) {
    if(prior.record.requestHash!=hash)return {false,"idempotency_conflict",prior.record};
    return {true,"replay",reconcile(h,prior.record)};
  }
  if(prior.code!=QLatin1String("not_found"))return {false,prior.code,{}};
  OperationLog::Record source;
  if(!plan.sourceOperationId.isEmpty()) {
    source=recovery_detail::authorized(log,h,plan.callerScope,plan.sourceOperationId);
    if(source.operationId.isEmpty() || (!plan.capabilityId.isEmpty() && source.capabilityId!=plan.capabilityId) || source.contractVersion!=plan.contractVersion ||
       (!plan.outputSpec.id.isEmpty() && source.stagePlan.value("outputSpec").toObject()!=publication_detail::specJson(plan.outputSpec)))return {false,"source_operation_mismatch",{}};
    if(!(plan.phase==QLatin1String("compute-only") && source.execution==QLatin1String("prepared") && source.preparedManifest.isEmpty()) &&
       !(plan.phase==QLatin1String("publish") && !source.artifactManifest.isEmpty()))return {false,"source_result_unavailable",{}};
    if(!publication_detail::specFromJson(source.stagePlan.value("outputSpec").toObject(),plan.outputSpec))return {false,"source_operation_mismatch",{}};
    if(!plan.frozenPlan.isEmpty() && plan.frozenPlan!=source.frozenRequest.value("plan").toObject())return {false,"mixed_source_operation_inputs",{}};
    QStringList parents;for(const auto &id:source.stagePlan.value("parents").toArray())parents.append(id.toString());
    if(!plan.parentVersionIds.isEmpty() && plan.parentVersionIds!=parents)return {false,"source_operation_mismatch",{}};
    plan.parentVersionIds=parents;plan.capabilityId=source.capabilityId;plan.implementationVersion=source.implementationVersion;
    plan.frozenPlan=source.frozenRequest.value("plan").toObject();
    auto originalPresentation=source.presentationIntent;originalPresentation.remove("members");
    auto requestedPresentation=plan.presentationIntent;requestedPresentation.remove("members");
    if(!requestedPresentation.isEmpty() && requestedPresentation!=originalPresentation)return {false,"source_operation_mismatch",{}};
    plan.presentationIntent=source.presentationIntent;
    if(!plan.inputValidator)plan.inputValidator=m_runtimeHooks.value(source.operationId).inputValidator;
  }
  auto code=host::checkContext(h.context,h.context,host::computeEffects(plan.phase) |
      (plan.externalRequest.isEmpty()?0U:unsigned(host::ExternalTransfer)));
  if(!code.isEmpty())return {false,code,{}};
  if(!log.ownsWrites() || !h.catalog)return {false,"project_write_claim_required",{}};
  const auto proof=h.catalog->publication(h.context.documentId,QUuid::createUuid().toString(QUuid::WithoutBraces),QString(64,'0'));
  if(proof.code!=QLatin1String("not_committed"))return {false,"publication_store_unavailable",{}};
  if(!source.operationId.isEmpty() && source.storeGeneration!=proof.storeGeneration)return {false,"publication_store_changed",{}};
  for(const auto &id:plan.parentVersionIds)if(id.isEmpty() || h.catalog->versionById(id).id!=id)return {false,"input_version_missing",{}};
  OperationLog::Record seed; seed.schemaVersion=2; seed.documentId=h.context.documentId;
  seed.callerScope=plan.callerScope;seed.clientRequestId=plan.clientRequestId;seed.requestHash=hash;
  seed.capabilityId=plan.capabilityId;seed.contractVersion=plan.contractVersion;
  seed.implementationVersion=plan.implementationVersion;seed.storeGeneration=proof.storeGeneration;
  QVector<AlgorithmOutputTarget> targets;QHash<QString,DerivedStaging> reservations;
  if(!source.operationId.isEmpty()) {
    if(!publication_detail::targetsFromJson(h.projectDir,source.stagePlan,targets))return {false,"source_operation_mismatch",{}};
  } else for(const auto &port:plan.outputSpec.outputs) {
    if(!plan.outputPorts.contains(port.id))continue;
    QString extension=port.constraints.value("fileExtension").toString();
    if(extension.isEmpty())extension=port.kind==QLatin1String("raster")?"tif":port.kind==QLatin1String("vector")?"gpkg":"bin";
    const auto staging=publication_detail::reserveTarget(h.catalog,h.projectDir,"agent_result",plan.targetNames.value(port.id,plan.capabilityId+"_"+port.id),port.id+'.'+extension,reservations,&code);
    if(!staging.isValid())return {false,code,{}}; targets.append({port.id,staging});
  }
  if(plan.phase!=QLatin1String("prepare") && targets.isEmpty() && plan.externalRequest.isEmpty())return {false,"output_contract_required",{}};
  seed.stagePlan=publication_detail::targetsJson(targets);
  seed.stagePlan.insert("outputSetId",source.operationId.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):source.stagePlan.value("outputSetId").toString());
  seed.stagePlan.insert("outputSpec",publication_detail::specJson(plan.outputSpec));
  seed.stagePlan.insert("parents",QJsonArray::fromStringList(plan.parentVersionIds));
  seed.frozenRequest={{"intent",plan.intent},{"plan",plan.frozenPlan},{"phase",plan.phase},{"sourceOperationId",plan.sourceOperationId}};
  seed.frozenRequest.insert("inputValidationRequired",bool(plan.inputValidator) || source.frozenRequest.value("inputValidationRequired").toBool());
  seed.presentationIntent=plan.presentationIntent;
  if(!seed.presentationIntent.isEmpty()) {
    QJsonArray members;for(const auto &target:targets)members.append(QJsonObject{{"portId",target.portId},
      {"assetId",target.staging.assetId},{"versionId",target.staging.versionId}});
    seed.presentationIntent.insert("members",members);
  }
  seed.externalRequest=plan.externalRequest;
  seed.planHash=storage::operationPlanHash(seed.frozenRequest,seed.stagePlan,seed.presentationIntent,seed.externalRequest);
  auto accepted=log.acceptDurable(seed);
  if(accepted.code!=QLatin1String("created"))return {false,accepted.code,accepted.record};
  if(plan.phase==QLatin1String("prepare")) {
    accepted.record.execution="prepared";accepted.record.code="prepared";
    const auto saved=recovery_detail::store(log,accepted.record);
    if(saved!=QLatin1String("ok"))return {false,saved,accepted.record};
  }
  // Plans/specs already live in the journal. Retain only non-serializable hooks;
  // a prepare-only operation never executes its own numerical worker.
  RuntimeHooks hooks{plan.phase==QLatin1String("prepare")?OperationWorker{}:plan.worker,plan.inputValidator};
  if(hooks.worker || hooks.inputValidator)m_runtimeHooks.insert(accepted.record.operationId,std::move(hooks));
  if(m_checkpoint)m_checkpoint("accepted");
  return {true,"accepted",accepted.record};
}
} // namespace paleo::capability
