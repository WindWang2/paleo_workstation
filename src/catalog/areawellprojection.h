// 层：数据
#pragma once
#include "datacatalog.h"

// Catalog IDs/links remain stable; the work-area model supplies well facts.
CatalogEntity overlayAreaWell(CatalogEntity stored, const CatalogEntity &model);
CatalogEntity projectAreaWell(const QString &project, const CatalogEntity &stored);
void projectAreaWells(const QString &project, QVector<CatalogEntity> &stored);
QStringList matchingAreaWellIds(const QString &project, const QString &normalizedName,
                                const CatalogIndex &index, const QVector<CatalogEntity> &entities);
bool cachedAreaWellRows(const QString &project, QVector<CatalogEntity> &rows);
bool registerAreaWells(DataCatalog &catalog, QString *error);
