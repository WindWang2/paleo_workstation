// 层：功能
// workflow/capabilityrecovery_query.cpp — 有作用域的查询/事件/取消/有界结果读取。
#include "capabilityrecovery_internal.h"
#include "capabilitypublication_internal.h"
#include "services/paleotaskservice.h"
#include <QFile>
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
      {"unit",descriptor.value("unit")},{"axis",descriptor.value("axis")},{"encoding",descriptor.value("constraints").toObject().value("encoding")},
      {"byteSize",port.value("byteSize")},{"resultRef",QJsonObject{{"kind","operation-result-v2"},{"operationId",id},{"portId",port.value("portId")}}}});
  }
  out.insert("ports",ports);
  if(!r.artifactManifest.value("childOperations").toArray().isEmpty())out.insert("childOperations",r.artifactManifest.value("childOperations"));
  if(ports.isEmpty())out.insert("code","result_unavailable");return out;
}
PaleoTask *CapabilityRecovery::read(const QString &scope,const QString &id,const QString &portId,qint64 offset,int maxBytes,
    QObject *subscriber,std::function<void(QJsonObject)> finished,QString *error) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto r=recovery_detail::authorized(log,h,scope,id);
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(r.operationId.isEmpty())return reject("operation_not_found");
  if(offset<0 || maxBytes<1 || maxBytes>24*1024)return reject("invalid_read_budget");
  if(!m_tasks || !subscriber)return reject("task_service_unavailable");
  auto output=std::make_shared<QJsonObject>();const auto allowed=recovery_detail::managedTargets(r,h.projectDir);
  auto *task=m_tasks->start("Read operation result",[h,r,portId,offset,maxBytes,output,allowed](PaleoTask *task){
    AlgorithmRunResult artifacts;QString code;
    auto manifest=r.artifactManifest;QJsonArray selected;
    for(const auto &entry:manifest.value("ports").toArray())if(entry.toObject().value("portId").toString()==portId)selected.append(entry);
    manifest.insert("ports",selected);
    if(!publication_detail::outputsFromManifest(h.projectDir,r.operationId,manifest,artifacts,&code,
         [task]{return task->cancelRequested();},true,allowed)){*output={{"contract",QLatin1String(OperationContract)},{"ok",false},{"code",code.isEmpty()?"result_unavailable":code}};return code;}
    for(const auto &artifact:artifacts.artifacts)if(artifact.portId==portId) {
      QFile file(artifact.absolutePath);if(!file.open(QIODevice::ReadOnly) || offset>file.size() || !file.seek(offset))break;
      const auto bytes=file.read(maxBytes);const auto next=offset+bytes.size();
      *output={{"contract",QLatin1String(OperationContract)},{"ok",true},{"code","ok"},{"operationId",r.operationId},{"portId",portId},{"encoding","base64"},
        {"offset",QString::number(offset)},{"nextOffset",QString::number(next)},{"hasMore",next<file.size()},
        {"data",QString::fromLatin1(bytes.toBase64())}};return QString();
    }
    *output={{"contract",QLatin1String(OperationContract)},{"ok",false},{"code","result_unavailable"}};return QStringLiteral("result_unavailable");
  });
  QObject::connect(task,&PaleoTask::finished,subscriber,[output,finished=std::move(finished)]{if(finished)finished(*output);});return task;
}
} // namespace paleo::capability
