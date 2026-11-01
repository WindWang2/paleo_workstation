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
class QgsVectorLayer;
class QColor;
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
// 工程已确认的测区 CRS：paleo/georeference 声明的投影 CRS；地理 CRS 或
// 缺声明时回落 EPSG:32650（与导入侧「无声明即 32650」同口径——缺坐标系
// 不得阻断来源功能）。测区全景画布、井位层、工区范围共用它。
QgsCoordinateReferenceSystem confirmedSurveyCrs(QgsProject *project);
QgsVectorLayer *areaRangeLayer(const QgsRectangle &extent,const QgsCoordinateReferenceSystem &crs,
                              const QString &name,const QColor &stroke,QObject *owner);
}
