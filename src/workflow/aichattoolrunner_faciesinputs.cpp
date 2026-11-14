// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "catalog/datacatalog.h"

void AiChatToolRunner::runFaciesInputs(const QJsonObject &args) {
  paleo::document::WellRef well;QString code;
  if(!paleo::document::wellRefFromJson(args.value("well_ref").toObject(),well,&code)) {
    finishActive(false,{{"code",code}});return;
  }
  if(!m_context.catalog||!m_context.catalog->isOpen()||!m_context.capabilityHost||!m_taskService) {
    finishActive(false,{{"code","well_provider_unavailable"}});return;
  }
  const auto host=m_context.capabilityHost->context();
  code=paleo::host::checkContext(host,host,paleo::host::Read);
  if(code.isEmpty()&&(host.documentId!=well.documentId||well.modelStoreId!="area-model"))code="well_document_mismatch";
  const auto entity=m_context.catalog->entityById(well.catalogEntityId);
  if(code.isEmpty()&&(entity.id.isEmpty()||entity.entityType!="well"||
      entity.extra.value("areaModelWellId").toLongLong()!=well.modelWellId))code="well_mapping_unavailable";
  if(!code.isEmpty()){finishActive(false,{{"code",code}});return;}
  paleo::wellfacies::InputInspectionJob job;job.projectDir=m_context.catalog->projectDir();job.well=well;
  if(m_context.faciesContext)job.context=m_context.faciesContext();
  auto result=std::make_shared<QJsonObject>();const auto generation=m_generation;
  auto *task=m_taskService->start(tr("查询井相输入元数据"),[job,result](PaleoTask *task) {
    *result=paleo::wellfacies::inspectInput(job,[task]{return task->cancelRequested();});
    return result->value("ok").toBool()?QString():result->value("code").toString();
  });
  if(generation!=m_generation){task->requestCancel();return;}
  m_activeTask=task;
  connect(task,&PaleoTask::finished,this,[this,generation,task=QPointer<PaleoTask>(task),result,host,well] {
    if(generation!=m_generation||!m_active||!task)return;
    const auto guard=m_context.capabilityHost;
    auto code=guard?paleo::host::checkContext(guard->context(),host,paleo::host::Read):QStringLiteral("document_unavailable");
    const auto live=m_context.catalog?m_context.catalog->entityById(well.catalogEntityId):CatalogEntity{};
    if(code.isEmpty()&&(live.id.isEmpty()||live.extra.value("areaModelWellId").toLongLong()!=well.modelWellId))code="well_mapping_changed";
    if(!code.isEmpty()){finishActive(false,{{"code",code}});return;}
    if(task->state()!=PaleoTask::State::Succeeded)finishActive(false,{{"code",task->errorText()}});
    else finishActive(true,*result);
  });
  if(!activeContextIsCurrent())task->requestCancel();
}
