// 层：QGIS 封装
#pragma once
#include <QVariantList>
#include <functional>
class QObject;
class QgsMapLayer;
class QgsVectorLayer;
namespace FaciesHierarchyRenderer {
QVariantList codes(QgsMapLayer *layer);
QVariantList legend(QgsMapLayer *layer, const QVariantList &schema,
                    const QString &level);
QVariantList apply(QgsMapLayer *layer, const QVariantList &schema,
                   const QString &level);
void watchUndo(QgsVectorLayer *layer, QObject *context,
               const std::function<void()> &callback);
bool validateTopology(QgsVectorLayer *v, QString *error);
} // namespace FaciesHierarchyRenderer
