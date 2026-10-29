// 层：数据
#pragma once
#include "datacatalog.h"

// Catalog IDs/links remain stable; the work-area model supplies well facts.
CatalogEntity projectAreaWell(const QString &project, const CatalogEntity &stored);
void projectAreaWells(const QString &project, QVector<CatalogEntity> &stored);
bool registerAreaWells(DataCatalog &catalog, QString *error);
