// 层：QGIS 封装
#pragma once
#include <QString>
namespace paleo::strat {struct SeismicUnitInput;}
namespace UnitRaster {
bool writeInput(const paleo::strat::SeismicUnitInput &input,const QString &path,QString *error);
bool maskOutput(const paleo::strat::SeismicUnitInput &input,const QString &path,QString *error);
bool maskOutputLikeInput(const QString &inputPath,const QString &outputPath,QString *error);
}
