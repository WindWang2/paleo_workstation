// 层：功能
#include "projectsetup.h"
#include "folderimport.h"
#include "../io/projectsetupscan.h"
#include "../io/dataimportservice.h"
#include "../metadata/paleoprojectfile.h"
#include "../metadata/nativereference.h"
#include "../qgis/qgisprojectservice.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>

ProjectSetupWorkflow::ProjectSetupWorkflow(QgisProjectService *project, FolderImportWorkflow *imports, QObject *parent)
  : QObject(parent), m_project(project), m_imports(imports) {}
ProjectSetupWorkflow::~ProjectSetupWorkflow() { if (m_stop) m_stop->store(true); }
ProjectSetupSources ProjectSetupWorkflow::discover(const QString &root) { return discoverProjectSources(root); }
bool ProjectSetupWorkflow::containsProject(const QString &root)
{
  return QFileInfo::exists(paleoProjectFilePath(root)) || !QDir(root).entryList({QStringLiteral("*.qgz")}, QDir::Files).isEmpty();
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
  watcher->setFuture(QtConcurrent::run([sources, stop] { return scanProjectSources(sources, [stop] { return stop->load(); }); }));
}
bool ProjectSetupWorkflow::create(const QString &path, const ProjectSetupSources &sources,
                                 const ProjectSetupPreview &preview, QString *error)
{
  const auto fail = [&](const QString &message) { if (error) *error = message; return false; };
  if (!m_project || !m_imports) return fail(tr("工程或导入服务未就绪"));
  if (!preview.errors.isEmpty()) return fail(preview.errors.join('\n'));
  const QString directory = QFileInfo(path).absolutePath();
  if (QFileInfo::exists(path) || QFileInfo::exists(paleoProjectFilePath(directory))) return fail(tr("该位置已有工程，请选择新的工程目录或打开已有工程。"));
  if (!QDir().mkpath(directory)) return fail(tr("无法创建工程目录：%1").arg(directory));
  if (!m_project->createProject(path)) return fail(m_project->lastErrors().join('\n'));
  return applySources(sources, preview, error);
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
  s.pointMapping = file.sourceStats.value("pointMapping").toMap();
  const auto choices = file.sourceStats.value("wellTargets").toMap();
  for (auto it = choices.cbegin(); it != choices.cend(); ++it) s.wellTargets.insert(it.key(), it.value().toString());
  return s;
}
bool ProjectSetupWorkflow::applySources(const ProjectSetupSources &sources, const ProjectSetupPreview &preview, QString *error)
{
  if (!m_project || m_project->projectPath().isEmpty() || !m_imports) { if (error) *error = tr("请先打开工程"); return false; }
  if (m_project->isReadOnly() || m_project->isOpening()) { if (error) *error = tr("工程只读或正在打开，不能修改资料设置"); return false; }
  if (!preview.errors.isEmpty()) { if (error) *error = preview.errors.join('\n'); return false; }
  const QString directory = QFileInfo(m_project->projectPath()).absolutePath();
  bool ok = false;
  auto file = readProjectFile(paleoProjectFilePath(directory), &ok, error);
  if (!ok) return false;
  file.sourceAreaRoot = sources.root;
  file.sourceStats = {{"points", sources.points}, {"wells", sources.wells}, {"seismic", sources.seismic},
      {"horizons", sources.horizons}, {"references", sources.references}, {"boundary", sources.boundary},
      {"pointMapping", preview.pointMapping}, {"files", preview.paths.size()}};
  QVariantMap choices;
  for (auto it = preview.wellTargets.cbegin(); it != preview.wellTargets.cend(); ++it) choices.insert(it.key(), it.value());
  file.sourceStats.insert(QStringLiteral("wellTargets"), choices);
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
    if (!m_project->updateMapConfiguration(file, error)) return false;
    if (m_imports->importService()) m_imports->importService()->setGeoreference(reference);
  }
  if (!writeProjectFile(directory, file, error)) return false;
  const quint64 session = m_project->sessionId();
  QTimer::singleShot(0, this, [this, preview, session] {
    if (!m_project || m_project->sessionId() != session || !m_imports) return;
    if (preview.paths.isEmpty()) { emit finished(tr("空白工程已创建，可随时补充资料。"), true); return; }
    m_imports->importFiles(preview.paths, preview.types, {}, [this, session](const auto &rows, const QString &error) {
      if (!m_project || m_project->sessionId() != session) return;
      int failed = 0, imported = 0, skipped = 0, unresolved = 0;
      for (const auto &row : rows) {
        if (row.outcome == FolderRowResult::Outcome::Failed) ++failed;
        else if (row.outcome == FolderRowResult::Outcome::Skipped) ++skipped;
        else if (row.outcome == FolderRowResult::Outcome::Unresolved) ++unresolved;
        else ++imported;
      }
      const bool saved = m_project->writeProject();
      QString summary = tr("工程资料：已导入 %1，跳过 %2，待关联 %3，失败 %4。详细记录见任务与导入台账。").arg(imported).arg(skipped).arg(unresolved).arg(failed);
      if (!error.isEmpty()) summary += '\n' + error;
      if (!saved) summary += '\n' + m_project->lastErrors().join('\n');
      emit finished(summary, !failed && error.isEmpty() && saved);
    }, preview.wellTargets, preview.hashes, {}, preview.paths, preview.mappings);
  });
  return true;
}
