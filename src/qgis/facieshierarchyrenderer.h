// 层：QGIS 封装
#pragma once
#include <QMap>
#include <QVariantList>
#include <functional>
class QObject;
class QgsMapLayer;
class QgsVectorLayer;
namespace FaciesHierarchyRenderer {
QVariantList codes(QgsMapLayer *layer);
// Class pins stored in the layer property paleo/faciesPinnedLevels (JSON
// object, see FaciesHierarchy::parsePins): present code -> pinned level.
QMap<QString, QString> pinnedCodeLevels(const QgsMapLayer *layer, const QVariantList &schema,
                                        const QVariantList &present);
QVariantList legend(QgsMapLayer *layer, const QVariantList &schema,
                    const QString &level);
QVariantList apply(QgsMapLayer *layer, const QVariantList &schema,
                   const QString &level);
void watchUndo(QgsVectorLayer *layer, QObject *context,
               const std::function<void()> &callback);
bool validateTopology(QgsVectorLayer *v, QString *error);
} // namespace FaciesHierarchyRenderer
