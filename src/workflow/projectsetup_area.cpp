// 层：功能
#include "projectsetup.h"
#include "folderimport.h"
#include "../io/areamodel.h"
#include "../io/areamodelpublication.h"
#include "../io/projectcoordinates.h"
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
#include <algorithm>

namespace {
bool insideImportRoot(const QString &root, const QString &path) {
  const QString relative = QDir(root).relativeFilePath(path);
  return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
         !QDir::isAbsolutePath(relative);
}
}

void ProjectSetupWorkflow::importAreaSources(const ProjectSetupSources &sources,
    const ProjectSetupPreview &preview, quint64 session)
{
  struct Input { QString path; bool directory; };
  QVector<Input> inputs;
  const auto addInput = [&](const QString &path, bool directory) {
    if (std::any_of(inputs.cbegin(), inputs.cend(), [&](const Input &input) {
          return input.directory ? insideImportRoot(input.path, path) : input.path == path;
        }))
      return;
    if (directory)
      inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [&](const Input &input) {
        return insideImportRoot(path, input.path);
      }), inputs.end());
    inputs.append({path, directory});
  };
  const QFileInfo root(sources.root);
  if (sources.importRootDirectory && root.isDir())
    addInput(root.absoluteFilePath(), true);
  for (const auto &path : {sources.points, sources.wells, sources.seismic, sources.horizons, sources.references}) {
    if (path.isEmpty()) continue;
    const QFileInfo source(path);
    const QString absolute = source.absoluteFilePath();
    if (preview.paths.contains(absolute))
      addInput(absolute, false);
    else if (source.isDir())
      addInput(absolute, true);
  }
  if (inputs.isEmpty()) { importCatalogSources(sources, preview, session); return; }
  if (m_areaStop) m_areaStop->store(true);
  m_areaStop = std::make_shared<std::atomic_bool>(false);
  const auto stop = m_areaStop;
  const auto generation = ++m_areaGeneration;
  const QString project = QFileInfo(m_project->projectPath()).absolutePath();
  QString recoveryError;
  if (auto *catalog=m_imports->importService()->catalog(); catalog &&
      !paleo::area_model::recoverModelPublication(*catalog,&recoveryError)) {
    emit finished(recoveryError,false); return;
  }
  const auto staging = std::make_shared<QTemporaryDir>(
      QDir(QFileInfo(project).absolutePath()).filePath(".paleo-area-XXXXXX"));
  if (!staging->isValid()) { emit finished(QStringLiteral("无法创建工区模型暂存目录"), false); return; }
  const auto modelProblems = std::make_shared<QMap<QString, QString>>();
  const auto modelSummary = std::make_shared<QString>();
  const auto coordinates = std::make_shared<paleo::data::ProjectCrsInfo>();
  const auto expected = std::make_shared<paleo::data::ModelChangeHead>();
  auto *watcher = new QFutureWatcher<QString>(this);
  auto *modelTask = m_mapTasks ? m_mapTasks->beginExternal(QStringLiteral("导入工区数据模型"), {}, false) : nullptr;
  const auto reportStage = [modelTask](int done, int total, const QString &text) {
    paleo::tasks::reportProgress(modelTask, done, total, text);
  };
  reportStage(0, 0, QStringLiteral("准备工区数据模型"));
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, sources, preview, session, stop, project, staging, generation, modelSummary, modelProblems, coordinates, expected] {
    const QString error = watcher->future().isCanceled() ? QStringLiteral("工区模型导入已取消") : watcher->result();
    watcher->deleteLater();
    if (generation != m_areaGeneration || !m_project || m_project->sessionId() != session || !m_imports) return;
    if (stop->load() || !error.isEmpty()) { emit finished(error, false); return; }
    staging->setAutoRemove(false);
    QString publicationError;
    if (!paleo::area_model::publishPreparedModel(project, staging->path(), *expected, &publicationError)) {
      if (!paleo::area_model::hasPendingModelPublication(project)) staging->setAutoRemove(true);
      emit finished(publicationError, false); return;
    }
    paleo::area_model::registerProject(project, m_imports->importService());
    auto projection = preview;
    const auto crs = QString::fromUtf8(coordinates->coordinateSystem);
    const bool confirmed = coordinates->selectionBasis == "import" || coordinates->selectionBasis == "default";
    if (preview.projectCrs.isEmpty() && !m_project->georeference() &&
        confirmed && !coordinates->hasConflict &&
        coordinates->coordinateUnit == "m" && paleo::io::isMetricProjectCrs(crs)) {
      auto configuration = m_project->mapConfiguration();
      PaleoGeoreference reference; reference.kind = QStringLiteral("crs");
      reference.sourceCrs = crs; reference.targetCrs = QStringLiteral("EPSG:4326");
      configuration.georeference = reference;
      QString coordinateError;
      if (!m_project->updateMapConfiguration(configuration, &coordinateError))
        modelProblems->insert(QStringLiteral("工区坐标系"), coordinateError);
    }
    projection.notes << *modelSummary;
    for (auto it = modelProblems->cbegin(); it != modelProblems->cend(); ++it) projection.sourceProblems.insert(it.key(), it.value());
    importCatalogSources(sources, projection, session);
  });
  const auto crs = preview.projectCrs;
  const auto crsUnit = preview.pointMapping.value("unit").toString();
  const auto future = QtConcurrent::run(paleo::tasks::pool(m_mapTasks.data()), [inputs, project, stop, staging, modelSummary, modelProblems, coordinates, reportStage, crs, crsUnit, expected](QPromise<QString> &promise) {
    promise.setProgressRange(0, 0);
    promise.setProgressValueAndText(0, QStringLiteral("准备工区数据模型"));
    const QString active = paleo::area_model::directory(project);
    const QString prepared = paleo::area_model::directory(staging->path());
    {
      promise.setProgressValueAndText(0, QStringLiteral("准备现有工区模型的一致性快照"));
      reportStage(0, 0, QStringLiteral("准备现有工区模型的一致性快照"));
      paleo::data::DataService current;
      std::string detail;
      QDir().mkpath(prepared);
      const bool opened=QFileInfo::exists(active+"/data.sqlite") ? current.openProject(active.toUtf8().toStdString(),detail) :
          current.createProject(active.toUtf8().toStdString(),QFileInfo(project).fileName().toUtf8().toStdString(),detail);
      if (!opened ||
          !current.readModelChangeHead(*expected, detail) || !expected->available ||
          !current.snapshotDatabaseCancellable((prepared + "/data.sqlite").toUtf8().toStdString(), detail, [stop] { return stop->load(); }))
        { promise.addResult(QString::fromUtf8(detail)); return; }
    }
    const auto cancelled = [stop] { return stop->load(); };
    const auto progress = [&promise, reportStage](int done, int total, const QString &text) {
      reportStage(done, total, text);
      promise.setProgressRange(0, total);
      promise.setProgressValueAndText(done, text);
    };
    for (const auto &input : inputs) {
      if (input.directory && !QFileInfo(input.path).isDir()) continue;
      paleo::data::ImportReport report;
      QString error;
      const bool imported = input.directory
          ? paleo::area_model::importDirectoryWithCrs(staging->path(), input.path, report, &error,
                cancelled, {QDir(project).filePath("artifacts"), staging->path(),
                QDir(input.path) == QDir(project) ? QDir(project).filePath("artifacts") : project},
                progress, crs, crsUnit)
          : paleo::area_model::importFileWithCrs(staging->path(), input.path, report, &error,
                cancelled, progress, crs, crsUnit);
      if (!imported)
        { promise.addResult(error.isEmpty() ? QStringLiteral("工区模型导入失败") : error); return; }
      if (report.failed > 0) modelProblems->insert(input.path, error);
      *modelSummary += ProjectSetupWorkflow::tr("工区模型：扫描 %1，导入 %2，登记 %3，待核对 %4，跳过 %5，失败 %6。")
          .arg(report.seen).arg(report.imported).arg(report.registered).arg(report.review).arg(report.skipped).arg(report.failed) + '\n';
    }
    paleo::data::DataService model; std::string detail;
    paleo::data::ModelChangeHead preparedHead, publicationHead;
    if (!model.openProject(prepared.toUtf8().toStdString(), detail) ||
        !model.readModelChangeHead(preparedHead, detail) ||
        !model.forkModelGeneration(preparedHead, publicationHead, detail) ||
        !model.readProjectCrsInfo(*coordinates, detail)) { promise.addResult(QString::fromUtf8(detail)); return; }
    if (coordinates->hasConflict)
      *modelSummary += ProjectSetupWorkflow::tr("资料中存在坐标系或单位冲突，已在数据模型中保留各来源声明，请核对。") + '\n';
    QString basisLabel = QString::fromUtf8(coordinates->selectionBasis);
    if (basisLabel == QLatin1String("import")) basisLabel = ProjectSetupWorkflow::tr("来源声明");
    else if (basisLabel == QLatin1String("default")) basisLabel = ProjectSetupWorkflow::tr("未声明 EPSG 时默认");
    else if (basisLabel == QLatin1String("manual")) basisLabel = ProjectSetupWorkflow::tr("人工指定");
    if (!coordinates->coordinateSystem.empty())
      *modelSummary += ProjectSetupWorkflow::tr("数据模型坐标系：%1 [%2]；依据：%3；来源声明 %4 条。")
          .arg(QString::fromUtf8(coordinates->coordinateSystem), QString::fromUtf8(coordinates->coordinateUnit),
               basisLabel).arg(coordinates->candidates.size()) + '\n';
    if (coordinates->selectionBasis == "default" && !coordinates->candidates.empty())
      *modelSummary += ProjectSetupWorkflow::tr("来源坐标系不是 EPSG 代码，工程基准已默认采用 EPSG:32650。已写明且不能对应到 EPSG 的坐标系保持原样。") + '\n';
    else if (coordinates->coordinateSystem.empty() && !coordinates->hasConflict)
      *modelSummary += ProjectSetupWorkflow::tr("资料未提供可唯一确定的 EPSG，工程基准已默认采用 EPSG:32650。") + '\n';
    reportStage(0, 0, QStringLiteral("工区数据模型已准备完成，正在更新资料索引"));
    promise.addResult(QString());
  });
  auto *task = paleo::tasks::track(m_mapTasks, watcher, future, QStringLiteral("导入工区数据模型"), false,
      [watcher] { return watcher->future().resultCount() ? watcher->result() : QString(); }, true, modelTask);
  if (task) connect(task, &PaleoTask::cancelAsked, watcher, [stop] { stop->store(true); });
  watcher->setFuture(future);
}
