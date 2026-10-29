// 层：数据
#include "areamodel.h"
#include "../catalog/datacatalog.h"
#include <paleo_data/service.h>
#include "projectcoordinates.h"
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QDebug>
#include <algorithm>

namespace paleo::area_model {
namespace {
QMutex mutex;
QHash<const void *, QString> projects;
QHash<const void *, QHash<QString, QString>> aliases;
struct SourceIndex { QString stamp; QHash<QString, long long> ids; };
QHash<QString, SourceIndex> sourceIndexes;
QString key(const QString &path) {
  const QFileInfo info(path);
  auto result = QDir::cleanPath(info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath());
#ifdef Q_OS_WIN
  result = result.toCaseFolded();
#endif
  return result;
}
QString databaseStamp(const QString &root) {
  QString result;
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"}) {
    const QFileInfo file(root + suffix);
    result += QString(":%1:%2:%3:%4").arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch())
        .arg(file.metadataChangeTime().toMSecsSinceEpoch()).arg(file.birthTime().toMSecsSinceEpoch());
  }
  return result;
}
bool indexedSource(const QString &root, const QString &wanted, long long &id, QString *error) {
  const QString stamp = databaseStamp(root);
  { QMutexLocker lock(&mutex);
    const auto found = sourceIndexes.constFind(root);
    if (found != sourceIndexes.cend() && found->stamp == stamp) {
      id = found->ids.value(wanted); return true;
    }
  }
  paleo::data::DataService reader; std::string detail;
  std::vector<paleo::data::SourceEntry> sources;
  if (!reader.openProjectReadOnly(root.toUtf8().toStdString(), detail) || !reader.listSources(sources, detail)) {
    if (error) *error = QString::fromUtf8(detail); return false;
  }
  SourceIndex index; index.stamp = stamp;
  for (const auto &source : sources) {
    const QString path = key(QString::fromUtf8(source.path));
    if (!index.ids.contains(path)) index.ids.insert(path, source.id);
  }
  id = index.ids.value(wanted);
  if (databaseStamp(root) == stamp) { QMutexLocker lock(&mutex); sourceIndexes.insert(root, std::move(index)); }
  return true;
}
}
QString directory(const QString &project) { return QDir(project).filePath("artifacts/metadata/area-model"); }
QString confirmedProjectCrs(const QString &project) {
  if (!QFileInfo::exists(directory(project) + QStringLiteral("/data.sqlite"))) return {};
  paleo::data::DataService service; std::string detail;
  if (!service.openProject(directory(project).toUtf8().toStdString(), detail)) return {};
  paleo::data::ProjectCrsInfo info;
  if (!service.readProjectCrsInfo(info, detail) || info.hasConflict || info.coordinateUnit != "m") return {};
  const auto crs = QString::fromUtf8(info.coordinateSystem);
  return paleo::io::isMetricProjectCrs(crs) ? crs : QString();
}
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
  return importDirectoryWithCrs(project, source, report, error, cancelled, excluded, progress, {}, {});
}
bool importDirectoryWithCrs(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled, const QStringList &excluded,
                     const std::function<void(int, int, const QString &)> &progress,
                     const QString &crs, const QString &crsUnit) {
  if (progress) progress(0, 0, QStringLiteral("初始化工区数据模型"));
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
  bool configured = false;
  const int threads = qEnvironmentVariableIntValue("PALEO_IMPORT_PARSER_THREADS", &configured);
  // Parallel parse workers have been aborting the process while publishing one
  // file's result (fast-fail while freeing a parse note). Keep import serial
  // until that result can be published safely.
  options.parserThreads = std::clamp(configured ? threads : 1, 1, 1);
  const int pendingMb = qEnvironmentVariableIntValue("PALEO_IMPORT_PENDING_MB", &configured);
  options.maxPendingBytes = qint64(std::clamp(configured ? pendingMb : 256, 8, 1024)) * 1024 * 1024;
  options.parsing.comprehensiveWellMetreDepth = true;
  // A computation target is not a declaration of every input's source CRS.
  // Data_service collects explicit source declarations during parsing.
  if (!crs.isEmpty() && !service.setProjectCrs(crs.toUtf8().toStdString(), crsUnit.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail);
    return false;
  }
  for (const auto &path : excluded) options.excludedDirectories.push_back(path.toUtf8().toStdString());
  const bool ok = service.importDirectory(source.toUtf8().toStdString(), options, report, detail, [&progress](const paleo::data::ImportProgress &p) {
    if (progress) progress(p.completed, p.total, QString::fromUtf8(p.status) + ": " + QString::fromUtf8(p.file));
  }, cancelled);
  qInfo() << "area-import: files=" << report.seen << "elapsed_ms=" << report.elapsedMs
          << "hash_ms=" << report.hashMs << "parse_ms=" << report.parseMs
          << "copy_ms=" << report.copyMs << "store_ms=" << report.storeMs
          << "hashed_bytes=" << report.hashedBytes << "workers=" << report.workerCount
          << "pending_bytes=" << options.maxPendingBytes << "copy_sources=false";
  if (error) *error = QString::fromUtf8(detail);
  return !report.cancelled && (ok || report.failed > 0);
}
void registerProject(const QString &project, const void *owner) {
  QMutexLocker lock(&mutex);
  projects.insert(owner, directory(project));
  sourceIndexes.remove(directory(project));
}
void registerAliases(const DataCatalog &catalog, const void *owner) {
  QHash<QString, QString> entries;
  for (const auto &version : catalog.versions())
    if (version.stage == "RAW" && catalog.currentVersion(version.assetId).id == version.id && !version.sourceUri.isEmpty())
      entries.insert(key(catalog.versionFilePath(version)), key(version.sourceUri));
  QMutexLocker lock(&mutex); aliases.insert(owner, entries);
}
void unregisterProject(const void *owner) {
  QMutexLocker lock(&mutex); const auto root = projects.take(owner); aliases.remove(owner);
  if (!projects.values().contains(root)) sourceIndexes.remove(root);
}
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
    long long candidate = 0;
    if (!indexedSource(root, wanted, candidate, error)) return Read::Failed;
    if (candidate) {
      if (!selected.isEmpty() && selected != root) {
        if (error) *error = QStringLiteral("source belongs to multiple open area models");
        return Read::Failed;
      }
      selected = root; sourceId = candidate;
    }
  }
  if (selected.isEmpty()) return Read::Unbound;
  if (!service.openProjectReadOnly(selected.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail);
    return Read::Failed;
  }
  return Read::Ready;
}
Read surveyRange(const QString &path, paleo::data::SeismicCoordinateRange &range,
                 QString *error, const std::function<bool()> &cancelled) {
  paleo::data::DataService service; long long source = 0;
  const auto state = openSource(path, service, source, error);
  if (state != Read::Ready) return state;
  std::string detail;
  std::vector<paleo::data::DataEntry> entries;
  if (!service.listSourceData("volumes", source, entries, detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  if (entries.empty()) { if (error) *error = QStringLiteral("该来源未登记为地震体"); return Read::Failed; }
  const long long volume = entries.front().id;
  if (!service.readSeismicCoordinateRange(volume, range, detail, cancelled)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  return Read::Ready;
}
Read wellHeadRange(const QString &root, paleo::data::WellHeadRange &range, QString *error) {
  if (root.isEmpty() || !QFileInfo::exists(root + "/data.sqlite")) {
    if (error) *error = QStringLiteral("工区数据模型尚未建立"); return Read::Unbound;
  }
  paleo::data::DataService service; std::string detail;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  if (!service.readWellHeadRange(range, detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  return Read::Ready;
}
Read projectRange(const QString &root, paleo::data::ProjectPlanarRange &range, QString *error) {
  if (root.isEmpty() || !QFileInfo::exists(root + "/data.sqlite")) {
    if (error) *error = QStringLiteral("工区数据模型尚未建立"); return Read::Unbound;
  }
  paleo::data::DataService service; std::string detail;
  if (!service.openProjectReadOnly(root.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
  }
  if (!service.readProjectPlanarRange(range, detail)) {
    if (error) *error = QString::fromUtf8(detail); return Read::Failed;
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
