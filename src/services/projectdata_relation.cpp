// 层：数据
#include "projectdata.h"
#include "documentrelationresolver.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "wellpathpositions.h"
#include <paleo_data/service.h>
#include <QDir>
#include <cmath>

namespace {
bool persistedChoice(const DataCatalog &catalog,const paleo::document::RelationResolveRequest &request) {
  const auto &object=request.object;
  if(request.selectedBindingId.isEmpty() || request.catalogRevision!=catalog.catalogRevision())return false;
  CatalogVersion version;
  // Scope lookup is stamped by the document. Also reject a historical locator
  // alias that still carries a superseded copy of a primary binding.
  for(const auto &candidate:catalog.versionsForAsset(request.selected.assetId))
    if(candidate.sha256==object.source.fileRevisionId && candidate.extra.value("areaModelSourceId").toLongLong()==object.source.sourceId &&
       (version.id.isEmpty() || candidate.versionNumber>=version.versionNumber))version=candidate;
  if(version.id.isEmpty() || version.assetId!=request.selected.assetId || version.sha256!=object.source.fileRevisionId ||
     version.id!=request.selected.versionId || version.extra.value("areaModelSourceId").toLongLong()!=object.source.sourceId)return false;
  const auto bindings=version.extra.value("documentSourceBindings").toMap();
  int selected=0;bool matched=false;
  for(const auto &value:bindings) {
    paleo::document::SourceBinding binding;paleo::document::RelationUseSpec use;paleo::document::ModelObjectRef ref;
    if(!paleo::document::sourceBindingFromJson(QJsonObject::fromVariantMap(value.toMap()),binding) ||
       !binding.primary || binding.decision!=paleo::document::BindingDecision::Confirmed)continue;
    if(paleo::document::relationBinding(binding,use,ref) && use==request.use) {
      ++selected;if(ref==object && binding.id==request.selectedBindingId)matched=true;
    }
  }
  return selected==1 && matched;
}
}
QVector<TdSample> ProjectDataFacade::tdTableFor(const paleo::document::RelationResolveRequest &request,
    const std::function<bool()> &cancelled) const {
  m_lastError.clear();
  const auto fail=[&](const QString &why){m_lastError=why;return QVector<TdSample>{};};
  if(cancelled && cancelled())return fail(QStringLiteral("已取消时深读取"));
  if(!m_catalog || request.use.relation!=paleo::document::RelationKind::TimeDepth ||
     request.object.kind!=paleo::document::ObjectKind::TimeDepth ||
     QDir::cleanPath(request.selected.modelDirectory)!=QDir::cleanPath(paleo::area_model::directory(projectDir())))
    return fail(QStringLiteral("时深用途不属于当前工程的模型"));
  const auto well=m_catalog->storedEntityById(request.use.wellEntityId);
  if(well.entityType!="well" || well.id!=request.wellEntityId ||
     well.extra.value("areaModelWellId").toLongLong()!=request.use.modelWellId ||
     request.mappedModelWellId!=request.use.modelWellId || !persistedChoice(*m_catalog,request))
    return fail(QStringLiteral("时深用途缺少当前文档已确认的明确对象选择"));
  const auto resolved=paleo::document::resolveDocumentRelation(request);
  if(!resolved.ok)return fail(resolved.error);
  paleo::data::DataService service;paleo::data::TimeDepthTable table;std::string error;
  if(!service.openProjectReadOnly(request.selected.modelDirectory.toUtf8().toStdString(),error) ||
     !service.readTimeDepthAtRevision(request.object.id,request.object.revision.toStdString(),table,error,cancelled,100000))
    return fail(QString::fromStdString(error));
  if(table.depthInfo.depthType!="MD" || table.depthInfo.mdReference.empty() ||
     (table.depthInfo.depthUnit!="m" && table.depthInfo.depthUnit!="ft") ||
     (table.timeUnit!="ms" && table.timeUnit!="s") || (table.timeType!="TWT" && table.timeType!="OWT") ||
     table.timeReference.empty() || (!table.timeSign.empty() && table.timeSign!="negated"))
    return fail(QStringLiteral("所选时深关系缺少已确认的MD起算面、单位、时间类型、符号或基准"));
  QVector<TdSample> result;result.reserve(qsizetype(table.depthTimePairs.size()));
  const double depthScale=table.depthInfo.depthUnit=="ft"?.3048:1.;
  const double timeScale=(table.timeUnit=="s"?1000.:1.)*(table.timeType=="OWT"?2.:1.);
  qsizetype index=0;
  for(const auto &point:table.depthTimePairs) {
    if((index++&4095)==0 && cancelled && cancelled())return fail(QStringLiteral("已取消时深读取"));
    TdSample row;row.md=point.depthValue*depthScale;row.timeMs=point.timeValue*timeScale;
    row.timeDepthId=request.object.id;row.timeReference=QString::fromStdString(table.timeReference);
    if(!std::isfinite(row.md) || !std::isfinite(row.timeMs))return fail(QStringLiteral("所选时深关系含缺失或非有限采样"));
    result.append(row);
  }
  attachTimeDepthPositions(table,paleo::area_model::confirmedProjectCrs(projectDir()),result);
  if(cancelled && cancelled())return fail(QStringLiteral("已取消时深读取"));
  return result;
}
