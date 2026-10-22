// 层：数据
#include "welltable.h"
#include "wellfileparsers.h"
#include "dataimportservice.h"
#include "lasparser.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace paleo::io {
bool readImportWellInput(const QString &path, const QString &type, const QString &expectedHash,
                        const QVariantMap &mapping, ValidatedWellInput *out, QString *error)
{
  const auto fail = [&](const QString &reason) { if (error) *error = reason; return false; };
  QFile source(path);
  if (!source.open(QIODevice::ReadOnly)) return fail(source.errorString());
  if (source.size() > LasParser::fileSizeLimit()) return fail(QObject::tr("井表过大：%1").arg(path));
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QString ioError;
  bool ok = false;
  if ((isWellTableContainer(path) && (type == QLatin1String("well_head") || type == QLatin1String("well_stratification"))) ||
      (type == QLatin1String("well_head") && QFileInfo(path).suffix().compare("dat", Qt::CaseInsensitive) == 0)) {
    const auto table = readWellTable(path, mapping);
    if (!table.error.isEmpty() || table.type != type) return fail(table.error.isEmpty() ? QObject::tr("井表类型与字段不匹配") : table.error);
    out->heads = table.heads; out->tops = table.tops; out->report = table.report; out->normalized = table.normalized;
    ok = hash.addData(&source);
  } else if (type == QLatin1String("well_head")) {
    const auto r = parseWellHeadDevice(&source, &hash, &out->report, &ioError);
    if (r) { out->heads = *r; ok = true; }
  } else if (type == QLatin1String("well_stratification")) {
    const auto r = parseWellTopsDevice(&source, &hash, &out->report, &ioError);
    if (r) { out->tops = *r; ok = true; }
  } else if (type == QLatin1String("time_depth")) {
    const auto r = parseTimeDepthDevice(&source, &hash, &out->report, &ioError);
    if (r) { out->td = *r; ok = true; }
  } else {
    const auto r = parseDeviationDevice(&source, &hash, &out->report, &ioError);
    if (r) { out->deviation = *r; ok = true; }
  }
  if (!ok) return fail(QObject::tr("读取井表失败 %1：%2").arg(path, ioError));
  if (QString::fromLatin1(hash.result().toHex()) != expectedHash) return fail(QObject::tr("井表读取期间内容发生变化，请重试：%1").arg(path));
  if (!out->report.acceptedRows) return fail(QObject::tr("井表没有可用数据行：%1；%2").arg(path, wellParseSummary(out->report)));
  return true;
}
bool storeNormalizedWellTable(ImportSession &s, const CatalogVersion &raw, const QByteArray &text, QString *error)
{
  if (text.isEmpty()) return true;
  if (!s.ensureStagingRoot(error)) return false;
  CatalogVersion v;
  v.id = s.cat->nextVersionId(); v.assetId = raw.assetId;
  v.stage = QStringLiteral("DERIVED"); v.versionNumber = s.cat->currentVersion(raw.assetId).versionNumber + 1; v.managed = true;
  v.fileName = QStringLiteral("well_data.dat");
  v.path = QStringLiteral("artifacts/") + DataCatalog::managedPath(QStringLiteral("derived"), v.assetId, v.id, v.fileName);
  v.sourceUri = raw.sourceUri; v.parentVersionIds = {raw.id};
  for (const auto &input : raw.extra.value("depthResolution").toMap().value("input_versions").toStringList())
    if (!v.parentVersionIds.contains(input)) v.parentVersionIds << input;
  v.sha256 = QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex());
  v.extra = raw.extra;
  v.extra.insert(QStringLiteral("generatedFrom"), raw.id);
  const QString path = DataCatalog::resolvedVersionPath(s.stagingRoot, v);
  if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) { if (error) *error = QObject::tr("无法创建井表派生目录"); return false; }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) { if (error) *error = file.errorString(); return false; }
  return s.cat->addVersion(v, error);
}
}
