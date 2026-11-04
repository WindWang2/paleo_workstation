// 层：数据
#pragma once
#include "areamodel.h"
#include "horizonbinner.h"

// Read stored model points, without reparsing or requiring the external file.
paleo::area_model::Read readModelHorizonPreview(const QString &source, BinnedHorizon &grid,
    QString *error, const std::function<bool()> &cancelled = {});
