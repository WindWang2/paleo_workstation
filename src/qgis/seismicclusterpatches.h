// 层：QGIS 封装
#pragma once
#include <QVariantList>
#include <functional>

namespace SeismicClusterPatches {
// Every feature represents a four-connected component of the original, unfiltered grid.
bool polygonize(const QString &rasterPath, const QString &outputPath, const QString &crsWkt,
                QString *error, const std::function<bool()> &cancel = {},
                const std::function<void(double)> &progress = {});
QVariantList choices(const QString &sourcePath, QString *error, qint64 firstFid = 0, int limit = 129);
qint64 clusterSelection(const QString &sourcePath, int cluster, QList<qint64> &highlight, QString *error);
bool locateWells(const QString &sourcePath, QVariantList &supportingWells, QString *error,
                 const std::function<bool()> &cancel = {});
bool writeWellAssignment(const QString &sourcePath, const QString &outputPath,
                        const QVariantList &facies, const QVariantList &clusterMapping,
                        const QVariantList &supportingWells, QString *error,
                        const std::function<bool()> &cancel = {});
bool writeManualAssignments(const QString &sourcePath, const QString &outputPath,
                            const QVariantList &facies, const QVariantList &assignments,
                            int *changedCount, QString *error);
bool writeManualAssignment(const QString &sourcePath, const QString &outputPath,
                           const QVariantList &facies, qint64 patchId, bool wholeCluster,
                           int faciesCode, int *changedCount, QString *error, qint64 featureId = -1);
}
