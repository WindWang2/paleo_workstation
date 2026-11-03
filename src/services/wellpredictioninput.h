// 层：数据
#pragma once
#include "welllogset.h"
#include <functional>
namespace WellComposite { struct ComprehensiveWellData; }
namespace paleo {
bool readModelWellPredictionInput(const QString &project,WellComposite::ComprehensiveWellData &data,
    QString *error,const std::function<bool()> &cancelled={});
bool readFileWellPredictionCurves(const QVector<WellCurveRef> &curves,WellComposite::ComprehensiveWellData &data,
    QString *error,const std::function<bool()> &cancelled={});
}
