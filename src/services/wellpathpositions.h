// 层：数据
#pragma once
#include "projectdata.h"
#include <paleo_data/model.h>

// Positions belong to the selected time-depth relation, never a different well
// or a silently selected second table. Missing Z conventions remain missing TVD.
void attachTimeDepthPositions(const paleo::data::TimeDepthTable &table,
                             const QString &projectCrs, QVector<TdSample> &rows);
