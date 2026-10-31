// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
namespace paleo::io {
// SEG-Y 3200 字节文本头解码：自动识别 EBCDIC（cp037）或 ASCII。
QString segyTextHeaderToString(const QByteArray &text);
bool isMetricProjectCrs(const QString &crs);
QString coordinateWkt(const QString &crs);
bool projectRasterFile(const QString &path, const QString &target, QString *error, const QString &source = {});
bool projectSegyFile(const QString &source, const QString &output, const QString &from,
                     const QString &target, QString *error);
}
