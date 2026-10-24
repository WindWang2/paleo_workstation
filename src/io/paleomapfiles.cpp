// 层：数据
#include "paleomapfiles.h"
#include "../catalog/datacatalog.h"
#include "../domain/sidecarvocab.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QObject>
#include <algorithm>
namespace PaleoMapFiles {
namespace {
bool memberOf(const QFileInfo &main, const QFileInfo &info) {
  if (main.absolutePath() != info.absolutePath()) return false;
  if (main.fileName().compare(info.fileName(), Qt::CaseInsensitive) == 0) return true;
  const auto &sidecars = main.suffix().compare("shp", Qt::CaseInsensitive) == 0 ? paleo::sidecars::shpFamily() : paleo::sidecars::rasterFamily();
  if (info.completeBaseName().compare(main.completeBaseName(), Qt::CaseInsensitive) == 0 && sidecars.contains(info.suffix().toLower())) return true;
  for (const auto &suffix : paleo::sidecars::nameSuffixes())
    if (info.fileName().compare(main.fileName() + '.' + suffix, Qt::CaseInsensitive) == 0) return true;
  return false;
}
}
QStringList discover(const QStringList &files, const QString &folder, const std::function<bool()> &cancelled) {
  const QStringList extensions{"shp", "gpkg", "geojson", "json", "tif", "tiff", "img", "png", "jpg", "jpeg", "pdf", "svg"};
  QStringList out; QSet<QString> seen;
  const auto add = [&](const QString &path) {
    QFileInfo info(path); const auto canonical = info.canonicalFilePath();
    if (info.isFile() && extensions.contains(info.suffix().toLower()) && !seen.contains(canonical)) {
      seen.insert(canonical); out << canonical;
    }
  };
  for (const auto &path : files) add(path);
  if (!folder.isEmpty()) {
    const auto prefix = QFileInfo(folder).canonicalFilePath() + '/';
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      if (cancelled && cancelled()) return {};
      const auto path = it.next();
      if (QFileInfo(path).canonicalFilePath().startsWith(prefix)) add(path);
    }
  }
  out.sort(); return out;
}
QStringList members(const QString &path, QString *error) {
  QFileInfo main(path); if (!main.isFile()) { if (error) *error = QObject::tr("图件文件不存在：%1").arg(path); return {}; }
  const auto extension = main.suffix().toLower();
  QStringList out{main.absoluteFilePath()};
  // B-03/A-14：sidecar 词表统一 domain/sidecarvocab.h——shp 族并入 dbf
  // 属性索引 7 项（此前导入不全）；栅格族含 world/投影/样式。
  QSet<QString> present;
  for (const auto &info : main.absoluteDir().entryInfoList(QDir::Files)) {
    if (memberOf(main, info)) {
      out << info.absoluteFilePath(); present.insert(info.suffix().toLower());
    }
  }
  if (extension == "shp" && (!present.contains("shx") || !present.contains("dbf"))) {
    if (error) *error = QObject::tr("Shapefile 配套文件不完整（须含 SHP、SHX、DBF）：%1").arg(path);
    return {};
  }
  out.removeDuplicates(); out.sort(); return out;
}
QString inferredHorizon(const QString &path, const QStringList &horizons, const QString &fallback) {
  QStringList found;
  for (const auto &h : horizons) {
    const QRegularExpression re("(?<![A-Za-z0-9])" + QRegularExpression::escape(h) + "(?![A-Za-z0-9])", QRegularExpression::CaseInsensitiveOption);
    if (re.match(QDir::fromNativeSeparators(path)).hasMatch()) found << h;
  }
  return found.size() == 1 ? found.first() : fallback;
}
QVariantList copyDataset(const QString &source, const QString &directory, const std::function<bool()> &cancelled, QString *error,
                         const QVariantMap &expectedHashes) {
  const auto files = members(source, error); if (files.isEmpty()) return {};
  for (auto it = expectedHashes.cbegin(); it != expectedHashes.cend(); ++it)
    if (memberOf(QFileInfo(source), QFileInfo(it.key())) && !files.contains(it.key())) {
      if (error) *error = QObject::tr("预览中的图件配套文件已缺失，请重新识别：%1").arg(it.key());
      return {};
    }
  QVariantList result;
  if (!QDir().mkpath(directory)) return {};
  for (const auto &path : files) {
    if (cancelled && cancelled()) { QDir(directory).removeRecursively(); return {}; }
    const auto target = QDir(directory).filePath(QFileInfo(path).fileName());
    if (!QFile::copy(path, target)) { if (error) *error = QObject::tr("无法复制图件配套文件：%1").arg(path); QDir(directory).removeRecursively(); return {}; }
    const auto hash = DataCatalog::sha256FileHex(target, error, cancelled);
    const auto original = DataCatalog::sha256FileHex(path, error, cancelled);
    if (!expectedHashes.isEmpty() && (!expectedHashes.contains(path) || original != expectedHashes.value(path).toString())) {
      if (error) *error = QObject::tr("图件配套文件在预览后发生变化，请重新识别：%1").arg(path);
      QDir(directory).removeRecursively(); return {};
    }
    if (hash.isEmpty() || hash != original) { if (error) *error = QObject::tr("图件在导入期间发生变化：%1").arg(path); QDir(directory).removeRecursively(); return {}; }
    result << QVariantMap{{"file", QFileInfo(path).fileName()}, {"sha256", hash}, {"bytes", QFileInfo(target).size()}};
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
  }
  return result;
}
QString fingerprint(const QVariantList &members) {
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument::fromVariant(members).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
}
