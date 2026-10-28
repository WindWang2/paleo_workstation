// 层：数据
#include "areamodel.h"
#include "../catalog/datacatalog.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace paleo::area_model {
namespace {
QMutex mutex;
QHash<const void *, QString> projects;
QHash<const void *, QHash<QString, QString>> aliases;
QString key(const QString &path) {
  const QFileInfo info(path);
  auto result = QDir::cleanPath(info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath());
#ifdef Q_OS_WIN
  result = result.toCaseFolded();
#endif
  return result;
}
}
QString directory(const QString &project) { return QDir(project).filePath("artifacts/metadata/area-model"); }
bool ensureProject(const QString &project, QString *error) {
  paleo::data::DataService service; std::string detail;
  const auto root = directory(project).toUtf8().toStdString();
  const bool exists = QFileInfo::exists(directory(project) + "/data.sqlite");
  const bool ok = exists ? service.openProject(root, detail)
      : service.createProject(root, QFileInfo(project).fileName().toUtf8().toStdString(), detail);
  if (error) *error = QString::fromUtf8(detail);
  return ok;
}
bool importDirectory(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled, const QStringList &excluded,
                     const std::function<void(int, int, const QString &)> &progress) {
  paleo::data::DataService service;
  std::string detail;
  const auto root = directory(project).toUtf8().toStdString();
  const bool exists = QFileInfo::exists(directory(project) + "/data.sqlite");
  if (!(exists ? service.openProject(root, detail)
               : service.createProject(root, QFileInfo(project).fileName().toUtf8().toStdString(), detail))) {
    if (error) *error = QString::fromUtf8(detail);
    return false;
  }
  paleo::data::ImportOptions options;
  options.copySources = false;
  options.parsing.comprehensiveWellMetreDepth = true;
  for (const auto &path : excluded) options.excludedDirectories.push_back(path.toUtf8().toStdString());
  const bool ok = service.importDirectory(source.toUtf8().toStdString(), options, report, detail, [&progress](const paleo::data::ImportProgress &p) {
    if (progress) progress(p.completed, p.total, QString::fromUtf8(p.status) + ": " + QString::fromUtf8(p.file));
  }, cancelled);
  if (error) *error = QString::fromUtf8(detail);
  return !report.cancelled && (ok || report.failed > 0);
}
void registerProject(const QString &project, const void *owner) {
  QMutexLocker lock(&mutex);
  projects.insert(owner, directory(project));
}
void registerAliases(const DataCatalog &catalog, const void *owner) {
  QHash<QString, QString> entries;
  for (const auto &version : catalog.versions())
    if (version.stage == "RAW" && catalog.currentVersion(version.assetId).id == version.id && !version.sourceUri.isEmpty())
      entries.insert(key(catalog.versionFilePath(version)), key(version.sourceUri));
  QMutexLocker lock(&mutex); aliases.insert(owner, entries);
}
void unregisterProject(const void *owner) { QMutexLocker lock(&mutex); projects.remove(owner); aliases.remove(owner); }
Read openSource(const QString &path, paleo::data::DataService &service,
                long long &sourceId, QString *error) {
  QList<QString> roots;
  { QMutexLocker lock(&mutex); roots = projects.values(); }
  roots.removeDuplicates();
  QString selected;
  QString wanted = key(path);
  { QMutexLocker lock(&mutex);
    QString resolved;
    for (const auto &entries : aliases) if (entries.contains(wanted)) {
      if (!resolved.isEmpty() && resolved != entries.value(wanted)) {
        if (error) *error = QStringLiteral("ambiguous area source alias");
        return Read::Failed;
      }
      resolved = entries.value(wanted);
    }
    if (!resolved.isEmpty()) wanted = resolved;
  }
  std::string detail;
  for (const auto &root : roots) {
    if (!QFileInfo::exists(root + "/data.sqlite")) continue;
    paleo::data::DataService candidate;
    if (!candidate.openProjectReadOnly(root.toUtf8().toStdString(), detail)) {
      if (error) *error = QString::fromUtf8(detail);
      return Read::Failed;
    }
    std::vector<paleo::data::SourceEntry> sources;
    if (!candidate.listSources(sources, detail)) { if (error) *error = QString::fromUtf8(detail); return Read::Failed; }
    for (const auto &source : sources) {
      if (key(QString::fromUtf8(source.path)) != wanted) continue;
      if (!selected.isEmpty() && selected != root) {
        if (error) *error = QStringLiteral("source belongs to multiple open area models");
        return Read::Failed;
      }
      selected = root; sourceId = source.id; break;
    }
  }
  if (selected.isEmpty()) return Read::Unbound;
  if (!service.openProjectReadOnly(selected.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail);
    return Read::Failed;
  }
  return Read::Ready;
}
QString cacheToken(const QString &path) {
  paleo::data::DataService service; long long source = 0; QString error;
  const auto state = openSource(path, service, source, &error);
  if (state == Read::Unbound) return {};
  if (state == Read::Failed) return "|area-error:" + error;
  QString result = "|area:" + QString::fromUtf8(service.projectPath()) + ":" + QString::number(source);
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"}) {
    const QFileInfo file(QString::fromUtf8(service.projectPath()) + suffix);
    result += QString(":%1:%2:%3").arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch())
        .arg(file.metadataChangeTime().toMSecsSinceEpoch());
  }
  return result;
}

}
