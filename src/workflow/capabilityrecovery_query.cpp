// 层：功能
// workflow/capabilityrecovery_query.cpp — 有作用域的查询/事件/取消/结果描述。
#include "capabilityrecovery_internal.h"
#include <QJsonDocument>
namespace paleo::capability {
QJsonObject CapabilityRecovery::get(const QString &scope,const QString &id) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  return publicOperation(reconcile(h,recovery_detail::authorized(log,h,scope,id)));
}
QJsonObject CapabilityRecovery::events(const QString &scope,const QString &id,quint64 after,int pageSize,int maxBytes) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto r=recovery_detail::authorized(log,h,scope,id);
  if(r.operationId.isEmpty())return publicOperation(r);
  if(pageSize<1 || pageSize>64 || maxBytes<1024 || maxBytes>32*1024)return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","invalid_page_budget"}};
  const auto first=r.events.isEmpty()?r.stateRevision:r.events.first().toObject().value("revision").toString().toULongLong();
  if(after && after<first-1)return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","event_history_expired"},{"firstRevision",QString::number(first)}};
  if(after>r.stateRevision)return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","invalid_event_cursor"}};
  QJsonArray rows;quint64 next=after;
  for(const auto &entry:r.events) {
    const auto revision=entry.toObject().value("revision").toString().toULongLong();if(revision<=after)continue;
    auto candidate=rows;candidate.append(entry);
    const auto bytes=QJsonDocument(candidate).toJson(QJsonDocument::Compact).size();
    if(rows.isEmpty() && bytes>maxBytes-512)return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","event_page_budget_too_small"},{"minimumBytes",bytes+512}};
    if(candidate.size()>pageSize || bytes>maxBytes-512)break;
    rows=candidate;next=revision;
  }
  return {{"contract",QLatin1String(OperationContract)},{"ok",true},{"code","ok"},{"operationId",id},{"events",rows},{"nextRevision",QString::number(next)},
    {"hasMore",next<r.stateRevision},{"firstRevision",QString::number(first)},{"historyTruncated",first>1}};
}
QJsonObject CapabilityRecovery::cancel(const QString &scope,const QString &id) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  auto r=reconcile(h,recovery_detail::authorized(log,h,scope,id));
  if(r.operationId.isEmpty())return publicOperation(r);
  if(r.commitState==QLatin1String("committed")){auto out=publicOperation(r);out.insert("code","cancel_after_commit");return out;}
  const auto permitted=host::checkContext(h.context,h.context,host::Read|host::PrivateStage);
  if(!permitted.isEmpty())return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code",permitted}};
  if(r.commitState==QLatin1String("publishing")){auto out=publicOperation(r);out.insert("code","commit_uncertain");return out;}
  r.cancelRequested=true;r.code="cancel_requested";
  const auto code=recovery_detail::store(log,r);
  if(code!=QLatin1String("ok"))return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code",code}};
  m_runtimeHooks.remove(id);
  recovery_detail::cancelActive(h.projectDir,id);return publicOperation(r);
}
QJsonObject CapabilityRecovery::results(const QString &scope,const QString &id) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto r=reconcile(h,recovery_detail::authorized(log,h,scope,id));auto out=publicOperation(r);
  if(r.operationId.isEmpty())return out;
  QJsonArray ports;
  for(const auto &entry:r.artifactManifest.value("ports").toArray()) {
    const auto port=entry.toObject();QJsonObject descriptor;
    for(const auto &value:r.stagePlan.value("outputSpec").toObject().value("outputs").toArray())
      if(value.toObject().value("id")==port.value("portId"))descriptor=value.toObject();
    ports.append(QJsonObject{{"portId",port.value("portId")},{"sha256",port.value("sha256")},{"kind",descriptor.value("kind")},
      {"diagnostic",descriptor.value("constraints").toObject().value("diagnostic").toBool()},
      {"unit",descriptor.value("unit")},{"axis",descriptor.value("axis")},{"encoding",descriptor.value("constraints").toObject().value("encoding")},
      {"byteSize",port.value("byteSize")},{"resultRef",QJsonObject{{"kind","operation-result-v2"},{"operationId",id},{"portId",port.value("portId")}}}});
  }
  out.insert("ports",ports);
  if(!r.artifactManifest.value("childOperations").toArray().isEmpty())out.insert("childOperations",r.artifactManifest.value("childOperations"));
  if(ports.isEmpty())out.insert("code","result_unavailable");return out;
}
} // namespace paleo::capability
