// 层：数据
#include "projectcopy.h"
#include "ziparchive.h"
#include "metadata/layermanifest.h"
#include "metadata/metastore.h"
#include "metadata/paleoprojectfile.h"
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QUrl>

namespace paleo::projectcopy {
namespace {
struct CopyPaths {
  QDir original, staged, target;
  QString qgisFolder;
  QString reference(const QString &value) const
  {
    if (value.isEmpty()) return value;
    const qsizetype suffix = value.indexOf('|');
    const QString path = QDir::fromNativeSeparators(value.left(suffix));
    const QUrl url(path);
    const bool uri = url.isLocalFile();
    const QString name = uri ? url.toLocalFile() : path;
    const bool absolute = QDir::isAbsolutePath(name);
    const QDir context(original.filePath(qgisFolder));
    if (!absolute && !name.startsWith('.') && !QFileInfo::exists(context.filePath(name))) return value;
    const QString source = QDir::cleanPath(context.absoluteFilePath(name));
    const QString relative = original.relativeFilePath(source);
    const bool copied = !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
                        !relative.startsWith(QStringLiteral("../")) && QFileInfo::exists(staged.filePath(relative));
    if ((copied && !absolute) || (!copied && absolute)) return value;
    QString mapped = copied ? target.filePath(relative) : source;
    if (uri) {
      QUrl relocated = QUrl::fromLocalFile(mapped);
      relocated.setQuery(url.query(QUrl::FullyEncoded), QUrl::StrictMode);
      relocated.setFragment(url.fragment(QUrl::FullyEncoded), QUrl::StrictMode);
      mapped = relocated.toString(QUrl::FullyEncoded);
    }
    return mapped + (suffix < 0 ? QString() : value.mid(suffix));
  }
};

bool relocateDocument(QByteArray &xml, const CopyPaths &paths, QString &error, bool &changed)
{
  QDomDocument document;
  if (!document.setContent(xml)) { error = QObject::tr("工程中的 QGIS XML 无法读取。"); return false; }
  const auto sources = document.elementsByTagName(QStringLiteral("datasource"));
  for (int i = 0; i < sources.size(); ++i) {
    auto node = sources.at(i).toElement();
    const QString before = node.text(), after = paths.reference(before);
    if (before != after) { node.firstChild().setNodeValue(after); changed = true; }
  }
  const auto tree = document.elementsByTagName(QStringLiteral("layer-tree-layer"));
  for (int i = 0; i < tree.size(); ++i) {
    auto node = tree.at(i).toElement();
    const auto before = node.attribute("source"), after = paths.reference(before);
    if (before != after) { node.setAttribute("source", after); changed = true; }
  }
  auto projection = document.documentElement().firstChildElement("properties").firstChildElement("paleo").firstChildElement("manifest");
  const auto input = QJsonDocument::fromJson(projection.text().toUtf8());
  if (input.isArray()) {
    auto rows = input.array();
    for (qsizetype i = 0; i < rows.size(); ++i) {
      auto row = rows.at(i).toObject();
      const auto before = row.value("source").toString(), after = paths.reference(before);
      if (before != after) { row.insert("source", after); rows.replace(i, row); }
    }
    if (rows != input.array()) {
      projection.firstChild().setNodeValue(QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));
      changed = true;
    }
  }
  if (changed) xml = document.toByteArray();
  return true;
}

bool relocateQgisFile(const QString &path, const CopyPaths &paths, QString &error)
{
  bool changed = false;
  if (path.endsWith(QStringLiteral(".qgs"), Qt::CaseInsensitive)) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    QByteArray xml = file.readAll(); file.close();
    if (!relocateDocument(xml, paths, error, changed)) return false;
    if (!changed) return true;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(xml) != xml.size() || !file.flush()) {
      error = file.errorString(); return false;
    }
    return true;
  }
  using namespace paleo::io;
  const auto archive = zipReadArchive(path, &error);
  if (archive.isEmpty()) return false;
  const auto entries = zipListBytes(archive);
  if (!entries.ok) { error = entries.error; return false; }
  QVector<ZipWriteEntry> output;
  bool found = false;
  for (const auto &entry : entries.entries) {
    ZipWriteEntry member{entry.name, {}};
    if (!zipExtractEntry(archive, entry, &member.data, &error)) return false;
    if (member.name.endsWith(QStringLiteral(".qgs"), Qt::CaseInsensitive)) {
      if (found) { error = QObject::tr("工程包有多个 QGIS 工程文件，无法确定入口。"); return false; }
      found = true;
      if (!relocateDocument(member.data, paths, error, changed)) return false;
    }
    output.append(std::move(member));
  }
  if (!found) { error = QObject::tr("工程包中缺少 QGIS 工程文件。"); return false; }
  return !changed || zipWriteArchive(path, output, {ZipCompression::Deflate}, &error);
}
}

bool inspectStoredBindings(const QString &copy, const QString &original, const QString &stage,
                           QStringList &review, QString &error)
{
  const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto close = qScopeGuard([&]{ QSqlDatabase::removeDatabase(connection); });
  auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
  db.setDatabaseName(copy);
  db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
  if (!db.open()) { error = db.lastError().text(); return false; }
  const auto tables = db.tables();
  const bool catalog = QFileInfo(copy).fileName() == QStringLiteral("catalog.sqlite");
  const QString table = catalog ? QStringLiteral("versions") : QStringLiteral("sources");
  if (!tables.contains(table)) return true; // Another managed dataset may also be named data.sqlite.
  QSqlQuery rows(db);
  const QString sql = catalog ? QStringLiteral("SELECT id, path FROM versions") : QStringLiteral("SELECT id, stored_path FROM sources");
  if (!rows.exec(sql)) { error = rows.lastError().text(); return false; }
  while (rows.next()) {
    const QString path = QDir::fromNativeSeparators(rows.value(1).toString());
    if (!QDir::isAbsolutePath(path)) continue;
    const auto relative = QDir(original).relativeFilePath(path);
    if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative) || !QFileInfo::exists(QDir(stage).filePath(relative))) continue;
    // Rebinding an SDK source requires its fingerprint and a versioned command.
    // Do not silently rewrite provenance in a verified catalog/area-model snapshot.
    if (review.size() < 20)
      review.append(QStringLiteral("%1 [%2] %3").arg(QDir(stage).relativeFilePath(copy), rows.value(0).toString(), path));
  }
  return true;
}

bool relocateCopiedLayers(const PaleoProjectFile &file, const QString &origin, const QString &stage,
                          const QString &target, QString &error)
{
  const CopyPaths paths{QDir(origin), QDir(stage), QDir(target), QFileInfo(file.qgz).path()};
  if (!relocateQgisFile(paths.staged.filePath(file.qgz), paths, error)) return false;
  const QString standard = file.qgz + QStringLiteral(".project.sqlite");
  const QString metadata = paths.staged.filePath(file.manifest.isEmpty() ? standard : file.manifest);
  if (!QFileInfo::exists(metadata)) return true;
  const auto close = qScopeGuard([&]{ MetaStore::closeConnectionsFor(metadata); });
  LayerManifest layers(metadata);
  // This unpublished private stage is exclusively owned by this worker.
  layers.setWriteQueueEnforced(false);
  QVector<LayerDeclaration> rows;
  if (!layers.readAll(&rows, &error)) return false;
  for (auto row : rows) {
    const QString source = paths.reference(row.source);
    if (source == row.source) continue;
    row.source = source;
    if (!layers.upsert(row, &error)) return false;
  }
  return true;
}
}
