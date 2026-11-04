// 层：QGIS 封装
#include "factorwellcoordinates.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include <paleo_data/service.h>
#include <qgscoordinatetransform.h>
#include <cmath>

QgsGeometry FactorWellCoordinates::resolve(const QString &id,const QgsGeometry &geometry,
                                          const QgsCoordinateReferenceSystem &source) const {
  if(native.contains(id))return native.value(id);
  if(geometry.isEmpty()||!source.isValid()||!crs.isValid())return {};
  if(source==crs)return geometry;
  try {
    QgsCoordinateTransform transform(source,crs,QgsCoordinateTransformContext{});
    if(!transform.isValid())return {};
    auto projected=geometry;projected.transform(transform);return projected;
  }catch(const QgsCsException &){return {};}
}

bool readFactorWellCoordinates(const DataCatalog *catalog,const QString &project,
                              FactorWellCoordinates &result,QString *error){
  result={};paleo::data::DataService ds;paleo::data::ProjectCoordinates coordinates;
  std::vector<paleo::data::WellInfoRecord> wells;std::string detail;
  if(!ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),detail)||
     !ds.readProjectCoordinates(coordinates,detail)||!ds.listWellInfos(wells,detail)){
    if(error)*error=QString::fromStdString(detail);return false;
  }
  result.crs.createFromUserInput(QString::fromStdString(coordinates.coordinateSystem));
  if(!result.crs.isValid()||coordinates.coordinateUnit!="m"){
    if(error)*error=QObject::tr("单因素需要 DataService 已声明的工区米制坐标系");return false;
  }
  QHash<qlonglong,paleo::data::WellInfo> byId;for(const auto &well:wells)byId.insert(well.id,well.value);
  for(const auto &entity:catalog->entities("well")){
    const auto id=entity.extra.value("areaModelWellId").toLongLong();if(id<=0)continue;
    QgsGeometry geometry;const auto well=byId.value(id);
    const auto definition=well.coordinateSystem.empty()?coordinates.coordinateSystem:well.coordinateSystem;
    const auto unit=well.coordinateUnit.empty()?coordinates.coordinateUnit:well.coordinateUnit;
    QgsCoordinateReferenceSystem source;source.createFromUserInput(QString::fromStdString(definition));
    if(source.isValid()&&unit=="m"&&std::isfinite(well.wellheadX)&&std::isfinite(well.wellheadY)){
      geometry=QgsGeometry::fromPointXY(QgsPointXY(well.wellheadX,well.wellheadY));
      geometry=result.resolve({},geometry,source);
    }
    result.native.insert(entity.id,geometry);
  }
  return true;
}
