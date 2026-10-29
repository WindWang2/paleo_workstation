// 层：功能
#include "projectsetup.h"
#include "../services/futuretask.h"
#include "folderimport.h"
#include "../io/projectsetupscan.h"
#include "../io/projectcoordinates.h"
#include "../io/dataimportservice.h"
#include "../io/areamodel.h"
#include "../catalog/areawellprojection.h"
#include "../catalog/areasourceprojection.h"
#include "../metadata/paleoprojectfile.h"
#include "../metadata/nativereference.h"
#include "../domain/mappinghorizons.h"
#include "../qgis/qgisprojectservice.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <filesystem>
#include <QElapsedTimer>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool registerModelWells(FolderImportWorkflow *imports, QString *error) {
  auto *service = imports ? imports->importService() : nullptr;
  auto *catalog = service ? service->catalog() : nullptr;
  if (!catalog) { if (error) *error = QStringLiteral("import service is not ready"); return false; }
  return registerAreaWells(*catalog, error);
}
bool restrictedProcess()
{
#ifdef Q_OS_WIN
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    const bool restricted = IsTokenRestricted(token);
    CloseHandle(token);
    return restricted;
  }
#endif
  return false;
}
QString locationFailure(const QString &directory, const QString &reason)
{
  QString message = ProjectSetupWorkflow::tr("工程保存位置无法写入：%1\n原因：%2").arg(directory, reason);
  message += restrictedProcess()
      ? ProjectSetupWorkflow::tr("\n当前应用运行在受限权限环境中。请从桌面或资源管理器正常启动应用，或选择当前应用可写的保存位置。")
      : ProjectSetupWorkflow::tr("\n请选择可写的保存位置，并检查目录权限、磁盘状态或安全软件的拦截记录。");
  return message;
}
bool probeWritableDirectory(const QString &directory, QString *error, const QString &displayPath = {})
{
  QTemporaryFile probe(QDir(directory).filePath(QStringLiteral(".paleo-write-check-XXXXXX")));
  if (probe.open() && probe.write("p", 1) == 1 && probe.flush() && probe.remove()) return true;
  if (error) *error = locationFailure(displayPath.isEmpty() ? directory : displayPath, probe.errorString());
  return false;
}
std::filesystem::path nativeDirectoryPath(const QString &directory)
{
#ifdef Q_OS_WIN
  return std::filesystem::path(QDir::toNativeSeparators(directory).toStdWString());
#else
  return std::filesystem::u8path(directory.toUtf8().constData());
#endif
}
}

ProjectSetupWorkflow::ProjectSetupWorkflow(QgisProjectService *project, FolderImportWorkflow *imports, QObject *parent)
  : QObject(parent), m_project(project), m_imports(imports) {
  if (project) connect(project, &QgisProjectService::projectAboutToClose, this, [this] {
    if (m_areaStop) m_areaStop->store(true);
  });
}
ProjectSetupWorkflow::~ProjectSetupWorkflow() { if (m_stop) m_stop->store(true); if (m_areaStop) m_areaStop->store(true); }
ProjectSetupSources ProjectSetupWorkflow::discover(const QString &root) { return discoverProjectSources(root); }
bool ProjectSetupWorkflow::containsProject(const QString &root)
{
  return QFileInfo::exists(paleoProjectFilePath(root)) || !QDir(root).entryList({QStringLiteral("*.qgz")}, QDir::Files).isEmpty();
}
bool ProjectSetupWorkflow::checkProjectLocation(const QString &path, QString *error)
{
  if (error) error->clear();
  const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
  if (path.trimmed().isEmpty()) return fail(tr("请填写工程保存位置。"));
  const QString directory = QFileInfo(path).absolutePath();
  if (QFileInfo::exists(path) || QFileInfo::exists(paleoProjectFilePath(directory)))
    return fail(tr("该位置已有工程文件：%1。请打开已有工程，或更换工程名称和保存位置。").arg(directory));
  QString ancestor = directory;
  while (!QFileInfo::exists(ancestor)) {
    const QString parent = QFileInfo(ancestor).absolutePath();
    if (parent == ancestor) return fail(tr("工程保存位置所在的磁盘或根目录不可用：%1").arg(directory));
    ancestor = parent;
  }
  if (!QFileInfo(ancestor).isDir()) return fail(tr("工程保存路径被同名文件占用：%1").arg(ancestor));
  if (ancestor == directory) return probeWritableDirectory(directory, error);
  // Probe a private child of the nearest existing parent, so parent folders
  // allowing subdirectories but not loose files are still valid destinations.
  QTemporaryDir probe(QDir(ancestor).filePath(QStringLiteral(".paleo-directory-check-XXXXXX")));
  if (!probe.isValid()) return fail(locationFailure(directory, probe.errorString()));
  return probeWritableDirectory(probe.path(), error, directory);
}
void ProjectSetupWorkflow::scan(const ProjectSetupSources &sources)
{
  if (m_stop) m_stop->store(true);
  m_stop = std::make_shared<std::atomic_bool>(false);
  const auto stop = m_stop;
  const quint64 generation = ++m_generation;
  auto *watcher = new QFutureWatcher<ProjectSetupPreview>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
    if (generation == m_generation && watcher->future().resultCount()) emit previewReady(watcher->result());
    watcher->deleteLater();
  });
  // PW-034：扫描走任务服务的有界池（不与 GDAL/QGIS 抢全局池，计入排空）。
  watcher->setFuture(QtConcurrent::run(paleo::tasks::pool(m_mapTasks.data()), [sources, stop] { return scanProjectSources(sources, [stop] { return stop->load(); }); }));
}
bool ProjectSetupWorkflow::create(const QString &path, const ProjectSetupSources &sources,
                                 const ProjectSetupPreview &preview, QString *error)
{
  QElapsedTimer creationTimer; creationTimer.start();
  QString phase = QStringLiteral("validate");
  const auto timing = qScopeGuard([&] { qInfo() << "project-create: final_phase=" << phase << "elapsed_ms=" << creationTimer.elapsed(); });
  if (error) error->clear();
  const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
  if (!m_project || !m_imports) return fail(tr("工程或导入服务未就绪"));
  if (!preview.errors.isEmpty()) return fail(preview.errors.join('\n'));
  if (!validateProjectSourceSnapshots(preview, error)) return false;
  if (!preview.projectCrs.isEmpty() && !paleo::io::isMetricProjectCrs(preview.projectCrs)) return fail(tr("工程基准必须为有效的米制投影坐标系"));
  PaleoProjectFile configuration;
  if (!preview.projectCrs.isEmpty()) {
    PaleoGeoreference reference;
    reference.kind = QStringLiteral("crs"); reference.sourceCrs = preview.projectCrs;
    reference.targetCrs = QStringLiteral("EPSG:4326");
    configuration.georeference = reference;
  }
  // Check coordinates before creating files or closing the active project.
  if (!QgisProjectService::validateMapReference(configuration, error)) return false;
  const QString directory = QFileInfo(path).absolutePath();
  if (!checkProjectLocation(path, error)) return false;
  const QString previous = m_project->projectPath();
  std::error_code directoryError;
  const bool createdDir = std::filesystem::create_directories(nativeDirectoryPath(directory), directoryError);
  if (directoryError) return fail(locationFailure(directory, tr("系统错误 %1：%2")
      .arg(directoryError.value()).arg(QString::fromLocal8Bit(directoryError.message().c_str()))));
  if (!probeWritableDirectory(directory, error)) {
    if (createdDir) QDir().rmdir(directory); // Remove only this attempt's still-empty directory.
    return false;
  }
  const auto removeCreated = [&](QString *removeError) {
    if (m_imports && m_imports->importService()) {
      auto *svc = m_imports->importService();
      if (QDir(svc->projectDir()) == QDir(directory))
        svc->setProjectDir(previous.isEmpty() ? QString() : QFileInfo(previous).absolutePath());
    }
    QStringList failed;
    const auto dropFile = [&](const QString &p) {
      if (!QFileInfo::exists(p)) return;
      QFile::setPermissions(p, QFile::permissions(p) | QFileDevice::WriteOwner);
      if (!QFile::remove(p)) failed << p;
    };
    if (createdDir) {
      if (QDir(directory).exists() && !QDir(directory).removeRecursively())
        failed << directory;
    } else {
      dropFile(path);
      dropFile(paleoProjectFilePath(directory));
    }
    if (removeError && !failed.isEmpty())
      *removeError = tr("回滚时未能删除：%1").arg(failed.join(QStringLiteral("，")));
  };
  const auto discardNewProject = [&](QString *removeError) {
    m_project->closeProject();
    removeCreated(removeError);
  };
  qInfo() << "project-create: validate_ms=" << creationTimer.elapsed();
  phase = QStringLiteral("qgis-and-databases");
  const auto qgisStart = creationTimer.elapsed();
  if (!m_project->createProject(path)) {
    const QString message = m_project->lastErrors().join('\n');
    // 锁失败等早退还停在上一工程，不能 close。只有已经切走或清单写失败才回滚。
    const bool switched = m_project->projectPath() == path;
    const bool closedByFailure = !previous.isEmpty() && m_project->projectPath().isEmpty();
    QString removeError;
    if (switched)
      m_project->closeProject();
    if ((createdDir && (switched || closedByFailure || m_project->projectPath() != path))
        || switched || closedByFailure)
      removeCreated(&removeError);
    const QString reported = removeError.isEmpty() ? message : message + '\n' + removeError;
    if ((switched || closedByFailure) && !previous.isEmpty() && m_project->projectPath().isEmpty() &&
        !m_project->openProject(previous))
      return fail(reported + '\n' + m_project->lastErrors().join('\n'));
    return fail(reported);
  }
  if (!QFileInfo::exists(paleoProjectFilePath(directory))) {
    const QString message = m_project->lastErrors().join('\n');
    QString removeError;
    discardNewProject(&removeError);
    const QString reported = removeError.isEmpty()
        ? (message.isEmpty() ? tr("写入工程清单失败") : message)
        : (message.isEmpty() ? tr("写入工程清单失败") : message) + '\n' + removeError;
    if (!previous.isEmpty() && !m_project->openProject(previous))
      return fail(reported + '\n' + m_project->lastErrors().join('\n'));
    return fail(reported);
  }
  qInfo() << "project-create: qgis_and_databases_ms=" << creationTimer.elapsed() - qgisStart;
  phase = QStringLiteral("schedule-directory-import");
  if (applySources(sources, preview, error)) return true;
  const QString message = error && !error->isEmpty() ? *error : tr("新建工程失败");
  QString removeError;
  discardNewProject(&removeError);
  const QString reported = removeError.isEmpty() ? message : message + '\n' + removeError;
  if (!previous.isEmpty() && !m_project->openProject(previous) && error)
    return fail(reported + '\n' + m_project->lastErrors().join('\n'));
  return fail(reported);
}
ProjectSetupSources ProjectSetupWorkflow::currentSources() const
{
  ProjectSetupSources s;
  if (!m_project || m_project->projectPath().isEmpty()) return s;
  bool ok = false;
  const auto file = readProjectFile(paleoProjectFilePath(QFileInfo(m_project->projectPath()).absolutePath()), &ok);
  if (!ok) return s;
  s.root = file.sourceAreaRoot;
  s.points = file.sourceStats.value("points").toString(); s.wells = file.sourceStats.value("wells").toString();
  s.seismic = file.sourceStats.value("seismic").toString(); s.horizons = file.sourceStats.value("horizons").toString();
  s.references = file.sourceStats.value("references").toString(); s.boundary = file.sourceStats.value("boundary").toString();
  s.paleomaps = file.sourceStats.value("paleomaps").toString();
  s.projectCrs = file.georeference && file.georeference->kind == QLatin1String("crs") ? file.georeference->sourceCrs : QString();
  s.normalizeMaps = file.sourceStats.value("normalizeMaps", true).toBool();
  s.mapChoices = file.sourceStats.value("mapChoices").toMap();
  s.pointMapping = file.sourceStats.value("pointMapping").toMap();
  const auto choices = file.sourceStats.value("wellTargets").toMap();
  for (auto it = choices.cbegin(); it != choices.cend(); ++it) s.wellTargets.insert(it.key(), it.value().toString());
  return s;
}
bool ProjectSetupWorkflow::applySources(const ProjectSetupSources &sources, const ProjectSetupPreview &input, QString *error)
{
  if (error) error->clear();
  auto preview = input;
  if (!m_project || m_project->projectPath().isEmpty() || !m_imports) { if (error) *error = tr("请先打开工程"); return false; }
  if (m_project->isReadOnly() || m_project->isOpening()) { if (error) *error = tr("工程只读或正在打开，不能修改资料设置"); return false; }
  if (!preview.errors.isEmpty()) { if (error) *error = preview.errors.join('\n'); return false; }
  if (!validateProjectSourceSnapshots(preview, error)) return false;
  for (auto &map : preview.maps) if (map.included && (!map.problem.isEmpty() || !isMappingHorizon(map.horizon))) {
    preview.sourceProblems.insert(map.path, map.problem.isEmpty() ? tr("古地理图未选择有效层位") : map.problem);
    map.included = false;
  }
  for (auto it = preview.sourceProblems.cbegin(); it != preview.sourceProblems.cend(); ++it)
    preview.paths.removeAll(it.key());
  const QString directory = QFileInfo(m_project->projectPath()).absolutePath();
  bool ok = false;
  auto file = readProjectFile(paleoProjectFilePath(directory), &ok, error);
  if (!ok) return false;
  const auto previous = file;
  file.sourceAreaRoot = sources.root;
  file.sourceStats = {{"points", sources.points}, {"wells", sources.wells}, {"seismic", sources.seismic},
      {"horizons", sources.horizons}, {"references", sources.references}, {"boundary", sources.boundary},
      {"pointMapping", preview.pointMapping}, {"files", preview.paths.size()}};
  QVariantMap mapChoices;
  for (const auto &map : input.maps) mapChoices.insert(map.path, QVariantMap{{"included", map.included}, {"horizon", map.horizon}});
  file.sourceStats.insert("paleomaps", sources.paleomaps);
  file.sourceStats.insert("normalizeMaps", sources.normalizeMaps);
  file.sourceStats.insert("mapChoices", mapChoices);
  QVariantMap choices;
  for (auto it = preview.wellTargets.cbegin(); it != preview.wellTargets.cend(); ++it) choices.insert(it.key(), it.value());
  file.sourceStats.insert(QStringLiteral("wellTargets"), choices);
  QVariantMap problems;
  for (auto it = preview.sourceProblems.cbegin(); it != preview.sourceProblems.cend(); ++it) problems.insert(it.key(), it.value());
  file.sourceStats.insert(QStringLiteral("sourceProblems"), problems);
  if (!preview.projectCrs.isEmpty() && file.georeference &&
      (file.georeference->kind != QLatin1String("crs") || !sameNativeCrs(file.georeference->sourceCrs, preview.projectCrs))) {
    if (error) *error = tr("当前工程与所选资料使用不同的坐标基础，请先统一资料坐标或新建工程。");
    return false;
  }
  if (!preview.projectCrs.isEmpty()) {
    PaleoGeoreference reference;
    reference.kind = QStringLiteral("crs"); reference.sourceCrs = preview.projectCrs; reference.targetCrs = QStringLiteral("EPSG:4326");
    reference.provenance = tr("新建工程：井点字段或矢量声明的原生坐标系；原始资料及字段对应保留在 sourceArea。");
    file.georeference = reference;
  }
  if (!writeProjectFile(directory, file, error)) return false;
  // Refresh the service from the newly persisted source settings too; later
  // map saves must not replace them with the previous in-memory configuration.
  if (!m_project->updateMapConfiguration(file, error)) {
    QString restoreError;
    if (!writeProjectFile(directory, previous, &restoreError) && error) *error += '\n' + restoreError;
    return false;
  }
  if (file.georeference && m_imports->importService()) m_imports->importService()->setGeoreference(*file.georeference);
  const quint64 session = m_project->sessionId();
  QTimer::singleShot(0, this, [this, sources, preview, session] {
    if (!m_project || m_project->sessionId() != session || !m_imports) return;
    importAreaSources(sources, preview, session);
  });
  return true;
}
void ProjectSetupWorkflow::importCatalogSources(const ProjectSetupSources &sources,
    const ProjectSetupPreview &preview, quint64 session)
{
    QString scanSummary = preview.notes.join('\n');
    for (auto it = preview.sourceProblems.cbegin(); it != preview.sourceProblems.cend(); ++it)
      scanSummary += '\n' + tr("未导入 %1：%2").arg(it.key(), it.value());
    const QString areaDatabase = paleo::area_model::directory(QFileInfo(m_project->projectPath()).absolutePath()) + "/data.sqlite";
    if (QFileInfo::exists(areaDatabase)) {
      QString registryError;
      auto *service = m_imports->importService();
      const QString project = QFileInfo(m_project->projectPath()).absolutePath();
      const bool registered = applyAreaPointMapping(project, sources.points, sources.pointMapping, &registryError) &&
          registerModelWells(m_imports, &registryError) && service && service->catalog() &&
          registerAreaSources(*service->catalog(), &registryError);
      const bool saved = registered && m_project->writeProject();
      QString summary = tr("工程资料索引已根据工区数据模型更新，未重复读取原始文件。") + scanSummary;
      if (!registryError.isEmpty()) summary += '\n' + registryError;
      if (!saved) summary += '\n' + m_project->lastErrors().join('\n');
      importMaps(sources, preview, session, summary, saved && preview.sourceProblems.isEmpty());
      return;
    }
    if (preview.paths.isEmpty()) {
      QString registryError;
      if (!registerModelWells(m_imports, &registryError)) { emit finished(registryError, false); return; }
      importMaps(sources, preview, session, tr("工程已保存，可随时补充资料。") + scanSummary, preview.sourceProblems.isEmpty()); return;
    }
      m_imports->setLinkExternal(true); // 工区目录导入始终引用来源，不沿用上次确认表的复制选项。
      m_imports->importFiles(preview.paths, preview.types, {}, [this, sources, preview, session, scanSummary](const auto &rows, const QString &error, bool cancelled) {
      if (!m_project || m_project->sessionId() != session) return;
      int failed = 0, imported = 0, skipped = 0, unresolved = 0;
      for (const auto &row : rows) {
        if (row.outcome == FolderRowResult::Outcome::Failed) ++failed;
        else if (row.outcome == FolderRowResult::Outcome::Skipped) ++skipped;
        else if (row.outcome == FolderRowResult::Outcome::Unresolved) ++unresolved;
        else ++imported;
      }
      QString registryError;
      const bool registered = registerModelWells(m_imports, &registryError);
      const bool saved = registered && m_project->writeProject();
      QString summary = tr("工程资料：已导入 %1，跳过 %2，待关联 %3，失败 %4。详细记录见任务与导入台账。").arg(imported).arg(skipped).arg(unresolved).arg(failed);
      summary += scanSummary;
      if (!error.isEmpty()) summary += '\n' + error;
      if (!registryError.isEmpty()) summary += '\n' + registryError;
      if (!saved) summary += '\n' + m_project->lastErrors().join('\n');
      // 用户取消文件批后不得再开古地理图任务；已取消不是下一阶段的启动条件。
      // 取消是结构化标志（ImportDone 第三参），不再嗅探 error 文案——跨层文案
      // 耦合一改措辞就失效（#801）。
      if (cancelled) {
        emit finished(summary, false);
        return;
      }
      importMaps(sources, preview, session, summary, !failed && preview.sourceProblems.isEmpty() && error.isEmpty() && saved);
    }, preview.wellTargets, preview.hashes, {}, preview.paths, {}, preview.mappings);
}
