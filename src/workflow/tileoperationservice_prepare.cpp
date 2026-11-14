// 层：功能
#include "tileoperationservice.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "ai/onnxpredictionservice.h"
#include "services/paleotaskservice.h"
#include <memory>

namespace paleo {
PaleoTask *TileOperationService::prepare(const QJsonObject &intent,const QString &scope,QObject *subscriber,
    std::function<void(capability::ExecuteResult)> finished,QString *error) {
  const auto reject=[&](const QString &code)->PaleoTask * {if(error)*error=code;return nullptr;};
  const auto denied=readiness(intent,false);if(!denied.isEmpty())return reject(denied);
  const auto context=m_host->context();const auto policy=host::checkContext(context,context,host::Read|host::PrivateStage);
  if(!policy.isEmpty())return reject(policy);
  if(intent.contains("source_operation_id"))return reject("unexpected_source_operation");
  FrozenRasterInput source;QString why;
  if(!m_source(intent.value("source_version_id").toString(),source,&why))return reject(why);
  QJsonObject presentation;
  if(m_presentation) {presentation=m_presentation(intent.value("model").toString(),context.horizon,&why);if(!why.isEmpty())return reject(why);}
  auto result=std::make_shared<FrozenTilePlan>();const auto onnx=m_onnx;
  const auto model=intent.value("model").toString();const auto parameters=intent.value("parameters").toObject().toVariantMap();
  const auto taskSession=m_tasks->session();
  auto *task=m_tasks->start("Prepare tile operation",[result,onnx,source,model,parameters](PaleoTask *running) {
    QString code;
    if(!prepareTilePlan(onnx,model,source,parameters,*result,[running]{return running->cancelRequested();},&code))return code;
    return QString();
  });
  if(!task)return reject("task_service_unavailable");
  const auto after=m_host->context();
  if(after.documentId!=context.documentId || after.epoch!=context.epoch || taskSession!=m_tasks->session())task->requestCancel();
  const QPointer<TileOperationService> self(this);const QPointer<QObject> listener(subscriber);
  connect(task,&PaleoTask::finished,this,[self,listener,task,result,context,taskSession,intent,scope,presentation,finished=std::move(finished)] {
    const auto done=[&](const QString &code) {
      task->failPublication(code);capability::ExecuteResult out;out.code=code;out.execution="rejected";out.commitState="not_requested";
      if(listener && finished)finished(out);
    };
    if(!self || !self->m_host || !self->m_tasks)return done("host_unavailable");
    if(task->state()!=PaleoTask::State::Succeeded)return done(task->state()==PaleoTask::State::Cancelled?"cancelled":task->errorText());
    const auto live=self->m_host->context();
    const auto invalid=host::checkContext(live,context,host::Read|host::PrivateStage);
    if(!invalid.isEmpty() || taskSession!=self->m_tasks->session())return done(invalid.isEmpty()?QStringLiteral("stale_session"):invalid);
    FrozenRasterInput current;QString sourceError;
    if(!self->m_source || !self->m_source(result->raster.versionId,current,&sourceError))
      return done(sourceError.isEmpty()?QStringLiteral("tile_source_provider_unavailable"):sourceError);
    if(current.versionId!=result->raster.versionId || current.sha256!=result->raster.sha256)
      return done("raster_catalog_binding_changed");
    QJsonObject declared;
    if(!current.catalogFeatureContract.isEmpty() && !normalizeRasterFeatureContract(current.catalogFeatureContract,declared))
      return done("source_feature_contract_invalid");
    if((!result->raster.catalogFeatureContract.isEmpty() && declared!=result->raster.catalogFeatureContract) ||
       (!declared.isEmpty() && declared!=result->raster.featureContract))return done("source_feature_contract_changed");
    result->raster.path=current.path;
    auto op=self->operation(intent,scope);op.frozenPlan=result->toJson();op.parentVersionIds={result->raster.versionId};
    op.presentationIntent=presentation;
    for(const auto &port:op.outputPorts)op.targetNames.insert(port,"tile_"+context.horizon+"_"+result->model+"_"+port);
    const auto accepted=self->m_host->recovery()->accept(op);
    if(!accepted.ok)return done(accepted.code);
    QString why;
    auto *settled=self->m_host->recovery()->start(scope,accepted.record.operationId,self,"Prepared tile operation",
      [listener,finished](auto out){if(listener && finished)finished(out);},false,&why);
    if(!settled)done(why);
  });
  return task;
}
}
