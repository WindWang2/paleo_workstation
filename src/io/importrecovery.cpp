// 层：数据
#include "importrecovery.h"
#include "dataimportservice.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace paleo::dataimport_detail
{
namespace
{
const QString kIntent = QStringLiteral("commit-intent.json");
QString hashFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}
bool fail(QString *error, const QString &path)
{
  if (error) *error = QCoreApplication::translate("ImportRecovery",
      "导入恢复未完成，已保留文件：%1。请检查文件权限或恢复记录后重试，原有数据未覆盖。").arg(path);
  return false;
}
bool recoverOne(const QString &intent, const QString &projectDir,
                const QSet<QString> &registered, QString *error)
{
  QFile file(intent);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return fail(error, intent);
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isArray()) return fail(error, intent);
  // 先验证全份清单，再做任何清理；只删清单内、摘要吻合且未入库的受管文件。
  QStringList cleanup;
  for (const auto &value : document.array())
  {
    const auto record = value.toObject();
    CatalogVersion version;
    version.managed = true;
    version.path = record.value(QStringLiteral("path")).toString();
    const QString path = DataCatalog::resolvedVersionPath(projectDir, version);
    const QString sha = record.value(QStringLiteral("sha256")).toString();
    if (!version.path.startsWith(QLatin1String("artifacts/raw/")) &&
        !version.path.startsWith(QLatin1String("artifacts/derived/"))) return fail(error, intent);
    if (path.isEmpty() || sha.size() != 64) return fail(error, intent);
    const QString stagedSource = QFileInfo(intent).absoluteDir().absoluteFilePath(version.path);
    if (QFileInfo::exists(stagedSource)) continue; // 原子 rename 尚未成功，目标不属于此次提交。
    if (registered.contains(QDir::cleanPath(path)) || !QFileInfo::exists(path)) continue;
    if (QFileInfo(path).isSymLink() || hashFile(path) != sha) return fail(error, path);
    cleanup << path;
  }
  file.close();
  for (const auto &path : cleanup)
  {
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner);
    if (!QFile::remove(path)) return fail(error, path);
    QDir().rmdir(QFileInfo(path).absolutePath());
  }
  return QFile::remove(intent) || fail(error, intent);
}
}

bool writeImportIntent(ImportSession &session, QString *error)
{
  if (!QDir(session.stagingRoot).exists()) { session.intentPrepared = true; return true; }
  QJsonArray records;
  QDirIterator files(session.stagingRoot + QStringLiteral("/artifacts"), QDir::Files,
                     QDirIterator::Subdirectories);
  while (files.hasNext())
  {
    const QString path = files.next();
    if (path.contains(QLatin1String(".partial"))) continue;
    const QString sha = hashFile(path);
    if (sha.isEmpty()) return fail(error, path);
    records.append(QJsonObject{{QStringLiteral("path"), QDir(session.stagingRoot).relativeFilePath(path)},
                               {QStringLiteral("sha256"), sha}});
  }
  if (records.isEmpty()) { session.intentPrepared = true; return true; }
  QSaveFile out(session.stagingRoot + QLatin1Char('/') + kIntent);
  const auto bytes = QJsonDocument(records).toJson(QJsonDocument::Compact);
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
    return fail(error, out.fileName());
  session.intentPrepared = true;
  return true;
}

bool recoverImportIntents(const QString &projectDir, const DataCatalog &catalog, QString *error)
{
  QSet<QString> registered;
  for (const auto &version : catalog.versions())
    if (version.managed) registered.insert(QDir::cleanPath(DataCatalog::resolvedVersionPath(projectDir, version)));
  QDir staging(projectDir + QStringLiteral("/artifacts/staging"));
  for (const auto &entry : staging.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
  {
    if (entry.isSymLink()) continue;
    const QString intent = entry.absoluteFilePath() + QLatin1Char('/') + kIntent;
    if (QFileInfo::exists(intent) && !recoverOne(intent, projectDir, registered, error)) return false;
  }
  return true;
}
}
