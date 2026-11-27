// 层：功能
#include "mockseismicinputs.h"
#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "services/analysisunitcontext.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include "services/subintervalfactors.h"
#include <cmath>

QVariantList mockSeismicCalibrationSources(DataCatalog *catalog,const QString &project,
    const QString &horizon,const paleo::strat::UnitSelection &unit){
  QVariantList sources;if(!catalog)return sources;
  QVariantMap positions;
  QMap<qlonglong, QStringList> entityIds;
  paleo::data::StratigraphicScheme scheme;
  if (unit.isValid()) paleo::strat::loadProjectSchemeFromDir(project,unit.schemeId,unit.revision,scheme);
  paleo::data::DataService ds;
  std::string error;
  if(ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),error))
    for(const auto &entity:catalog->entities("well")){
      const auto id=entity.extra.value("areaModelWellId").toLongLong();paleo::data::WellInfo well;
      if(id>0) entityIds[id].append(entity.id);
      if(id<=0||!ds.readWellInfo(id,well,error)||!std::isfinite(well.wellheadX)||!std::isfinite(well.wellheadY)||well.coordinateUnit!="m"||well.coordinateSystem.empty())continue;
      positions.insert(entity.id,QVariantMap{{"id",id},{"name",entity.name},{"x",well.wellheadX},{"y",well.wellheadY},{"crs",QString::fromStdString(well.coordinateSystem)}});
    }
  const auto versions=catalog->versions();
  for(auto it=versions.crbegin();it!=versions.crend();++it){
    const auto &extra=it->extra;
    const auto asset=catalog->assetById(it->assetId);
    const bool intervals = asset.type == QLatin1String("well_facies_prediction");
    if(extra.value("stale").toBool() || (intervals && !unit.isValid())) continue;
    if(!intervals && (extra.value("horizon").toString()!=horizon||
        (extra.value("kind")!="wells_prediction"&&extra.value("result_kind")!="formation_prediction"&&
         extra.value("result_kind")!="dominant_facies")))continue;
    if(unit.isValid()){
      const auto source=analysisResultUnitSelection(catalog,*it);
      if(!source.isValid()||source.schemeId!=unit.schemeId||source.intervalId!=unit.intervalId||
          source.subIntervalId!=unit.subIntervalId||source.aggregate!=unit.aggregate||extra.value("stale").toBool())continue;
      if(intervals) {
        if(!paleo::strat::analysisUnitGeometryCurrentFor(project,source,paleo::strat::AnalysisGeometryDependency::WellRange,
                 extra.value("analysisWellId").toLongLong())) continue;
      } else if(source.revision!=unit.revision && !paleo::strat::analysisUnitGeometryCurrent(project,source)) continue;
    }
    QVariantMap candidate{{"title",asset.displayName+QStringLiteral(" · v%1").arg(it->versionNumber)},{"version_id",it->id},{"path",DataCatalog::resolvedVersionPath(project,*it)},
      {"sha256",it->sha256},{"facies",extra.value("facies")},{"native_positions",positions}};
    if(intervals) {
      const auto modelId=extra.value("modelWellId").toLongLong();
      auto id=extra.value("wellEntityId").toString();
      const auto choices=entityIds.value(modelId);
      if(id.isEmpty() && choices.size()==1) id=choices.first();
      candidate.insert("format","intervals");
      if(id.isEmpty() && modelId>0 && choices.isEmpty()) {
        paleo::data::WellInfo native; std::string detail;
        if(ds.readWellInfo(modelId,native,detail) && native.coordinateUnit=="m" && !native.coordinateSystem.empty() &&
           std::isfinite(native.wellheadX) && std::isfinite(native.wellheadY)) {
          id=QStringLiteral("model:%1").arg(modelId);
          positions.insert(id,QVariantMap{{"id",modelId},{"name",QString::fromStdString(native.wellName)},{"x",native.wellheadX},{"y",native.wellheadY},
                                          {"crs",QString::fromStdString(native.coordinateSystem)}});
        }
      }
      const auto native=positions.value(id).toMap();
      if(id.isEmpty() || native.isEmpty() || (modelId>0 && native.value("id").toLongLong()!=modelId)) {
        candidate.insert("invalid_identity",true);
      } else {
        const auto range=paleo::strat::analysisWellRange(scheme,unit,id,modelId);
        if(!range.resolved || range.depthType!="MD" || range.depthUnit!="m" ||
           range.wellId!=extra.value("analysisWellId").toLongLong()) candidate.insert("exclude_reason","unresolved_md_range");
        else { candidate.insert("top",range.top); candidate.insert("base",range.bottom); }
      }
      candidate.insert("well_id",id); candidate.insert("position",native);
    }
    sources << candidate;
  }
  return sources;
}
