// 层：功能
#include "mockseismicinputs.h"
#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "services/analysisunitcontext.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <cmath>

QVariantList mockSeismicCalibrationSources(DataCatalog *catalog,const QString &project,
    const QString &horizon,const paleo::strat::UnitSelection &unit){
  QVariantList sources;if(!catalog)return sources;
  QVariantMap positions;
  if(unit.isValid()){
    paleo::data::DataService ds;std::string error;
    if(ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),error))
      for(const auto &entity:catalog->entities("well")){
        const auto id=entity.extra.value("areaModelWellId").toLongLong();paleo::data::WellInfo well;
        if(id<=0||!ds.readWellInfo(id,well,error)||!std::isfinite(well.wellheadX)||!std::isfinite(well.wellheadY)||well.coordinateUnit!="m"||well.coordinateSystem.empty())continue;
        positions.insert(entity.id,QVariantMap{{"id",id},{"x",well.wellheadX},{"y",well.wellheadY},{"crs",QString::fromStdString(well.coordinateSystem)}});
      }
  }
  const auto versions=catalog->versions();
  for(auto it=versions.crbegin();it!=versions.crend();++it){
    const auto &extra=it->extra;
    if(extra.value("horizon").toString()!=horizon||
        (extra.value("kind")!="wells_prediction"&&extra.value("result_kind")!="formation_prediction"&&
         extra.value("result_kind")!="dominant_facies"))continue;
    if(unit.isValid()){
      const auto source=analysisResultUnitSelection(catalog,*it);
      if(!source.isValid()||source.schemeId!=unit.schemeId||source.intervalId!=unit.intervalId||
          source.subIntervalId!=unit.subIntervalId||source.aggregate!=unit.aggregate||extra.value("stale").toBool())continue;
      if(source.revision!=unit.revision&&!paleo::strat::analysisUnitGeometryCurrent(project,source))continue;
    }
    sources<<QVariantMap{{"version_id",it->id},{"path",DataCatalog::resolvedVersionPath(project,*it)},
      {"sha256",it->sha256},{"facies",extra.value("facies")},{"native_positions",positions}};
  }
  return sources;
}
