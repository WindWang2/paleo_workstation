// 层：QGIS 封装
#pragma once
#include "metadata/paleoprojectfile.h"
#include <qgscoordinatetransformcontext.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsrectangle.h>
#include <qgspointxy.h>

class QgsProject;
class QgsMapCanvas;
class QgsRasterLayer;
class QObject;

namespace paleo::mapreference {
// 原始图层保持 ENGCRS，由工程的显式坐标操作负责显示和反向拾取。
bool configure(QgsProject *project, const std::optional<PaleoGeoreference> &reference,
               const QString &mapCrs, QString *error = nullptr);
bool transformPoint(QgsMapCanvas *canvas, const QgsPointXY &point,
                    const QgsCoordinateReferenceSystem &source,
                    const QgsCoordinateReferenceSystem &destination,
                    QgsPointXY *result);
QgsRectangle mapExtent(QgsMapCanvas *canvas, const QgsRectangle &extent,
                       const QgsCoordinateReferenceSystem &source);
QString mbtilesUri(const QString &path);
QgsRasterLayer *offlineBasemap(const QString &path, const QString &title,
                              QObject *parent = nullptr);
QString attribution(QgsMapCanvas *canvas);
}
