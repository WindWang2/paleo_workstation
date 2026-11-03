// 层：数据
#pragma once
#include "domain/stratigraphicunit.h"
namespace paleo::data {struct SeismicHorizonWindowResult;}
#include <QRectF>
#include <QVector>
#include <functional>

namespace paleo::strat {
struct SeismicUnitInput {
  UnitSelection unit;
  QVector<float> samples; // native layer-window RMS; NaN is outside coverage
  QRectF extent;
  QString crs,sourcePath,error;
  int rows=64,columns=64,validCells=0;
};
bool readSeismicAnalysisWindow(const QString &project,const UnitSelection &unit,const QString &source,
    paleo::data::SeismicHorizonWindowResult &out,QString *error=nullptr,
    const std::function<bool()> &cancelled={});
SeismicUnitInput prepareSeismicUnitInput(const QString &project,const UnitSelection &unit,const QString &source,
    const std::function<bool()> &cancelled={});
}
