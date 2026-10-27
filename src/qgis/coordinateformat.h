// 层：QGIS 封装
#pragma once

#include <QPair>
#include <QString>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgspointxy.h>

class QgsProject;

namespace paleo::coordinates
{
QgsCoordinateReferenceSystem engineeringGrid();
inline QgsPointXY point(double x = 0.0, double y = 0.0) { return {x, y}; }

enum class OrdinateFormat
{
  Decimal,
  DegreesMinutes,
  DegreesMinutesSeconds
};

QString formatOrdinate(double value, bool easting, const QgsCoordinateReferenceSystem &crs,
                       OrdinateFormat format, int precision);
QString formatPoint(const QgsPointXY &point, const QgsCoordinateReferenceSystem &crs,
                    OrdinateFormat format = OrdinateFormat::Decimal, int precision = 6);
bool parsePoint(const QString &x, const QString &y, const QgsCoordinateReferenceSystem &crs,
                OrdinateFormat format, QgsPointXY *point, QString *error);
bool transformPoint(const QgsPointXY &point, const QgsCoordinateReferenceSystem &source,
                    const QgsCoordinateReferenceSystem &destination,
                    const QgsCoordinateTransformContext &context,
                    QgsPointXY *result, QString *error);
QPair<QString, QString> mapReadoutParts(const QgsPointXY &point,
                                         const QgsCoordinateReferenceSystem &mapCrs,
                                         const QgsCoordinateTransformContext &context,
                                         QgsProject *project = nullptr);
QString mapReadout(const QgsPointXY &point, const QgsCoordinateReferenceSystem &mapCrs,
                   const QgsCoordinateTransformContext &context, QgsProject *project = nullptr);
}
