// 层：QGIS 封装
#pragma once
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
class QgsMapLayer;
class QgsVectorLayer;
namespace MappingArtifactWriter {
void restoreRasterCrs(QgsMapLayer *layer);
bool composeRaster(const QString &path, const QList<QgsMapLayer *> &layers,
                   const QVariantList &facies, const QList<double> &thresholds,
                   const QSet<QString> &continuous, QString *error);
bool raster(const QString &path, const QVector<int> &cells, int cols, int rows,
            const QRectF &extent, QString *error);
bool points(const QString &path, const QVariantList &points, QString *error);
bool vectorSnapshot(QgsVectorLayer *layer, const QString &path, QString *error);
QVariantList constraintGeometries(const QString &path, QString *error);
bool syncFaciesAttributes(QgsVectorLayer *layer, const QVariantList &facies,
                          QString *error);
void applyFaciesStyle(QgsMapLayer *layer, const QVariantList &facies);
} // namespace MappingArtifactWriter
