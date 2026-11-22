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
  return preparePlanCommon(intent,nodes,edges,nullptr,caller,session);
}
HostPrepared CapabilityHost::preparePlanCommon(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges,
                                              const PlanExports *exports,host::Caller caller,const QString &session) {
  HostPrepared out;const QPointer<CapabilityHost> guard(this);const auto invalidated=m_preparationInvalidated;
  const auto fail=[&](const QString &code){out.code=code;return out;};
  const auto interrupted=[guard,invalidated] {
    return !guard?QStringLiteral("document_unavailable"):invalidated->load()?QStringLiteral("stale_session"):QString();
  };
  auto h=binding();if(const auto code=interrupted();!code.isEmpty())return fail(code);
  prunePrepared(h.context);const auto nativeFactory=m_nativeAdapterFactory;
  const auto native=nativeFactory?nativeFactory(h):CapabilityExecutionAdapter{};
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
  if(!native.freeze)return fail("execution_adapter_unavailable");
  if(!intent.documentId.isEmpty() && intent.documentId!=h.context.documentId)return fail("document_mismatch");
  const auto scope=callerScope(caller,session);if(scope.isEmpty())return fail("caller_unbound");
  if(intent.clientRequestId.isEmpty())return fail("client_request_id_required");
  const auto original=exports?nativePlanIntentV2(intent,nodes,edges,*exports):nativePlanIntent(intent,nodes,edges);
  const auto prior=m_recovery->lookup(scope,intent.clientRequestId);
  if(const auto code=interrupted();!code.isEmpty())return fail(code);
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
    // 票据拥有整张图（v2 含 exports）；start 不能替换节点/边/导出。
    p.frozen=exports?freezeNativePlanV2(p.request,nodes,edges,*exports,&code)
                    :freezeNativePlan(p.request,nodes,edges,&code);
    if(const auto stopped=interrupted();!stopped.isEmpty())return fail(stopped);
    if(!code.isEmpty())return fail(code);
    p.frozen.intent=original;
  }
  p.intentDigest=requestDigest(intent);
  p.expiresAt=QDateTime::currentMSecsSinceEpoch()+5*60*1000;
  // check() uses the same original-request digest as individual capabilities.
  p.commandId="workflow.plan";
  if(m_prepared.size()>=128)return fail("prepared_capacity_exceeded");
  out.handle=QUuid::createUuid().toString(QUuid::WithoutBraces);m_prepared.insert(out.handle,std::move(p));
  out.ok=true;out.code=prior.code==QLatin1String("found")?"replay":"ok";return out;
}
} // namespace paleo::capability
