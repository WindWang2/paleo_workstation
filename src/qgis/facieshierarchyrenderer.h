// 层：QGIS 封装
#pragma once
#include <QMap>
#include <QStringList>
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
// Typed expression literals (QgsExpression::quotedValue) of the present codes
// pinned to level, de-duplicated in first-seen order. Integer codes stay
// unquoted, text codes are quoted/escaped, so "facies_code" IN (...) matches
// either field type without to_string formatting.
QStringList pinnedCodeLiterals(const QMap<QString, QString> &pinned, const QVariantList &present,
                               const QString &level);
QVariantList legend(QgsMapLayer *layer, const QVariantList &schema,
                    const QString &level);
QVariantList apply(QgsMapLayer *layer, const QVariantList &schema,
                   const QString &level);
void watchUndo(QgsVectorLayer *layer, QObject *context,
               const std::function<void()> &callback);
bool validateTopology(QgsVectorLayer *v, QString *error);
} // namespace FaciesHierarchyRenderer
