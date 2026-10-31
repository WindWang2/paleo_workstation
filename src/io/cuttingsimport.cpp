// 层：数据
#include "domain/shaequals.h"
#include "cuttingsimport.h"
#include "cuttingsdoc.h"
#include "dataimportservice.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace paleo::io {
bool storeImportedCuttings(ImportSession &s, const CatalogVersion &raw,
                           const QString &path, QString *error) {
  const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
  if (!paleo::domain::shaEquals(DataCatalog::sha256FileHex(path), raw.sha256))
    return fail(QObject::tr("岩屑录井表内容已变化，请重新预览：%1").arg(path));
  const auto table = readCuttingsFile(path);
  if (!table.ok || table.intervals.isEmpty())
    return fail(QObject::tr("岩屑录井表没有可用井段：%1；%2").arg(path, table.error));
  if (!paleo::domain::shaEquals(DataCatalog::sha256FileHex(path), raw.sha256))
    return fail(QObject::tr("岩屑录井表读取期间发生变化，请重试：%1").arg(path));
  QByteArray bytes = QStringLiteral("顶深,底深,岩性,描述,数据来源\n").toUtf8();
  const auto quote = [](QString value) { value.replace('"', "\"\""); return '"' + value + '"'; };
  for (const auto &row : table.intervals)
    bytes += (QString::number(row.topMd, 'g', 17) + ',' + QString::number(row.baseMd, 'g', 17) + ',' +
              quote(row.litho) + ',' + quote(row.description) + ',' + quote(row.source) + '\n').toUtf8();
  if (!s.ensureStagingRoot(error)) return false;
  CatalogVersion v; v.id = s.cat->nextVersionId(); v.assetId = raw.assetId;
  v.stage = "DERIVED"; v.versionNumber = s.cat->currentVersion(raw.assetId).versionNumber + 1;
  v.managed = true; v.fileName = "cuttings.csv"; v.sourceUri = raw.sourceUri; v.parentVersionIds = {raw.id};
  v.path = "artifacts/" + DataCatalog::managedPath("derived", v.assetId, v.id, v.fileName);
  v.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  v.extra = raw.extra;
  QStringList sources;
  for (const auto &row : table.intervals) {
    if (!row.source.isEmpty() && !sources.contains(row.source)) sources << row.source;
    if (row.source.contains(QStringLiteral("合成")) || row.source.contains(QStringLiteral("synthetic"), Qt::CaseInsensitive))
      v.extra.insert(QStringLiteral("synthetic_input"), true);
  }
  v.extra.insert(QStringLiteral("input_source_annotations"), sources);
  v.extra.insert("cuttings_import", QVariantMap{{"schema", 1}, {"source_sha256", raw.sha256},
      {"sheet", table.sheetName}, {"interval_count", table.intervals.size()}, {"issues", table.issues}, {"depth_basis", "MD"}});
  const auto destination = DataCatalog::resolvedVersionPath(s.stagingRoot, v);
  if (destination.isEmpty() || !QDir().mkpath(QFileInfo(destination).absolutePath()))
    return fail(QObject::tr("无法创建岩性表派生目录"));
  QSaveFile file(destination);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return fail(file.errorString());
  return s.cat->addVersion(v, error);
}
}
