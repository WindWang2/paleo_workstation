// 层：组装根
#include "appcontext.h"
#include "../workflow/mappingworkbench.h"

#include "../ai/onnxpredictionservice.h" // ORT-free header; instantiation is PALEO_HAVE_ORT-guarded
#include "../qgis/qgisruntime.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../services/toolavailability.h"
#include "../services/paleotaskservice.h"
#include "../services/startuptrace.h" // goal/perf-systematize 簇1：启动分段打点
#include "../services/crashreport.h" // wave4：projectOpened → 报告头工程路径
#include "../domain/arearules.h"         // wave4 接线点：projectOpened → setProjectDir
#include "../linkage/selectioncontext.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/wellmaplink.h"
#include "../catalog/datacatalog.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/projectlock.h"
#include "../metadata/layermanifest.h"
#include "../qgis/manifestprojection.h"
#include "../io/dataimportservice.h"
#include "../qgis/qgislayoutservice.h"
#include "../workflow/workflows.h"
#include "../workflow/faultinterpretationcontroller.h" // goal/fault-interpretation
#include "../services/projectdata.h"
#include "../workflow/mappingworkflow.h"
#include "../metadata/mapversionstore.h"
#include "../workflow/mapversioncontroller.h"

#include <QApplication>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QDebug>

#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgsmessagelog.h>
#include <qgsvectorlayer.h>
#include <qgsmarkersymbol.h>
#include <qgssinglesymbolrenderer.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextformat.h>

// Composition root. QgisRuntime::initialize() must run before any Qgs*
// construction (it owns the QgsApplication), so it is the very first thing the
// ctor does; everything else hangs off `this` as a QObject child and dies in
// the dtor before QgisRuntime::shutdown().
//
// Manifest: §37 declares the manifest the layer-set authority, persisted per
// project at "<qgz>.project.sqlite" (interim convention; the full
// metadata/project.sqlite layout from §4 lands with the store migration). A
// LayerManifest object exists from ctor — bound to an empty path, never opened
// — so QgisLayerService captures a permanently valid pointer. On
// projectOpened the object is copy-assigned in place to the real path
// (LayerManifest is a plain value type over a QString path, and the sqlite
// connection is looked up lazily per path in layermanifest.cpp), then opened.
namespace
{
  // Which AppContext instances performed the initialize() — mirrors the
  // file-scope guard pattern in qgiscanvascontroller.cpp so the header's
  // member set stays untouched. Only the initializing context shuts down.
  QSet<AppContext *> s_runtimeOwners;

  QString manifestPathFor(const QString &qgzPath)
  {
    return qgzPath + QStringLiteral(".project.sqlite");
  }

  QString gpkgPathFor(const QString &qgzPath)
  {
    const QFileInfo fi(qgzPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".gpkg"));
  }
} // namespace

AppContext::AppContext(const QString &qgisPrefix, QObject *parent)
  : QObject(parent)
{
  // QgsApplication is a QApplication: initialize() creates it, and it must be
  // the process's only Q(Core)Application. If some bootstrap already brought
  // the runtime up we are ready but do not own its shutdown.
  if (QgisRuntime::isInitialized())
  {
    m_ready = true;
  }
  else if (QgisRuntime::initialize(qgisPrefix))
  {
    s_runtimeOwners.insert(this);
    m_ready = true;
  }

  if (!m_ready)
  {
    qWarning() << "AppContext: QgisRuntime not initialized (prefix" << qgisPrefix
               << ") — all service accessors will return nullptr";
    return;
  }
  // 判别力测试缝（goal/perf-systematize 簇1）：注入已知劣化进 qgis 初始化
  // 段——它是启动比率门 qgis_init_share 的分子，tst_startup_trace 用它证明
  // 门对真实进程劣化必红。产品路径不设此 env。
  if (const QByteArray inj = qgetenv("PALEO_STARTUP_INJECT_DELAY_MS"); !inj.isEmpty())
    QThread::msleep(qMax<qint64>(0, inj.toLongLong()));
  StartupTrace::mark(QStringLiteral("qgis_app_ready")); // QgsApplication+initQgis 完

  m_store = new PaleoProjectStore(this);
  m_taskSvc = new PaleoTaskService(m_store, this);
  m_projectSvc = new QgisProjectService(this);

  // Deferred binding: constructed un-opened on an empty path; rebound (same
  // object, same pointer the layer service holds) when a project opens.
  m_manifest = new LayerManifest(QString());
  m_layerSvc = new QgisLayerService(m_projectSvc, m_manifest, this);

  // §37: every project write embeds the full declared layer set so
  // uninstantiated declarations survive the .qgz projection. The provider is
  // the error-reporting tryDeclared — a manifest read failure fails the write
  // instead of silently embedding an empty declaration set.
  m_projectSvc->setDeclarationProvider(
      [this](QVector<LayerDeclaration> *out, QString *error) {
        return m_layerSvc->tryDeclared(out, error);
      });

  m_import = new DataImportService(m_store, this);
  connect(m_import, &DataImportService::layerDeclared, this,
          [this](const LayerDeclaration &decl) {
            if (m_layerSvc) {
              QString derr;
              m_layerSvc->declare(decl, &derr);
            }
          });

  m_canvasCtl = new QgisCanvasController(this);
  m_canvasCtl->setLayerResolver([this](const QString &layerId) -> QgsMapLayer * {
    return m_layerSvc ? m_layerSvc->instantiate(layerId, nullptr) : nullptr;
  });
  m_procSvc = new QgisProcessingService(m_store, this);
  m_procSvc->setProject(m_projectSvc->project());
  m_editSvc = new QgisEditingService(m_store, this);
  m_styleSvc = new QgisStyleService(this);
  m_toolSvc = new ToolAvailabilityService(m_store, this);
  m_selection = new SelectionContext(this);

  // goal/fault-interpretation：断层解释编排器（存储值成员在 projectOpened
  // 重绑到本工程 meta 库；控制器地址稳定，撤销栈不因重开工程丢失）。
  m_faultCtl = new paleo::fault::FaultInterpretationController(&m_faultStore, m_selection, this);

  // 地震—地图联动: binds the selection context to the canvas. Forcing canvas()
  // here materializes the widget early; the main window reparents it into the
  // center stack. The link holds the context via its constructor — the
  // "paleo.seismic.ctx" dynamic property existed for SeismicPreviewPanel,
  // which is retired (T30); nothing else reads it.
  m_seismicLink = new SeismicMapLink(m_canvasCtl->canvas(), m_selection, this);

  // 井—地图联动（§31，预览壳重排接入）：与地震同一模式——画布拾取 ⇄
  // SelectionContext。wells 图层在项目打开时由 catalog 井点 GeoJSON
  // 声明/实例化后绑定（refreshWellsLayer），图层对象出现前链接静默空转。
  m_wellLink = new WellMapLink(m_canvasCtl->canvas(), m_selection, this);
  // catalog 每次变更（井新增/井口重挂/关联撤销）都重写井点 GeoJSON 并
  // 刷新图层——稳定指针（catalog 由 importSvc 持有，open 原地重绑）。
  // D6：导入使井点范围变大时 zoom-to-content（无关变更只重写同一份
  // geojson，范围不变不抢视野）。
  connect(m_import->catalog(), &DataCatalog::changed, this,
          [this] { refreshWellsLayer(true); });

  // Workflow orchestrators — thin bindings over the services above.
  // Layout service rides the service-owned QgsProject (eager — valid already).
  m_layoutSvc = new QgisLayoutService(m_projectSvc->project(), this);

  // Workflow orchestrators — thin bindings over the services above.
  m_predictionWf = new PredictionWorkflow(m_procSvc, m_layerSvc, this);
#if PALEO_HAVE_ORT
  // onnx:* 算法由 PaleoOnnxService 提供（ET4 spike 产物）。恒绑定：没有运行
  // 库或 models/ 里没有模型时服务自己如实报告——availableModels() 为空，
  // onnx:* 就不出现在 availableAlgorithms() 里。模型根在 projectOpened 时
  // 指向 <工程目录>/models。
  m_onnxSvc = new PaleoOnnxService(this);
  m_predictionWf->setOnnxService(m_onnxSvc);
#endif
  m_constraintWf = new ConstraintWorkflow(m_procSvc, m_layerSvc, this);
  m_constraintWf->setStore(m_store); // GeoPackage constraint persistence (wave/constraint-gpkg)
  m_mappingWorkbench = new MappingWorkbench(m_layerSvc, m_procSvc, m_projectSvc, m_constraintWf, this);
  m_compositionWf = new CompositionWorkflow(m_procSvc, m_layerSvc, this);
  m_validationWf = new ValidationWorkflow(m_layerSvc, m_store, this);

  // wave/mapping-pipeline 阶段C+E — 读侧门面 / D61 编图链 / 版本状态机。
  // catalog.json 由数据底座包落位；这里只在工程目录里发现它时绑定（未合
  // 并期间手工放置合成 catalog 亦可驱动整条链）。版本存储与 LayerManifest
  // 同库（meta sqlite），路径在 projectOpened 时原地重绑。
  m_projectData = new ProjectDataFacade(this);
  m_mappingWf = new MappingWorkflow(m_constraintWf, m_compositionWf, m_layerSvc, this);
  m_mappingWf->setProjectData(m_projectData);
  m_validationWf->setProjectData(m_projectData); // validate() 增加时间残差
  m_validationWf->setResidualThresholdMs(10.0);  // autoplan §5C：D61 残差阈值 10 ms
  m_versionStore = new MapVersionStore(QString());
  m_versionCtl = new MapVersionController(m_versionStore, m_layerSvc, this);
  m_versionCtl->setEditingService(m_editSvc);
  m_versionCtl->setProjectStore(m_store);
  // releaseHorizon 遇编辑中图层时经服务回滚（busy 随会话释放）——与
  // versionCtl 同一编辑服务实例。
  m_layerSvc->setEditingService(m_editSvc);

  // ensureManifest-on-open: first point a per-project path is derivable.
  connect(m_projectSvc, &QgisProjectService::projectOpened, this,
          [this](const QString &qgzPath) {
            const QFileInfo fi(qgzPath);
            m_projectLock = std::make_unique<ProjectDirLock>(fi.absolutePath());
            QString lockErr;
            const bool lockRefused = !m_projectLock->tryLock(&lockErr);
            if (lockRefused)
            {
              qWarning() << "AppContext: project lock refused:" << lockErr;
              QgsMessageLog::logMessage(
                  tr("工程已被另一个实例锁定（%1），当前以只读模式打开——"
                     "导入/保存/图层清单/版本登记都会被拒绝")
                      .arg(lockErr),
                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);

              const bool isHeadless = (QGuiApplication::platformName() == QStringLiteral("offscreen") ||
                                       QGuiApplication::platformName() == QStringLiteral("minimal") ||
                                       !qobject_cast<QApplication*>(QCoreApplication::instance()));
              if (!isHeadless)
              {
                QMessageBox box;
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(tr("工程已被另一个实例锁定"));
                box.setText(tr("工程目录正被另一个 Paleo 实例编辑：\n%1\n\n同一工程同时只允许一个写实例。\n是否以只读模式继续打开？").arg(lockErr));
                auto *btnReadOnly = box.addButton(tr("只读打开"), QMessageBox::AcceptRole);
                auto *btnCancel = box.addButton(tr("取消"), QMessageBox::RejectRole);
                box.setDefaultButton(btnReadOnly);
                box.exec();
                if (box.clickedButton() == btnCancel)
                {
                  QMetaObject::invokeMethod(this, [this]() {
                    if (m_projectSvc && m_projectSvc->project())
                      m_projectSvc->project()->clear();
                    closeProject();
                  }, Qt::QueuedConnection);
                  return;
                }
              }
            }
            // T4 单写实例降级：锁失败 = 真只读。四个落盘面全部接线——
            // catalog（save/mutator 回滚）、工程存储（gpkg/qgz/journal）、
            // 图层清单（manifest sqlite）、版本库（map_versions）。读面照常。
            const bool writable = m_projectLock->isHeld();
            m_store->setReadOnly(!writable);
            if (DataCatalog *cat = m_import->catalog())
              cat->setLockedReadOnly(!writable); // 实例级模式，open() 不清除
            // 广播去重：同工程重复打开（理论不可达）不重复弹。
            if (m_lastReadOnlyNotified != !writable)
            {
              m_lastReadOnlyNotified = !writable;
              emit projectReadOnlyChanged(!writable);
            }
            const QString metaPath = manifestPathFor(qgzPath);
            m_store->setProjectPaths(qgzPath, gpkgPathFor(qgzPath), metaPath);

            // data/commit-coord：提交 journal 恢复扫描。上次 commitAll 若在
            // catalog→qgz 写序中途崩溃会留下未 complete 的记档；如实上报
            // （qWarning + QGIS 消息日志 → 状态面），绝不自动重放——记档
            // 摘要不足以证明重跑安全，操作者重新保存即以同 opId 续跑。
            const QVector<PaleoProjectStore::CommitOp> unfinishedCommits =
                m_store->recoverCommitJournal();
            for (const PaleoProjectStore::CommitOp &op : unfinishedCommits)
            {
              qWarning() << "AppContext: unfinished commit op" << op.opId
                         << "at stage" << op.stage;
              QgsMessageLog::logMessage(
                  tr("检测到未完成的提交 %1（阶段：%2）——上次保存可能中途崩溃，"
                     "请检查数据后重新保存")
                      .arg(op.opId, op.stage),
                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            }

            // complete 记档是幂等重入凭证、只增不减——打开路径顺带封顶清理
            // （默认留 32 条）。未完成记档不清理，上面已如实上报。
            m_store->pruneCommitJournal();

            // §37 recovery: the SQLite manifest is authoritative, but the .qgz
            // carries an embedded copy of the declared set. Rehydrate ONLY when
            // the store file does not exist — a .qgz moved/shared without its
            // sidecar must not lose its layer set, while an existing store
            // (even a deliberately emptied one) is never overwritten.
            const bool metaExisted = QFileInfo::exists(metaPath);

            *m_manifest = LayerManifest(metaPath); // rebind in place
            m_manifest->setReadOnly(!writable); // 值重绑带回可写默认——重设
            QString err;
            if (!m_manifest->open(&err))
              qWarning() << "AppContext: failed to open layer manifest" << metaPath << err;

            if (!metaExisted && !writable)
            {
              // 只读实例不做 rehydrate 写入（.qgz 内嵌清单本就来自第一实例
              // 的 manifest；这里写入既越权也会在 upsert 门上如实失败）。
              qWarning() << "AppContext: read-only instance skips manifest rehydrate";
            }
            else if (!metaExisted)
            {
              const QVector<LayerDeclaration> embedded =
                  ManifestProjection::extractDeclarations(m_projectSvc->project());
              for (const LayerDeclaration &d : embedded)
              {
                LayerDeclaration copy = d;
                copy.instantiated = false; // runtime flag — layer service decides
                if (!m_manifest->upsert(copy, &err))
                  qWarning() << "AppContext: manifest rehydrate failed for" << d.layerId << err;
              }
              if (!embedded.isEmpty())
                qInfo() << "AppContext: manifest store missing — restored"
                        << embedded.size() << "declarations from .qgz projection";
            }

            m_styleSvc->setStylesRoot(fi.absoluteDir().filePath(QStringLiteral("styles")));
            // 工程级参数（AREA_PARAMETERS）：project_area.json → 层位名单/
            // 分类规则/SEG-Y 偏移/ONNX 网格/标定层位。必须在 importSvc/
            // catalog 之前装载——分类器与层位词表消费 active()。坏 JSON
            // 如实上报（active 保持默认），不静默。
            AreaRules::setProjectDir(fi.absolutePath());
            if (!AreaRules::lastError().isEmpty())
            {
              qWarning() << "AppContext: area rules rejected:" << AreaRules::lastError();
              QgsMessageLog::logMessage(
                  tr("工程参数文件 project_area.json 被拒用（%1）——本工程按内置"
                     "默认参数运行")
                      .arg(AreaRules::lastError()),
                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            }
            m_import->setProjectDir(fi.absolutePath());
            // wave4/崩溃报告：报告头的「当前工程路径」随工程打开更新（落点
            // 不变——AppData 下，脏退出检测要求先于工程存在）。
            CrashReport::setProjectContext(fi.absolutePath());

            // T26（wave3/derived-publish）：四个写出派生产物的 workflow 绑上
            // importSvc 持有的同一 catalog 实例（catalog.json 整文件重写，两
            // 个实例交错写会互相覆盖）——厚度/预测/IDW/融合/相多边形从此落
            // artifacts/derived/ 并登记 DERIVED 版本，不再有 /tmp 死链。
            if (DataCatalog *derivedCatalog = m_import->catalog())
            {
              m_mappingWf->setCatalog(derivedCatalog, fi.absolutePath());
              m_predictionWf->setCatalog(derivedCatalog, fi.absolutePath());
              m_constraintWf->setCatalog(derivedCatalog, fi.absolutePath());
              m_compositionWf->setCatalog(derivedCatalog, fi.absolutePath());
              m_mappingWorkbench->bindCatalog(derivedCatalog, fi.absolutePath());
            }
#if PALEO_HAVE_ORT
            // onnx:* 模型按层位钉在 <工程目录>/models/*.onnx。
            if (m_onnxSvc)
              m_onnxSvc->setModelRoot(fi.absoluteDir().filePath(QStringLiteral("models")));
#endif

            // wave/mapping-pipeline：版本存储重绑到本工程 meta 库；读侧门面
            // 接上数据底座的 catalog（<工程目录>/artifacts/metadata/catalog.json）。
            *m_versionStore = MapVersionStore(metaPath);
            m_versionStore->setReadOnly(!writable); // 值重绑带回可写默认——重设
            QString versionErr;
            if (!m_versionStore->open(&versionErr))
              qWarning() << "AppContext: map version store open failed" << metaPath << versionErr;
            QString facadeErr;
            if (!m_projectData->setProjectDir(fi.absolutePath()))
              qWarning() << "AppContext: project data facade:" << m_projectData->lastError();
            m_projectData->setManifest(m_manifest);

            // §4 井位图层（预览壳重排）：catalog 已随 setProjectDir 打开——
            // 井点写 GeoJSON、声明「wells」、实例化后绑给 WellMapLink。
            m_projectDir = fi.absolutePath();
            refreshWellsLayer(false); // 打开时视野归 .qgz 恢复态，不抢

            // goal/fault-interpretation：FaultSet 存储值重绑本工程 meta 库并
            // 读回；工程有断层时切割镜像层上图（内存层由模型整刷）。
            // catalog 角色：有 seismic_survey 实体及其地震体资产时挂上下文
            //（首次写断层时 ensure "fault" 角色链接，幂等）。
            m_faultStore = FaultSetStore(metaPath, m_store);
            m_faultStore.setReadOnly(!writable); // 值重绑带回可写默认——重设
            QString faultStoreErr;
            if (!m_faultStore.open(&faultStoreErr))
              qWarning() << "AppContext: fault set store open failed" << metaPath
                         << faultStoreErr;
            QString faultErr;
            if (!m_faultCtl->reload(&faultErr))
              qWarning() << "AppContext: fault set reload failed" << faultErr;
            if (m_import && m_import->catalog()) {
              DataCatalog *cat = m_import->catalog();
              const auto surveys = cat->entities(QStringLiteral("seismic_survey"));
              if (!surveys.isEmpty()) {
                const QString surveyId = surveys.first().id;
                QString volumeAssetId;
                for (const EntityAssetLink &l : cat->linksForEntity(surveyId)) {
                  if (l.role == QLatin1String("seismic_volume") && !l.assetId.isEmpty()) {
                    volumeAssetId = l.assetId;
                    break;
                  }
                }
                if (!volumeAssetId.isEmpty())
                  m_faultCtl->setCatalogContext(cat, surveyId, volumeAssetId);
              }
            }
            if (m_faultCtl->faultSet().faultCount() > 0)
              m_faultCtl->ensureMapLayer(m_projectSvc->project()->crs().authid());
          });
  StartupTrace::mark(QStringLiteral("services_ready")); // 服务装配完（簇1 仪表）
}

bool AppContext::isProjectReadOnly() const
{
  return m_projectLock && !m_projectLock->isHeld();
}

void AppContext::closeProject()
{
  m_projectLock.reset();
  m_lastReadOnlyNotified = false;
  if (m_store)
    m_store->setReadOnly(false);
  if (m_import)
  {
    if (DataCatalog *cat = m_import->catalog())
      cat->setLockedReadOnly(false);
  }
  emit projectReadOnlyChanged(false);
}

void AppContext::refreshWellsLayer(bool zoomOnGrowth)
{
  if (m_projectDir.isEmpty() || !m_import || !m_layerSvc)
    return;
  DataCatalog *cat = m_import->catalog();
  if (!cat)
    return;

  // 只读实例（T4）：不写 wells.geojson、不 upsert manifest——第一实例的
  // 产物已在位时直接按既有声明实例化；声明缺失（从未有写实例跑过）则
  // 如实无 wells 图层，静默降级。
  const bool readOnly = m_projectLock && !m_projectLock->isHeld();

  // catalog 井实体（有 surface 坐标者）→ artifacts/layers/wells.geojson。
  // 坐标是工程网格局部米（writeWellsGeoJson 写同一 WKT，不投 4326）。
  const QString wellsPath = QDir(m_projectDir).filePath(
      QStringLiteral("artifacts/layers/wells.geojson"));
  if (!readOnly)
  {
    QString werr;
    if (!cat->writeWellsGeoJson(wellsPath, &werr))
    {
      qWarning() << "AppContext: wells geojson write failed:" << werr;
      return;
    }
    if (!QFile::exists(wellsPath))
      return; // 没有可定位的井——不声明空图层（与 writeWellsGeoJson 同一约定）

    LayerDeclaration decl;
    decl.layerId = QStringLiteral("wells");
    decl.type = QStringLiteral("vector");
    decl.source = wellsPath;
    decl.group = QStringLiteral("00_Data");
    decl.title = tr("井位");
    QString derr;
    if (!m_layerSvc->declare(decl, &derr)) // upsert——重复声明安全
    {
      qWarning() << "AppContext: wells layer declare failed:" << derr;
      return;
    }
  }
  else if (!QFile::exists(wellsPath))
  {
    return; // 只读且无可复用产物——不造数据，也不算失败
  }
  QString ierr;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layerSvc->instantiate(QStringLiteral("wells"), &ierr));
  if (!layer)
  {
    qWarning() << "AppContext: wells layer instantiate failed:" << ierr;
    return;
  }
  layer->reload();         // 文件可能刚被重写——数据源重读要素
  QgisStyleService::applyWellLayerStyle(layer);
  // C3（wave/deepen-perf）：井头带类别字段时按 Q/HS 1011—2016 表 K.1 十二类
  // 探井分类渲染；无字段保持通用「探井」符号——不造假类别。
  for (const QString &field : {QStringLiteral("well_class"),
                               QStringLiteral("well_category"),
                               QStringLiteral("category"),
                               QStringLiteral("类别")})
  {
    if (layer->fields().lookupField(field) >= 0)
    {
      QgisStyleService::applyWellCategoryStyle(layer, field);
      break;
    }
  }
  layer->triggerRepaint();
  if (m_wellLink)
    m_wellLink->setWellLayer(layer, QStringLiteral("id"));

  // D6 zoom-to-content：井点范围实际变化（导入/补挂出新井）才把视野拉到
  // wells 层——无关 catalog 变更重写同一 geojson，extent 不变不打扰用户。
  const QgsRectangle ext = layer->extent();
  if (ext != m_lastWellsExtent)
  {
    m_lastWellsExtent = ext;
    if (zoomOnGrowth && !ext.isEmpty() && m_canvasCtl)
      m_canvasCtl->zoomToLayer(QStringLiteral("wells"));
  }
}

AppContext::~AppContext()
{
  // Services embed Qgs* objects whose destruction can touch providers —
  // they must die while the runtime is still up. They are all direct
  // children; delete them explicitly before shutdown instead of leaving
  // them to ~QObject (which runs after this body).
  const QObjectList kids = children();
  for (QObject *kid : kids)
    delete kid;

  delete m_manifest; // not a QObject — plain path-holding value type
  m_manifest = nullptr;
  delete m_versionStore; // same idiom as the manifest (wave/mapping-pipeline)
  m_versionStore = nullptr;
  m_projectLock.reset();

  if (s_runtimeOwners.remove(this))
    QgisRuntime::shutdown();
}
