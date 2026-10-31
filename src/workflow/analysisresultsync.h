// 层：功能
#pragma once
#include <QString>
#include <QStringList>
#include <functional>
class DataCatalog;
QString ensureProjectAnalysisScheme(DataCatalog *catalog, const QString &projectDir,
                                    const QStringList &boundaries);
// Rebuildable projection of immutable catalog results into Data Service.
// Returns a visible error; callers retain catalog results and retry on next change.
QString syncAnalysisResults(DataCatalog *catalog, const QString &projectDir,
                            const std::function<bool()> &cancelled = {});
