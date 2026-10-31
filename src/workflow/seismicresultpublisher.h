// 层：功能
#pragma once
#include "domain/stratigraphicunit.h"
#include <QVariantMap>
struct SeismicPublicationFeedback { bool ok = false; QString message; };
SeismicPublicationFeedback publishSeismicAggregate(const QString &projectDir,
    const paleo::strat::UnitSelection &unit, const QVariantMap &params);
