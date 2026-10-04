// 层：数据
#pragma once
#include "../catalog/datacatalog.h"
#include <QDateTime>
#include <QHash>
#include <functional>

namespace paleo::storage {
// Owner thread copies catalog values; worker performs every filesystem query.
struct Snapshot {
  QString projectDir;
  QString catalogPath;
  quint64 mutationSeq = 0;
  QVector<CatalogAsset> assets;
  QVector<CatalogEntity> entities;
  QVector<CatalogVersion> versions;
  QVector<EntityAssetLink> links;
};
Snapshot snapshot(DataCatalog *catalog);
struct FileFact {
  QString relativePath;
  QString canonicalPath;
  qint64 sizeBytes = -1;
  QDateTime modified;
};
struct StaleVersion {
  CatalogVersion version;
  QString assetName;
  QString reason;
  FileFact file;
};
struct Report {
  Snapshot source;
  QHash<QString, qint64> bytesByEntity, bytesByType;
  QHash<QString, FileFact> versionFiles;
  QHash<QString, QString> canonicalReferences; // Worker-normalized, including unsafe aliases.
  QVector<FileFact> orphans;
  QVector<StaleVersion> stale;
  QStringList scannedRoots, uncovered;
  qint64 versionBytes = 0;
  int unknownSizes = 0;
  bool complete = false;
};
using Progress = std::function<bool(int, int, const QString &)>;
// Logical storage accounting: each version counted once per type and once per
// distinct linked entity. Shared files/links are explicitly not physical usage.
Report scan(const Snapshot &source, const Progress &progress = {});
struct Preview {
  Snapshot source;
  QStringList versionIds;
  QVector<FileFact> orphanFiles;
  QVector<FileFact> files;
  QStringList blocked;
  qint64 bytes = 0;
  int affectedVersions = 0;
  bool valid = false;
};
Preview preview(const Report &report, const QStringList &versionIds,
                const QStringList &orphanPaths);
// Revalidate file identity and all references in worker before owner commit.
bool validate(const Preview &preview, const Progress &progress, QString *error);
} // namespace paleo::storage
Q_DECLARE_METATYPE(paleo::storage::Report)
Q_DECLARE_METATYPE(paleo::storage::Preview)
