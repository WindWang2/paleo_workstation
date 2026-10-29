// 层：数据
#pragma once
#include "datacatalog.h"

// Project the work-area model's sources into the separate artifact index without
// reopening, hashing or parsing the original files.
bool registerAreaSources(DataCatalog &catalog, QString *error);
bool applyAreaPointMapping(const QString &project, const QString &sourcePath,
                           const QVariantMap &mapping, QString *error);
