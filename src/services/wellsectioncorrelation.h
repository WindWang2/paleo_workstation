// 层：数据
#pragma once
#include "domain/wellsection.h"
#include <QHash>

namespace paleo {
// Read the current DataService seismic framework once for a section request.
// Original log tops remain intact; only the correlation boundary set changes.
void loadSeismicCorrelationTops(const QString &project, const QHash<int,qlonglong> &nativeWells,
                               QVector<wellsection::Well> &wells, QStringList &warnings);
}
