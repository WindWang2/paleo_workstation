// 层：功能
#pragma once
#include "domain/stratigraphicunit.h"
#include <QVariantList>
class DataCatalog;

// Newest-first immutable well predictions in the same frozen stratigraphic unit.
QVariantList mockSeismicCalibrationSources(DataCatalog *catalog,const QString &project,
    const QString &horizon,const paleo::strat::UnitSelection &unit);
