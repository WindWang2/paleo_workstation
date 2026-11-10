// 层：QGIS 封装
#pragma once

#include <QString>

struct GeoAffineParams;

namespace ProvisionalVector {
// source is an OGR URI. destination is a new, unpublished GeoPackage file.
bool write(const QString &source, const QString &destination,
           const GeoAffineParams &params, int *featureCount, QString *error);
}
