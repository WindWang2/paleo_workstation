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
Plan plan(const DataCatalog &snapshot, const QString &projectDir, const DataRequest &request,
          const Cancel &cancel = {});
// Publishes one new directory, never replaces a previous batch. All I/O is worker-safe.
Result execute(const Plan &plan, const Cancel &cancel = {}, const Progress &progress = {},
               const Convert &convert = {});
}
