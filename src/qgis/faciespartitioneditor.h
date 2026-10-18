// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVariantList>
#include <qgsgeometry.h>
class QgsVectorLayer;

// One leaf partition owns all three display levels. Operations never commit
// providers: geometry, attributes and observer audits share one native command.
namespace FaciesPartitionEditor {
bool isPartition(const QgsVectorLayer *layer);
QVariantList choices(const QgsVectorLayer *layer);
int selectionSize(const QgsVectorLayer *layer);
QString layerKey(const QgsVectorLayer *layer);
QString footprint(QgsVectorLayer *layer, QString *error = nullptr);
bool install(QgsVectorLayer *layer, const QVariantList &schema,
             const QString &frozenFootprint = {}, QString *error = nullptr);
bool validate(QgsVectorLayer *layer, QString *error = nullptr);
bool split(QgsVectorLayer *layer, const QgsGeometry &line, QString *error = nullptr);
// Paint is clipped to the existing footprint (including existing no-data holes).
bool paint(QgsVectorLayer *layer, const QgsGeometry &polygon, int targetCode,
           QString *error = nullptr);
// targetCode=0 means preserve a common microfacies; mixed classes need a target.
bool merge(QgsVectorLayer *layer, int targetCode = 0, QString *error = nullptr);
}
