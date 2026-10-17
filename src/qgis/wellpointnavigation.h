// 层：QGIS 封装
#pragma once
#include <QString>
#include <QList>
#include <functional>
class QgsMapCanvas;
class QgsVectorLayer;
class QgsMapTool;
namespace WellPointNavigation {
void install(QgsMapCanvas *canvas, QgsVectorLayer *wells, const QList<QgsMapTool *> &tools,
             const std::function<void(const QString &)> &openWell);
}
