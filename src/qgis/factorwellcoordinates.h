// 层：QGIS 封装
#pragma once
#include <QHash>
#include <QString>
#include <qgscoordinatereferencesystem.h>
#include <qgsgeometry.h>
class DataCatalog;
// Native well identity takes precedence over historical map coordinates. A bound
// well without coordinates remains unlocated; map-only points require a real CRS operation.
struct FactorWellCoordinates {
  QgsCoordinateReferenceSystem crs;
  QHash<QString,QgsGeometry> native;
  QgsGeometry resolve(const QString &id,const QgsGeometry &geometry,
                      const QgsCoordinateReferenceSystem &source) const;
};
bool readFactorWellCoordinates(const DataCatalog *catalog,const QString &project,
                              FactorWellCoordinates &result,QString *error);
