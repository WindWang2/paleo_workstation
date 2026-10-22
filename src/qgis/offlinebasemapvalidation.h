// 层：QGIS 封装
#pragma once
#include <QString>
namespace PaleoOfflineRaster {
// Owns and destroys its provider in the calling thread; safe for background preparation.
QString validate(const QString &path);
}
