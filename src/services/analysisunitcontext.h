// 层：数据
#pragma once
#include "domain/stratigraphicunit.h"
#include <paleo_data/stratigraphic_framework.h>
#include <QJsonArray>
namespace WellComposite { struct ComprehensiveWellData; }

namespace paleo::strat {
QString analysisProjectCrs(const QString &project,QString *error=nullptr);
// Resolve immutable identity once, before a task is scheduled. No implicit scheme creation.
bool freezeAnalysisUnit(const QString &project,const UnitSelection &requested,UnitSelection &frozen,
                        QString *error=nullptr,bool allowAggregate=false);
bool analysisUnitGeometryCurrent(const QString &project,const UnitSelection &unit,QString *error=nullptr);
// Native DataService well identity wins. Legacy keys apply only when no native occurrence exists.
paleo::data::WellSubIntervalRange analysisWellRange(const paleo::data::StratigraphicScheme &scheme,
    const UnitSelection &unit,const QString &entityId,qlonglong modelWellId=0);
QJsonArray scopeWellRows(const QJsonArray &rows,const paleo::data::WellSubIntervalRange &range);
WellComposite::ComprehensiveWellData scopeWellData(const WellComposite::ComprehensiveWellData &data,
    const paleo::data::WellSubIntervalRange &range);
}
