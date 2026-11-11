// 层：数据
#include "areamodel.h"
#include "areamodel_conventions.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>

namespace paleo::area_model {
QString directory(const QString &project) { return QDir(project).filePath("artifacts/metadata/area-model"); }
bool ensureProject(const QString &project, QString *error) {
  paleo::data::DataService service; std::string detail;
  const auto root = directory(project).toUtf8().toStdString();
  const bool exists = QFileInfo::exists(directory(project) + "/data.sqlite");
  const bool ok = exists ? service.openProject(root, detail)
      : service.createProject(root, QFileInfo(project).fileName().toUtf8().toStdString(), detail);
  if (error) *error = QString::fromUtf8(detail);
  return ok && inheritSurveyHorizonConventions(service,error);
}
bool inheritProjectHorizonConventions(const QString &project,QString *error) {
  return !QFileInfo::exists(directory(project)+QStringLiteral("/data.sqlite"))||ensureProject(project,error);
}
bool importDirectory(const QString &project, const QString &source,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled, const QStringList &excluded,
                     const std::function<void(int, int, const QString &)> &progress) {
  return importDirectoryWithCrs(project, source, report, error, cancelled, excluded, progress, {}, {});
}
static bool finishImport(paleo::data::DataService &service, QString *error,
                        const std::function<bool()> &cancelled,
                        const std::function<void(int, int, const QString &)> &progress) {
  const auto stopped = [&] {
    if (!cancelled || !cancelled()) return false;
    if (error) *error = QStringLiteral("工区模型导入已取消");
    return true;
  };
  if (stopped()) return false;
  QElapsedTimer elapsed; elapsed.start();
  if (!inheritSurveyHorizonConventions(service, error)) return false;
  if (stopped()) return false;
  std::vector<paleo::data::DataEntry> horizons; std::string detail;
  if (service.listData("horizons", 0, horizons, detail) && horizons.size() >= 2) {
    if (progress) progress(0, 0, QStringLiteral("分析地震层位与时深关系，生成井震四等分小层"));
    paleo::data::SeismicStratigraphyBuildOptions options;
    paleo::data::SeismicStratigraphyBuildReport report;
    if (service.buildSeismicStratigraphicFramework(options, report, detail, cancelled)) {
      qInfo() << "area-seismic-stratigraphy: scheme=" << QString::fromUtf8(report.schemeId)
              << "horizons=" << report.horizonCount << "intervals=" << report.intervalCount
              << "sub_intervals=" << report.subIntervalCount
              << "matched_wells=" << report.matchedWellCount << "skipped_wells=" << report.skippedWellCount;
    } else {
      qWarning() << "area-seismic-stratigraphy skipped:" << QString::fromUtf8(detail);
    }
  }
  qInfo() << "area-import-finalization: elapsed_ms=" << elapsed.elapsed();
  return !stopped();
}
bool finalizeImport(const QString &project, QString *error,
                    const std::function<bool()> &cancelled,
                    const std::function<void(int, int, const QString &)> &progress) {
  paleo::data::DataService service; std::string detail;
  if (!service.openProject(directory(project).toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail);
    return false;
  }
  return finishImport(service, error, cancelled, progress);
}
static bool importWithCrs(const QString &project, const QString &source, bool singleFile,
                     paleo::data::ImportReport &report, QString *error,
                     const std::function<bool()> &cancelled, const QStringList &excluded,
                     const std::function<void(int, int, const QString &)> &progress,
                     const QString &crs, const QString &crsUnit, ImportFinalization finalization) {
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
  bool threadsConfigured = false;
  const int threads = qEnvironmentVariableIntValue("PALEO_IMPORT_PARSER_THREADS", &threadsConfigured);
  options.parserThreads = std::clamp(threadsConfigured ? threads : 4, 1, 4);
  bool pendingConfigured = false;
  const int pendingMb = qEnvironmentVariableIntValue("PALEO_IMPORT_PENDING_MB", &pendingConfigured);
  options.maxPendingBytes = qint64(std::clamp(pendingConfigured ? pendingMb : 256, 8, 1024)) * 1024 * 1024;
  options.parsing.comprehensiveWellMetreDepth = true;
  // A computation target is not a declaration of every input's source CRS.
  // Data_service collects explicit source declarations during parsing.
  if (!crs.isEmpty() && !service.setProjectCrs(crs.toUtf8().toStdString(), crsUnit.toUtf8().toStdString(), detail)) {
    if (error) *error = QString::fromUtf8(detail);
    return false;
  }
  for (const auto &path : excluded) options.excludedDirectories.push_back(path.toUtf8().toStdString());
  bool ok;
  if (singleFile) {
    if (progress) progress(0, 1, source);
    ok = service.importFile(source.toUtf8().toStdString(), options, report, detail, cancelled);
    if (progress && !report.cancelled) progress(1, 1, source);
  } else {
    ok = service.importDirectory(source.toUtf8().toStdString(), options, report, detail, [&progress](const paleo::data::ImportProgress &p) {
      if (progress) progress(p.completed, p.total, QString::fromUtf8(p.status) + ": " + QString::fromUtf8(p.file));
    }, cancelled);
  }
  if (finalization == ImportFinalization::Immediate && !report.cancelled &&
      (ok || report.imported > 0 || report.registered > 0 || report.cached > 0) &&
      !finishImport(service, error, cancelled, progress)) return false;
  qInfo() << "area-import: files=" << report.seen << "elapsed_ms=" << report.elapsedMs
          << "hash_ms=" << report.hashMs << "parse_ms=" << report.parseMs
          << "copy_ms=" << report.copyMs << "store_ms=" << report.storeMs
          << "hashed_bytes=" << report.hashedBytes << "workers=" << report.workerCount
          << "pending_bytes=" << options.maxPendingBytes << "copy_sources=false";
  if (error) *error = QString::fromUtf8(detail);
  return !report.cancelled && (ok || report.failed > 0);
}
bool importDirectoryWithCrs(const QString &project, const QString &source,
                            paleo::data::ImportReport &report, QString *error,
                            const std::function<bool()> &cancelled, const QStringList &excluded,
                            const std::function<void(int, int, const QString &)> &progress,
                            const QString &crs, const QString &crsUnit, ImportFinalization finalization) {
  return importWithCrs(project, source, false, report, error, cancelled, excluded, progress, crs, crsUnit, finalization);
}
bool importFileWithCrs(const QString &project, const QString &source,
                       paleo::data::ImportReport &report, QString *error,
                       const std::function<bool()> &cancelled,
                       const std::function<void(int, int, const QString &)> &progress,
                       const QString &crs, const QString &crsUnit, ImportFinalization finalization) {
  return importWithCrs(project, source, true, report, error, cancelled, {}, progress, crs, crsUnit, finalization);
}
}
