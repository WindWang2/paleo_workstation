// 层：功能
// Bounded pages authenticated against the frozen artifact, without repeated whole-file scans.
#include "capabilityrecovery_internal.h"
#include "capabilitypublication_internal.h"
#include "services/authenticatedartifactreader.h"
#include "services/paleotaskservice.h"
#include <QFileInfo>

namespace paleo::capability {
namespace {
QJsonObject readFailure(const QString &code) {
  return {{"contract",QLatin1String(OperationContract)},{"ok",false},{"code",code}};
}
struct ReadRequest {
  QString project,scopeKey,port;
  OperationLog::Record record;
  QSet<QString> allowedManaged;
  qint64 offset=0;
  int maxBytes=0;
};
QJsonObject authenticatedPage(const ReadRequest &request,artifacts::AuthenticatedArtifactReader &reader,PaleoTask *task) {
  auto manifest=request.record.artifactManifest;QJsonArray selected;
  for(const auto &entry:manifest.value("ports").toArray())
    if(entry.toObject().value("portId").toString()==request.port)selected.append(entry);
  manifest.insert("ports",selected);
  AlgorithmRunResult outputs;QString code;
  // Authorization, containment and managed-target validation are repeated even on a warm index.
  if(!publication_detail::outputsFromManifest(request.project,request.record.operationId,manifest,outputs,&code,
      {},false,request.allowedManaged) || outputs.artifacts.size()!=1)
    return readFailure(code.isEmpty()?QStringLiteral("result_unavailable"):code);
  const auto port=selected.first().toObject();bool sizeOk=false;
  const auto size=port.value("byteSize").toString().toLongLong(&sizeOk);
  if(!sizeOk)return readFailure("invalid_artifact_manifest");
  const artifacts::AuthenticatedArtifactReader::Identity identity{
      request.scopeKey,outputs.artifacts.first().absolutePath,port.value("sha256").toString(),size};
  const auto page=reader.read(identity,request.offset,request.maxBytes,[task]{return task->cancelRequested();});
  if(page.code!=QLatin1String("ok"))return readFailure(page.code);
  const auto next=request.offset+page.bytes.size();
  return {{"contract",QLatin1String(OperationContract)},{"ok",true},{"code","ok"},
    {"operationId",request.record.operationId},{"portId",request.port},{"encoding","base64"},
    {"offset",QString::number(request.offset)},{"nextOffset",QString::number(next)},{"hasMore",next<size},
    {"data",QString::fromLatin1(page.bytes.toBase64())},
    {"integrity",QJsonObject{{"scope","returned_bytes"},{"artifactSha256",identity.sha256}}}};
}
QString currentReadError(const RecoveryHost &current,const host::CallContext &expected,const ReadRequest &request) {
  const auto contextError=host::checkContext(current.context,expected,host::Read);
  if(!contextError.isEmpty())return contextError;
  if(QFileInfo(current.projectDir).canonicalFilePath()!=request.project)return "stale_session";
  OperationLog log(current.operationDir,current.projectDir,current.writeClaim);
  const auto record=recovery_detail::authorized(log,current,request.record.callerScope,request.record.operationId);
  if(record.operationId.isEmpty())return "operation_not_found";
  if(record.artifactManifest!=request.record.artifactManifest || record.stagePlan!=request.record.stagePlan)
    return "result_changed";
  return {};
}
}
PaleoTask *CapabilityRecovery::read(const QString &scope,const QString &id,const QString &portId,qint64 offset,int maxBytes,
    QObject *subscriber,std::function<void(QJsonObject)> finished,QString *error) {
  const auto h=live();OperationLog log(h.operationDir,h.projectDir,h.writeClaim);
  const auto record=recovery_detail::authorized(log,h,scope,id);
  const auto reject=[&](const QString &code)->PaleoTask*{if(error)*error=code;return nullptr;};
  if(record.operationId.isEmpty())return reject("operation_not_found");
  const auto contextError=host::checkContext(h.context,h.context,host::Read);
  if(!contextError.isEmpty())return reject(contextError);
  if(offset<0 || maxBytes<1 || maxBytes>24*1024)return reject("invalid_read_budget");
  if(!m_tasks || !subscriber)return reject("task_service_unavailable");
  if(!m_resultReader)m_resultReader=std::make_shared<artifacts::AuthenticatedArtifactReader>();
  const auto project=QFileInfo(h.projectDir).canonicalFilePath();
  const auto scopeKey=operationDigest({{"project",project},{"document",h.context.documentId},
    {"epoch",QString::number(h.context.epoch)},{"caller",scope},{"operation",id},{"port",portId}});
  const ReadRequest request{project,scopeKey,portId,record,recovery_detail::managedTargets(record,h.projectDir),offset,maxBytes};
  const QPointer<CapabilityRecovery> guard(this);
  const QPointer<QObject> listener(subscriber);
  const auto reader=m_resultReader;
  auto output=std::make_shared<QJsonObject>(readFailure("result_unavailable"));
  auto *task=m_tasks->start("Read operation result",[request,reader,output](PaleoTask *task){
    *output=authenticatedPage(request,*reader,task);
    return output->value("ok").toBool()?QString():output->value("code").toString();
  });
  // start() emits taskAdded synchronously; it may close the host or subscriber.
  if(!guard || !listener){if(task)task->requestCancel();return task;}
  QObject::connect(task,&PaleoTask::finished,guard.data(),
    [guard,task=QPointer<PaleoTask>(task),listener,reader,
      expected=h.context,request,output,finished=std::move(finished)] {
      if(!listener)return;
      auto code=guard?currentReadError(guard->live(),expected,request):QStringLiteral("document_unavailable");
      if(code.isEmpty() && guard->m_resultReader!=reader)code="stale_session";
      if(code.isEmpty() && (!task || task->cancelRequested()))code="cancelled";
      if(!code.isEmpty())*output=readFailure(code);
      if(finished)finished(*output);
    },Qt::SingleShotConnection); // Release the frozen request and bytes with this completion.
  return task;
}
} // namespace paleo::capability
