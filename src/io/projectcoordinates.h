// 层：数据
#pragma once
#include <QString>
namespace paleo::io {
bool isMetricProjectCrs(const QString &crs);
QString coordinateWkt(const QString &crs);
bool projectRasterFile(const QString &path, const QString &target, QString *error, const QString &source = {});
bool projectSegyFile(const QString &source, const QString &output, const QString &from,
                     const QString &target, QString *error);
}
