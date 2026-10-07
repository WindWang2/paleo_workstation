// 层：QGIS 封装
#include "qgisprojectservice.h"
#include "metadata/paleoprojectfile.h"
#include <QDomDocument>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QThread>
#include <QtConcurrent>
#include <atomic>
#include <qgslayoutmanager.h>
#include <qgslayertree.h>
#include <qgsauxiliarystorage.h>
#include <qgsannotationlayer.h>
#include <qgselevationprofilemanager.h>
#include <qgslabelingenginesettings.h>
#include <qgsmaplayer.h>
#include <qgsmapthemecollection.h>
#include <qgsproject.h>
#include <qgsprojectelevationproperties.h>
#include <qgsprojectgpssettings.h>
#include <qgsreadwritecontext.h>
#include <qgsvectorlayer.h>

struct ProjectLoadState {
  QString input, path, error;
  QStringList warnings;
  std::optional<PaleoGeoreference> georeference;
  PaleoProjectFile mapConfiguration;
  std::unique_ptr<QgsProject> project;
  QDomDocument document;
  QDomDocument treeDocument;
  std::atomic_bool cancelled{false};
  QDateTime modified;
  qint64 size = 0;
};

void QgisProjectService::cancelOpen()
{
  if (!m_opening)
    return;
  ++m_openGeneration;
  if (m_pendingLoad)
    m_pendingLoad->cancelled.store(true);
  m_lastOpenCancelled = true;
  m_opening = false;
  emit openAborted();
  emit openActiveChanged(false);
  emit openFinished(false);
}

bool QgisProjectService::openProjectAsync(const QString &input)
{
  if (m_opening || m_openFuture.isRunning()) {
    m_errors = {tr("正在读取工程，请等待完成或取消后再打开")};
    return false;
  }
  m_errors.clear();
  m_lastOpenCancelled = false;
  const QFileInfo source(input);
  if (!source.isFile()) {
    m_errors << tr("工程文件不存在：%1").arg(input);
    return false;
  }
  // 锁/用户决策保持在 GUI，拒绝时不碰现有工程。
  if (!runGate(source.absolutePath(), false))
    return false;
  auto state = std::make_shared<ProjectLoadState>();
  state->input = source.absoluteFilePath();
  m_pendingLoad = state;
  const quint64 generation = ++m_openGeneration;
  QThread *ownerThread = thread();
  auto *watcher = new QFutureWatcher<void>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
          [this, watcher, generation](int value) {
    if (generation == m_openGeneration)
      emit openProgress(value, watcher->progressText());
  });
  connect(watcher, &QFutureWatcherBase::progressTextChanged, this,
          [this, watcher, generation](const QString &status) {
    if (generation == m_openGeneration)
      emit openProgress(watcher->progressValue(), status);
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, state, generation] {
    watcher->deleteLater();
    if (generation != m_openGeneration || state->cancelled.load()) {
      if (m_pendingLoad == state)
        m_pendingLoad.reset();
      return;
    }
    m_errors = state->warnings;
    bool success = state->error.isEmpty();
    const QFileInfo current(state->path);
    if (success && (current.size() != state->size || current.lastModified() != state->modified)) {
      state->error = tr("读取期间工程文件已变化，请重新打开");
      success = false;
    }
    if (success) {
      emit openProgress(85, tr("接管图层与恢复工程视图"));
      // 只在读取完全成功后替换旧工程。Qt/QGIS 视图始终引用同一 QgsProject。
      notifyAboutToClose();
      // QGIS 读取原文件的设置与附件，但不打开数据源；重型 provider 已由 worker 读好。
      // 用原路径可保留 QGZ 的辅助存储、附件和所有工程设置的相对路径语义。
      success = m_project->read(state->path,
          Qgis::ProjectReadFlag::DontResolveLayers | Qgis::ProjectReadFlag::DontLoadLayouts |
          Qgis::ProjectReadFlag::DontLoad3DViews);
      if (success) {
        m_project->setFileName(state->path);
        m_project->setOriginalPath(state->path);
        m_project->setPresetHomePath(state->project->presetHomePath());
        m_project->removeAllMapLayers(); // 不解析数据源的占位层由后台已读图层替换
        const auto loadedLayers = state->project->mapLayers();
        const auto attachments = state->project->attachedFiles();
        QgsReadWriteContext context;
        context.setPathResolver(m_project->pathResolver());
        context.setTransformContext(m_project->transformContext());
        context.setProjectTranslator(m_project);
        for (auto *layer : loadedLayers) {
          // 仅附件数据源需要指向当前工程自己的解压目录；外部 GIS 数据不重读。
          QString source = layer->source();
          for (const auto &attachment : attachments)
            if (source.contains(attachment)) {
              const QString rebased = m_project->resolveAttachmentIdentifier(
                  state->project->attachmentIdentifier(attachment));
              if (!rebased.isEmpty())
                source.replace(attachment, rebased);
            }
          if (source != layer->source())
            layer->setDataSource(source, layer->name(), layer->providerType(),
                QgsDataProvider::ProviderOptions{m_project->transformContext()});
          if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
            const bool hadAuxiliary = vector->auxiliaryLayer();
            if (!vector->loadAuxiliaryLayer(*m_project->auxiliaryStorage()) && hadAuxiliary) {
              vector->setAuxiliaryLayer(nullptr);
              m_errors << tr("图层 %1 的辅助存储无法恢复").arg(layer->name());
            }
          }
          state->project->takeMapLayer(layer);
          m_project->addMapLayer(layer, false);
        }
        for (auto *layer : m_project->mapLayers())
          layer->resolveReferences(m_project);
        m_project->mainAnnotationLayer()->resolveReferences(m_project);
        auto *root = m_project->layerTreeRoot();
        root->removeAllChildren();
        const auto tree = state->treeDocument.documentElement().firstChildElement("layer-tree-group");
        root->readChildrenFromXml(tree, context);
        root->resolveReferences(m_project);
        root->readLayerOrderFromXml(tree);
        m_project->mapThemeCollection()->readXml(state->document);
        auto labels = m_project->labelingEngineSettings();
        labels.resolveReferences(m_project);
        m_project->setLabelingEngineSettings(labels);
        m_project->elevationProfileManager()->resolveReferences(m_project);
        m_project->elevationProperties()->resolveReferences(m_project);
        m_project->gpsSettings()->resolveReferences(m_project);
        emit openProgress(92, tr("恢复打印布局与工程设置"));
        if (!m_project->layoutManager()->readXml(state->document.documentElement(), state->document))
          m_errors << tr("部分打印布局恢复失败，请检查图件设计器");
        // 占位层替换后按 QGIS 原生信号重新绑定关系与画布读取钩子。
        emit m_project->readProject(state->document);
        emit m_project->readProjectWithContext(state->document, context);
        m_georeference = state->georeference;
        m_mapConfiguration = state->mapConfiguration;
        if (!m_georeference && m_mapConfiguration.name.isEmpty()) {
          const auto json = m_project->readEntry("paleo", "georeference");
          PaleoGeoreference reference;
          QString error;
          if (paleoGeoreferenceFromJson(QJsonDocument::fromJson(json.toUtf8()).object(), &reference, &error))
            m_georeference = reference;
        }
        if (m_georeference)
          m_project->writeEntry("paleo", "georeference", QString::fromUtf8(
              QJsonDocument(paleoGeoreferenceToJson(*m_georeference)).toJson(QJsonDocument::Compact)));
        else m_project->removeEntry("paleo", "georeference");
        m_path = state->path;
        applyMapConfiguration();
        ++m_sessionId;
        // 清单收养在主线程/锁决策之后。
        const QString dir = QFileInfo(m_path).absolutePath();
        if (!QFile::exists(paleoProjectFilePath(dir))) {
          auto adopted = projectFileForQgz(m_path);
          adopted.georeference = m_georeference;
          adopted.mapCrs = m_mapConfiguration.mapCrs;
          adopted.basemapEnabled = m_mapConfiguration.basemapEnabled;
          adopted.basemapTopo = m_mapConfiguration.basemapTopo;
          adopted.basemapHillshade = m_mapConfiguration.basemapHillshade;
          QString error;
          if (!writeProjectFile(dir, adopted, &error))
            m_errors << tr("无法收养工程清单：%1").arg(error);
          else m_mapConfiguration = adopted;
        }
        m_project->setDirty(false);
        emit openProgress(96, tr("装配工程数据与面板"));
        emit projectOpened(m_path);
        emit openProgress(100, tr("工程打开完成"));
      } else {
        state->error = m_project->error();
        failAfterClose();
      }
    }
    if (!success) {
      m_errors << (state->error.isEmpty() ? tr("工程打开失败") : state->error);
      emit openAborted();
    }
    state->project.reset();
    m_pendingLoad.reset();
    m_opening = false;
    emit openActiveChanged(false);
    emit openFinished(success);
  });
  m_opening = true;
  emit openActiveChanged(true);
  emit openProgress(0, tr("后台解析工程清单"));
  m_openFuture = QtConcurrent::run([state, ownerThread](QPromise<void> &promise) {
    promise.setProgressRange(0, 100);
    state->path = state->input;
    const QString dir = QFileInfo(state->input).absolutePath();
    const QString manifest = state->input.endsWith(".paleo", Qt::CaseInsensitive)
                                 ? state->input : paleoProjectFilePath(dir);
    if (QFile::exists(manifest)) {
      bool ok = false; QString error;
      const auto pf = readProjectFile(manifest, &ok, &error);
      if (!ok && state->input.endsWith(".paleo", Qt::CaseInsensitive)) {
        state->error = error;
        return;
      }
      if (!ok)
        state->warnings << QStringLiteral("工程清单无法读取：%1").arg(error);
      if (ok) {
        state->georeference = pf.georeference;
        state->mapConfiguration = pf;
        if (!pf.georeferenceError.isEmpty()) state->warnings << pf.georeferenceError;
        if (state->input.endsWith(".paleo", Qt::CaseInsensitive))
          state->path = QDir(dir).filePath(pf.qgz);
        for (const auto &member : missingMembers(dir, pf))
          state->warnings << QStringLiteral("工程成员缺失：%1").arg(member);
      }
    }
    const QFileInfo file(state->path);
    if (!file.isFile()) { state->error = QStringLiteral("工程 QGIS 成员不存在"); return; }
    state->size = file.size(); state->modified = file.lastModified();
    if (state->cancelled.load()) return;
    state->project = std::make_unique<QgsProject>();
    const auto documentConnection = QObject::connect(state->project.get(), &QgsProject::readProject, state->project.get(),
                     [target = state.get()](const QDomDocument &doc) { target->document = doc.cloneNode(true).toDocument(); });
    const auto loadingConnection = QObject::connect(state->project.get(), &QgsProject::loadingLayer, state->project.get(),
                     [&promise](const QString &name) { promise.setProgressValueAndText(
                         std::max(10, promise.future().progressValue()), QStringLiteral("后台加载图层：%1").arg(name)); });
    const auto loadedConnection = QObject::connect(state->project.get(), &QgsProject::layerLoaded, state->project.get(),
                     [&promise](int done, int total) { promise.setProgressValue(total > 0 ? 10 + 65 * done / total : 10); });
    // QGIS 的 DontLoadLayouts 契约允许后台读项目；3D 控件同样留在 GUI 恢复。
    const bool loaded = state->project->read(state->path,
        Qgis::ProjectReadFlag::DontLoadLayouts | Qgis::ProjectReadFlag::DontLoad3DViews);
    if (!loaded || state->document.isNull()) {
      state->error = state->project->error().isEmpty() ? QStringLiteral("工程解析失败") : state->project->error();
      state->project.reset();
      return;
    }
    if (state->cancelled.load()) { state->project.reset(); return; }
    // 冻结完成引用解析后的图层树（含嵌入组和自定义绘制顺序）。接管图层会改变旧树。
    auto root = state->treeDocument.createElement("qgis");
    state->treeDocument.appendChild(root);
    QgsReadWriteContext context;
    context.setPathResolver(state->project->pathResolver());
    state->project->layerTreeRoot()->writeXml(root, context);
    // 读取器的临时信号捕获 promise 引用，在任务退出前拆掉。
    QObject::disconnect(documentConnection);
    QObject::disconnect(loadingConnection);
    QObject::disconnect(loadedConnection);
    // QgsLayerTree 由 unique_ptr 持有，不是 QgsProject 的 QObject 子对象。
    state->project->layerTreeRoot()->moveToThread(ownerThread);
    state->project->moveToThread(ownerThread);
    promise.setProgressValueAndText(80, QStringLiteral("后台读取完成，准备恢复视图"));
  });
  watcher->setFuture(m_openFuture);
  return true;
}
