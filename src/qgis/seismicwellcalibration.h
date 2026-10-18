// 层：QGIS 封装
#pragma once
#include "../services/seismicclusterlabels.h"
class QgsCoordinateTransformContext;

// Sources are immutable catalog snapshots in newest-first order. Read on the
// task thread; never borrow live project layers or unsaved editing buffers.
QVector<SeismicClusterWell> readSeismicCalibrationWells(
    const QVariantList &sources, const QVariantList &schema,
    const QString &gridCrsWkt, const QgsCoordinateTransformContext &context,
    QStringList *usedVersions, QString *error,
    const std::function<bool()> &cancel = {});
