// 层：功能
#include "projectsetup.h"
#include "folderimport.h"
#include "../io/areamodel.h"
#include "../io/dataimportservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/futuretask.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QUuid>
#include <QPromise>
#include <QtConcurrent>

void ProjectSetupWorkflow::importAreaSources(const ProjectSetupSources &sources,
    const ProjectSetupPreview &preview, quint64 session)
{
  QStringList roots;
  if (!sources.root.isEmpty()) roots << sources.root;
  for (const auto &path : {sources.points, sources.wells, sources.seismic, sources.horizons, sources.references}) {
    if (path.isEmpty()) continue;
    const QString root = QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
    bool contained = false;
    for (const auto &parent : roots) {
      const QString relative = QDir(parent).relativeFilePath(root);
      if (relative == "." || (!relative.startsWith("../") && !QDir::isAbsolutePath(relative))) contained = true;
    }
    if (!contained) roots << root;
  }
  if (roots.isEmpty()) { importCatalogSources(sources, preview, session); return; }
  if (m_areaStop) m_areaStop->store(true);
  m_areaStop = std::make_shared<std::atomic_bool>(false);
  const auto stop = m_areaStop;
  const auto generation = ++m_areaGeneration;
  const QString project = QFileInfo(m_project->projectPath()).absolutePath();
  const auto staging = std::make_shared<QTemporaryDir>(
      QDir(QFileInfo(project).absolutePath()).filePath(".paleo-area-XXXXXX"));
  if (!staging->isValid()) { emit finished(QStringLiteral("无法创建工区模型暂存目录"), false); return; }
  const auto modelProblems = std::make_shared<QMap<QString, QString>>();
  const auto modelSummary = std::make_shared<QString>();
  auto *watcher = new QFutureWatcher<QString>(this);
  auto *modelTask = m_mapTasks ? m_mapTasks->beginExternal(QStringLiteral("导入工区数据模型"), {}, false) : nullptr;
  const auto reportStage = [modelTask](int done, int total, const QString &text) {
    paleo::tasks::reportProgress(modelTask, done, total, text);
  };
  reportStage(0, 0, QStringLiteral("准备工区数据模型"));
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, sources, preview, session, stop, project, staging, generation, modelSummary, modelProblems] {
    const QString error = watcher->future().isCanceled() ? QStringLiteral("工区模型导入已取消") : watcher->result();
    watcher->deleteLater();
    if (generation != m_areaGeneration || !m_project || m_project->sessionId() != session || !m_imports) return;
    if (stop->load() || !error.isEmpty()) { emit finished(error, false); return; }
    const QString final = paleo::area_model::directory(project);
    const QString prepared = paleo::area_model::directory(staging->path());
    const QString backup = final + ".previous-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(QFileInfo(final).absolutePath());
    const bool previous = QFileInfo::exists(final);
    if (previous && !QDir().rename(final, backup)) { emit finished(QStringLiteral("无法替换正在使用的工区模型"), false); return; }
    if (!QDir().rename(prepared, final)) {
      if (previous) QDir().rename(backup, final);
      emit finished(QStringLiteral("无法发布工区模型"), false); return;
    }
    if (previous) QDir(backup).removeRecursively();
    paleo::area_model::registerProject(project, m_imports->importService());
    auto projection = preview;
    projection.notes << *modelSummary;
    for (auto it = modelProblems->cbegin(); it != modelProblems->cend(); ++it) projection.sourceProblems.insert(it.key(), it.value());
    importCatalogSources(sources, projection, session);
  });
  const auto future = QtConcurrent::run(paleo::tasks::pool(m_mapTasks.data()), [roots, project, stop, staging, modelSummary, modelProblems, reportStage](QPromise<QString> &promise) {
    promise.setProgressRange(0, 0);
    promise.setProgressValueAndText(0, QStringLiteral("准备工区数据模型"));
    const QString active = paleo::area_model::directory(project);
    const QString prepared = paleo::area_model::directory(staging->path());
    if (QFileInfo::exists(active + "/data.sqlite")) {
      promise.setProgressValueAndText(0, QStringLiteral("准备现有工区模型的一致性快照"));
      reportStage(0, 0, QStringLiteral("准备现有工区模型的一致性快照"));
      paleo::data::DataService current;
      std::string detail;
      QDir().mkpath(prepared);
      if (!current.openProjectReadOnly(active.toUtf8().toStdString(), detail) ||
          !current.snapshotDatabase((prepared + "/data.sqlite").toUtf8().toStdString(), detail))
        { promise.addResult(QString::fromUtf8(detail)); return; }
    }
    for (const auto &root : roots) {
      if (!QFileInfo(root).isDir()) continue;
      paleo::data::ImportReport report;
      QString error;
      if (!paleo::area_model::importDirectory(staging->path(), root, report, &error,
          [stop] { return stop->load(); }, {QDir(project).filePath("artifacts"), staging->path(),
           QDir(root) == QDir(project) ? QDir(project).filePath("artifacts") : project},
          [&promise, reportStage](int done, int total, const QString &text) {
            reportStage(done, total, text);
            promise.setProgressRange(0, total);
            promise.setProgressValueAndText(done, text);
          }))
        { promise.addResult(error.isEmpty() ? QStringLiteral("工区模型导入失败") : error); return; }
      if (report.failed > 0) modelProblems->insert(root, error);
      *modelSummary += ProjectSetupWorkflow::tr("工区模型：扫描 %1，导入 %2，登记 %3，待核对 %4，跳过 %5，失败 %6。")
          .arg(report.seen).arg(report.imported).arg(report.registered).arg(report.review).arg(report.skipped).arg(report.failed) + '\n';
    }
    reportStage(0, 0, QStringLiteral("工区数据模型已准备完成，正在更新资料索引"));
    promise.addResult(QString());
  });
  auto *task = paleo::tasks::track(m_mapTasks, watcher, future, QStringLiteral("导入工区数据模型"), false,
      [watcher] { return watcher->future().resultCount() ? watcher->result() : QString(); }, true, modelTask);
  if (task) connect(task, &PaleoTask::cancelAsked, watcher, [stop] { stop->store(true); });
  watcher->setFuture(future);
}
