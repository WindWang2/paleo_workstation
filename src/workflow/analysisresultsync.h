// 层：功能
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
#include "domain/stratigraphicunit.h"
class DataCatalog;
struct CatalogVersion;
// Read the frozen identity, including an unambiguous immutable parent chain.
paleo::strat::UnitSelection analysisResultUnitSelection(DataCatalog *catalog,const CatalogVersion &version);
QString ensureProjectAnalysisScheme(DataCatalog *catalog, const QString &projectDir,
                                    const QStringList &boundaries);
// Rebuildable projection of immutable catalog results into Data Service.
// Returns a visible error; callers retain catalog results and retry on next change.
QString syncAnalysisResults(DataCatalog *catalog, const QString &projectDir,
                            const std::function<bool()> &cancelled = {});
