// 层：数据
#include "projectdata.h"
#include "documentrelationresolver.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "wellpathpositions.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QSet>
#include <cmath>
#include <utility>

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
void ProjectDataFacade::setInterpretationPlanProvider(std::function<InterpretationPlan()> provider) {
  m_interpretationPlan=std::move(provider);
}
ProjectDataFacade::InterpretationPlan ProjectDataFacade::interpretationPlan() const {
  return m_interpretationPlan?m_interpretationPlan():nullptr;
}
QVector<TdSample> ProjectDataFacade::tdTableForSelection(const QString &wellId,
    const paleo::document::RelationInput &input,const std::function<bool()> &cancelled) const {
  using State=paleo::document::RelationInput::State;
  if(input.state==State::Unselected)return tdTableFor(wellId);
  if(input.state==State::Selected && input.request.use.wellEntityId==wellId)return tdTableFor(input.request,cancelled);
  m_lastError=input.error.isEmpty()?QStringLiteral("所选时深关系不可用"):input.error;
  return {};
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
  if(request.use.context.targetVolume) {
    paleo::data::SeismicVolume volume;
    if(!service.readVolume(request.use.context.targetVolume->id,volume,error))return fail(QString::fromStdString(error));
    if(volume.sampling.timeType!="TWT" || volume.sampling.timeReference!=table.timeReference)
      return fail(QStringLiteral("时深表与所选地震体的双程时间基准不一致"));
  }
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

QHash<QString,QVector<WellTop>> ProjectDataFacade::pinnedMappingTops(
    const paleo::document::InterpretationContext &context,const QStringList &horizons,
    QString *error,const std::function<bool()> &cancelled) const {
  if(error)error->clear();
  const auto fail=[&](const QString &why){if(error)*error=why;return QHash<QString,QVector<WellTop>>{};};
  if(!m_catalog || !context.valid() || context.schemeId.isEmpty() || context.modelStoreId!="area-model")
    return fail(QStringLiteral("编图分层方案无效"));
  paleo::data::DataService service;std::string detail;paleo::data::StratigraphicScheme scheme;
  if(!service.openProjectReadOnly(paleo::area_model::directory(projectDir()).toUtf8().toStdString(),detail) ||
     !service.readStratigraphicScheme(context.schemeId.toStdString(),context.schemeRevision.toStdString(),scheme,detail))
    return fail(QString::fromStdString(detail));
  if(!context.subIntervalId.isEmpty())return fail(QStringLiteral("等厚编图需要父层间单元边界"));
  QHash<QString,QString> names;
  for(const auto &boundary:scheme.boundaries) {
    const auto name=QString::fromStdString(boundary.name);
    if(horizons.contains(name))names.insert(QString::fromStdString(boundary.id.value),name);
  }
  QHash<QString,QPair<QString,QString>> intervals;
  for(const auto &interval:scheme.intervals) {
    const auto id=QString::fromStdString(interval.id.value);
    if(interval.status=="valid" && (context.intervalId.isEmpty()||context.intervalId==id))
      intervals.insert(id,{names.value(QString::fromStdString(interval.top.value)),names.value(QString::fromStdString(interval.bottom.value))});
  }
  if(!context.intervalId.isEmpty() && !intervals.contains(context.intervalId))
    return fail(QStringLiteral("编图父层间单元不属于所选方案修订"));
  QHash<qlonglong,QString> wells;
  for(const auto &well:m_catalog->storedEntities("well")) {
    const auto id=well.extra.value("areaModelWellId").toLongLong();
    if(id>0)wells.insert(id,well.id);
  }
  QHash<QString,QVector<WellTop>> result;
  QHash<QString,QSet<QString>> conflicts;
  for(const auto &occ:scheme.wellOccurrences) {
    if(cancelled&&cancelled())return fail(QStringLiteral("已取消分层读取"));
    const auto well=wells.value(occ.wellId);
    if(well.isEmpty() || occ.status=="pending" || occ.depthUnit!="m" ||
       (occ.depthType!="MD" && occ.depthType!="TVD"))continue;
    const auto pair=intervals.value(QString::fromStdString(occ.interval.value));
    const auto append=[&](const QString &name,double depth) {
      if(name.isEmpty() || !std::isfinite(depth))return;
      auto &tops=result[well];
      for(const auto &existing:tops)if(existing.horizon==name) {
        const double value=occ.depthType=="MD"?existing.md:existing.tvd;
        if(!std::isfinite(value) || std::abs(value-depth)>1e-6)conflicts[well].insert(name);
        return;
      }
      WellTop top;top.horizon=name;
      (occ.depthType=="MD"?top.md:top.tvd)=depth;
      tops.append(top);
    };
    append(pair.first,occ.top);append(pair.second,occ.bottom);
  }
  for(auto it=result.begin();it!=result.end();++it) {
    if(cancelled&&cancelled())return fail(QStringLiteral("已取消分层读取"));
    const auto survey=trajectoryFor(it.key());
    const auto names=conflicts.value(it.key());
    it.value().removeIf([&](const WellTop &top){return names.contains(top.horizon);});
    if(survey)for(auto &top:it.value())if(std::isfinite(top.md))top.tvd=survey->tvdAt(top.md);
  }
  return result;
}
