// 层：数据
#pragma once
#include "../domain/exportrequest.h"
#include "../catalog/datacatalog.h"
#include <QVariantMap>
#include <functional>

namespace paleo::exporting {
struct Item { CatalogAsset asset; CatalogVersion version; QString path, category; };
struct Plan {
  DataRequest request;
  QVector<Item> items;
  QVariantList relations, entities, links;
  QStringList failures, warnings;
  qint64 bytes = 0;
};
using Cancel = std::function<bool()>;
using Progress = std::function<void(qint64, qint64, const QString &)>;
using Convert = std::function<bool(const QString &, const QString &, DataFormat, QStringList *,
                                   QVariantMap *, QString *, const Cancel &)>;
// Pure catalogue lookup, with no stat/canonicalPath/hash I/O on the UI thread.
// Multiple candidates are returned explicitly instead of choosing the latest.
QHash<QString, QStringList> versionIdsForPaths(const DataCatalog &catalog, const QStringList &paths);
// 导出暂存根（.map-products- / .paleo-map- / .horizon-export- / .paleo-batch- /
// .paleo-export-XXXXXX）此前只靠 QTemporaryDir 析构清理，进程崩溃即残留
// （#540）。导出入口启动时清扫 parentDirectory 内的超龄残留（>24h）；未超龄
// 的只 qWarning 留痕（可能正是另一次正在进行的导出，不动它）。
void sweepStaleStagingRoots(const QString &parentDirectory);
Plan plan(const DataCatalog &snapshot, const QString &projectDir, const DataRequest &request,
          const Cancel &cancel = {});
// Publishes one new directory, never replaces a previous batch. All I/O is worker-safe.
Result execute(const Plan &plan, const Cancel &cancel = {}, const Progress &progress = {},
               const Convert &convert = {});
}
