// 层：数据
#pragma once
#include <QString>
bool validNativeCrs(const QString &crs);
bool sameNativeCrs(const QString &first, const QString &second);
QString metricNativeCrs(const QString &source, double x, double y);
bool nativeToGeographic(const QString &crs, double x, double y, double *lon, double *lat);
