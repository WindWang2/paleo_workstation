// 层：数据
#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

#include <vector>

namespace paleo::wellsiting
{
struct HoleRegion;

// Mutable, project-local map projections. The catalog and diagnosis own the inputs.
bool writePlannedWellsLayer(const QString &path, const QList<QVariantMap> &wells, QString *error);
bool writeCoverageHolesLayer(const QString &path, const std::vector<HoleRegion> &regions, QString *error);
}
