// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVariantList>
#include <qgsgeometry.h>
class QgsVectorLayer;

// One leaf partition owns all three display levels. Operations never commit
// providers: geometry, attributes and observer audits share one native command.
// Parameter names mirror the definitions (readability-inconsistent-declaration-parameter-name).
namespace FaciesPartitionEditor {
bool isPartition(const QgsVectorLayer *v);
QVariantList choices(const QgsVectorLayer *v);
int selectionSize(const QgsVectorLayer *v);
QString layerKey(const QgsVectorLayer *v);
QString footprint(QgsVectorLayer *v, QString *error = nullptr);
bool install(QgsVectorLayer *v, const QVariantList &source,
             const QString &frozenFootprint = {}, QString *error = nullptr);
bool validate(QgsVectorLayer *v, QString *error = nullptr);
bool split(QgsVectorLayer *v, const QgsGeometry &line, QString *error = nullptr);
// Paint is clipped to the existing footprint (including existing no-data holes).
bool paint(QgsVectorLayer *v, const QgsGeometry &polygon, int code,
           QString *error = nullptr);
// code=0 means preserve a common microfacies; mixed classes need a target.
bool merge(QgsVectorLayer *v, int code = 0, QString *error = nullptr);
}
