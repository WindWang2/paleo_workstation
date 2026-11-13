// 层：功能
// workflow/capabilityhost_plan.cpp — 产品 DAG 必经可信宿主及持久父操作。
#include "capabilityhost.h"
#include "capabilitynativeoperation.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
namespace paleo::capability {
HostPrepared CapabilityHost::preparePlan(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges,
                                        host::Caller caller,const QString &session) {
  HostPrepared out;auto h=binding();
  const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!intent.documentId.isEmpty() && intent.documentId!=h.context.documentId)return fail("document_mismatch");
  const auto scope=callerScope(caller,session);if(scope.isEmpty())return fail("caller_unbound");
  if(intent.clientRequestId.isEmpty())return fail("client_request_id_required");
  const auto original=nativePlanIntent(intent,nodes,edges);
  const auto prior=m_recovery->lookup(scope,intent.clientRequestId);
  Prepared p;p.request=intent;p.request.documentId=h.context.documentId;p.request.callerScope=scope;
  p.request.projectDir=h.projectDir;p.request.operationDir=h.operationDir;p.request.bindingMode="pinned-value";p.request.catalog=nullptr;
  p.context=h.context;p.effects=host::computeEffects(intent.publishIntent);
  if(!p.effects)return fail("unsupported_intent");
  if(prior.code==QLatin1String("found")) {
    if(prior.record.schemaVersion!=2)return fail("legacy_unverifiable");
    if(prior.record.requestHash!=operationDigest(original))return fail("idempotency_conflict");
    p.recoveryId=prior.record.operationId;p.effects=host::Read;
  } else {
    if(prior.code!=QLatin1String("not_found"))return fail(prior.code);
    auto code=host::checkContext(h.context,h.context,p.effects);if(!code.isEmpty())return fail(code);
    p.frozen=freezeNativePlan(p.request,nodes,edges,&code);if(!code.isEmpty())return fail(code);
    p.frozen.intent=original;
  }
  // The opaque ticket owns the entire graph; start cannot replace its nodes/edges.
  p.intentDigest=requestDigest(intent);
  p.expiresAt=QDateTime::currentMSecsSinceEpoch()+5*60*1000;
  // check() uses the same original-request digest as individual capabilities.
  p.commandId="workflow.plan";
  if(m_prepared.size()>=128)return fail("prepared_capacity_exceeded");
  out.handle=QUuid::createUuid().toString(QUuid::WithoutBraces);m_prepared.insert(out.handle,std::move(p));
  out.ok=true;out.code=prior.code==QLatin1String("found")?"replay":"ok";return out;
}
} // namespace paleo::capability
