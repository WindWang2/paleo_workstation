// 层：功能
// workflow/capabilityrecovery_commit.cpp — 只用同 Catalog 的 SQL 提交证据修复控制状态。
#include "capabilityrecovery_internal.h"
#include "capabilitypublication_internal.h"
#include "catalog/datacatalog.h"
#include <QJsonDocument>
namespace paleo::capability {
OperationLog::Record CapabilityRecovery::reconcile(const RecoveryHost &h,OperationLog::Record r) {
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  if(r.schemaVersion!=2)return r;
  const bool writable=log.ownsWrites() && host::checkContext(h.context,h.context,host::Read|host::PrivateStage).isEmpty();
  if(r.commitState==QLatin1String("publishing")) {
    const auto proof=h.catalog?h.catalog->publication(r.documentId,r.operationId,r.requestHash):PublicationLookup{};
    if(proof.code==QLatin1String("committed")) {
      r.commitState="committed";r.execution="succeeded";r.code="reconciled";
      auto summary=QJsonDocument::fromJson(r.summaryJson).object(); summary.insert("receipt",proof.receipt.value);
      r.summaryJson=QJsonDocument(summary).toJson(QJsonDocument::Compact);
      const auto committedFact=r;
      if(!writable)r.code="committed_log_repair_pending";
      else if(recovery_detail::store(log,r,OperationLog::Transition::Reconcile)!=QLatin1String("ok")) {
        r=committedFact;r.code="committed_log_repair_pending";
      }
    } else if(proof.code==QLatin1String("not_committed") && proof.storeGeneration==r.storeGeneration) {
      r.commitState="not_requested";r.execution="prepared";r.code="not_committed";
      if(writable)recovery_detail::store(log,r,OperationLog::Transition::Reconcile);
    } else { r.code="commit_uncertain"; if(writable)recovery_detail::store(log,r); }
  }
  if(writable && r.execution==QLatin1String("running") && !recovery_detail::active(h.projectDir,r.operationId)) {
    r.execution=r.externalRequest.isEmpty()?"interrupted":"external_unknown";
    r.code=r.externalRequest.isEmpty()?"interrupted":"external_outcome_unknown";
    recovery_detail::store(log,r,OperationLog::Transition::Reconcile);
  }
  return r;
}

ExecuteResult CapabilityRecovery::finish(const RecoveryHost &h,OperationLog::Record r,const OperationWorkResult &work) {
  const QPointer<CapabilityRecovery> guard(this);const auto checkpoint=m_checkpoint;
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto expectedAttempt=r.ownerAttempt;
  r=log.recordById(r.operationId);
  if(r.ownerAttempt!=expectedAttempt)return recovery_detail::result(r);
  if(r.execution!=QLatin1String("prepared") || r.preparedManifest.isEmpty())return recovery_detail::result(r);
  if(r.frozenRequest.value("phase").toString()!=QLatin1String("publish"))return recovery_detail::result(r);
  if(!work.ok || work.prepared.members.isEmpty() || publication_detail::preparedJson(work.prepared)!=r.preparedManifest) {
    r.code=work.code.isEmpty()?"prepared_verification_failed":work.code;recovery_detail::store(log,r);return recovery_detail::result(r);
  }
  const auto current=live();const auto effects=recovery_detail::operationEffects(r);
  QString code=effects?host::checkContext(current.context,h.context,effects):QStringLiteral("invalid_execution_effects");
  if(code.isEmpty() && current.validateFrozen)code=current.validateFrozen(r);
  if(!guard)return recovery_detail::result(r);
  if(code.isEmpty() && r.cancelRequested)code="cancelled";
  if(!code.isEmpty()){r.code=code;recovery_detail::store(log,r);return recovery_detail::result(r);}
  AlgorithmSpec spec;
  if(!publication_detail::specFromJson(r.stagePlan.value("outputSpec").toObject(),spec))return {};
  r.commitState="publishing";r.code="publishing";
  if(recovery_detail::store(log,r)!=QLatin1String("ok"))return recovery_detail::result(r);
  if(checkpoint)checkpoint("publishing");
  if(!guard)return recovery_detail::result(r);
  const auto latest=log.recordById(r.operationId);
  if(latest.ownerAttempt!=r.ownerAttempt || latest.planHash!=r.planHash || latest.commitState!="publishing")
    return recovery_detail::result(latest);
  r=latest;code=r.cancelRequested?QStringLiteral("cancelled"):host::checkContext(live().context,h.context,effects);
  if(!code.isEmpty()){r.code=code;recovery_detail::store(log,r);return recovery_detail::result(r);}
  QStringList parents;for(const auto &id:r.stagePlan.value("parents").toArray())parents.append(id.toString());
  DerivedAssetRegistrar registrar(current.catalog,current.projectDir);DerivedOutputSetReceipt receipt;
  const bool committed=registrar.commitSet(spec,work.prepared,parents,"agent-operation:"+r.operationId,{},receipt);
  if(!guard)return recovery_detail::result(r); // Catalog.changed can synchronously destroy the owner; SQL remains authoritative.
  if(committed && checkpoint)checkpoint("catalog_committed");
  if(!guard)return recovery_detail::result(r);
  if(committed) {
    r.commitState="committed";r.execution="succeeded";r.code="committed";
    auto summary=QJsonDocument::fromJson(r.summaryJson).object();summary.insert("receipt",QJsonObject::fromVariantMap(receipt.toVariantMap()));
    r.summaryJson=QJsonDocument(summary).toJson(QJsonDocument::Compact);const auto fact=r;
    if(recovery_detail::store(log,r)!=QLatin1String("ok")){r=fact;r.code="committed_log_repair_pending";return recovery_detail::result(r);}
  } else r=reconcile(current,log.recordById(r.operationId));
  if(r.commitState!=QLatin1String("committed")) {
    if(r.commitState==QLatin1String("not_requested")){r.code=receipt.code;recovery_detail::store(log,r);}
    return recovery_detail::result(r);
  }
  r=presentCommitted(current,r);
  return recovery_detail::result(r);
}
OperationLog::Record CapabilityRecovery::presentCommitted(const RecoveryHost &h,OperationLog::Record r) {
  const QPointer<CapabilityRecovery> guard(this);
  if(r.commitState!="committed" || r.presentationIntent.isEmpty() || r.presentation=="presented" || r.presentation=="superseded")return r;
  if(!host::checkContext(live().context,h.context,host::Read|host::Presentation).isEmpty() || !h.catalog)return r;
  if(!h.presentationState || !h.present)return r;
  OperationLog log(h.operationDir,h.projectDir,h.writeClaim);if(!log.ownsWrites())return r;
  auto summary=QJsonDocument::fromJson(r.summaryJson).object();const int count=summary.value("presentationAttempts").toInt();
  const auto state=h.presentationState(r.presentationIntent);
  if(!guard || !host::checkContext(guard->live().context,h.context,host::Read|host::Presentation).isEmpty())return r;
  if(state.isEmpty() || state.startsWith(QLatin1String("unavailable:")))return r;
  if(state==QLatin1String("presented"))r.presentation="presented";
  else if(state!=r.presentationIntent.value("expectedState").toString())r.presentation="superseded";
  else if(count>=3){r.code="presentation_retry_exhausted";return r;}
  else {
    const auto proof=h.catalog->publication(r.documentId,r.operationId,r.requestHash);
    if(proof.code!="committed")return r;
    summary.insert("presentationAttempts",count+1);r.summaryJson=QJsonDocument(summary).toJson(QJsonDocument::Compact);
    r.presentation=h.present(r.presentationIntent,proof.receipt);
    if(!guard)return r;
    if(r.presentation!="presented" && r.presentation!="superseded")r.presentation="pending";
  }
  const auto fact=r;
  if(recovery_detail::store(log,r)!=QLatin1String("ok")){r=fact;r.code="committed_log_repair_pending";}
  return r;
}
} // namespace paleo::capability
