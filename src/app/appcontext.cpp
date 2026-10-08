// 层：组装根
#include "appcontext.h"
#include "../qgis/wellattributestore.h"
#include "../services/errorhub.h"                 // 方向64：统一错误通道
#include "aiwiring.h"                          // 方向51：远端预测装配（唯一入口）
#include "../ai/remotepredictconfig.h"
#include "../ai/chat/llmclient.h"              // 方向51：LLM 配置 + 助手编排
#include "../ai/chat/llmkeystore.h"
#include "../workflow/aichatcontroller.h"
#include "../workflow/mappingworkbench.h"
#include "../workflow/welltrajectorylayer.h" // goal/well-trajectory 轨迹线层组装

#include "../ai/onnxpredictionservice.h" // ORT-free header; instantiation is PALEO_HAVE_ORT-guarded
#if PALEO_HAVE_ORT
#include "../ai/modelregistry.h" // 注册表如实扫描（未装模型降级）
#include "../workflow/aiassistworkflow.h"
#endif
#include "../qgis/qgisruntime.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerorganizer.h" // 图层树布局器：置顶共享 + 层位组
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../services/toolavailability.h"
#include "../services/paleotaskservice.h"
#include "../services/scriptrunner.h"  // 方向68：Python 脚本运行服务
#include "../services/pythonrepl.h"    // 方向68：REPL 会话桥（实验性）
#include "../workflow/pythonconsolecontroller.h" // 方向68：脚本面编排
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
#include "../workflow/wellsitingworkflow.h"
#include "../workflow/faultinterpretationcontroller.h" // goal/fault-interpretation
#include "../services/projectdata.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/depthconversionworkflow.h"
#include "../workflow/propertymodelworkflow.h"
#include "../workflow/faciesmappingworkflow.h"
#include "../metadata/mapversionstore.h"
#include "../metadata/metastore.h"
#include "../workflow/mapversioncontroller.h"

#include <QApplication>
#include <QThread>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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
#include <qgsfillsymbol.h>
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
  // 图层树布局器：声明驱动摆放（测区/测井置顶共享，层位构成组）。须在
  // 首个 instantiate 之前建好——legendLayersAdded 归位与图层上图同步发生。
  m_layerOrganizer = new QgisLayerOrganizer(m_projectSvc, m_layerSvc, this);

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
  // 方向64：统一错误通道——视图层 PaleoNotify 经 ErrorHub::global() 入账。
  m_errorHub = new ErrorHub(this);
  ErrorHub::installGlobal(m_errorHub);
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
  connect(m_import->catalog(), &DataCatalog::changed, this, [this] {
    refreshWellsLayer(true);
    refreshSurveyLayer();
    refreshWellTrajectoriesLayer();
    // 方向77：catalog 变更（导入/登记）刷新工程摘要常驻段——system prompt
    // 里的计数不留在旧值（工具实查永远最新，这段只是开局指北针）。
    if (m_aiChat)
      m_aiChat->setProjectBrief(AiChatToolRunner::projectBrief(
        m_import->catalog(), m_projectDir));
  });

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
  // AI 辅助编排（tile 分类产品/追踪建议）：推理服务 + 任务池都在场时即刻
  // 绑定；catalog 通道随工程打开补绑（同 predictionWf 的 T26 位点）。
  m_aiAssistWf = new AiAssistWorkflow(m_layerSvc, this);
  m_aiAssistWf->setOnnxService(m_onnxSvc);
  if (m_taskSvc)
    m_aiAssistWf->setTaskService(m_taskSvc);
#endif
  m_constraintWf = new ConstraintWorkflow(m_procSvc, m_layerSvc, this);
  m_constraintWf->setStore(m_store); // GeoPackage constraint persistence (wave/constraint-gpkg)
  m_mappingWorkbench = new MappingWorkbench(m_layerSvc, m_procSvc, m_projectSvc, m_constraintWf, this);
  // 方向51：远端预测装配—— MappingWorkbench 自己的那份替身 Mock 已删除，
  // 这里显式装入 RemotePredictionRouter。端点未配置时 router 不带传输，
  // 任何预测请求如实失败；UI 侧显示「远端预测未配置，走本地引擎」。
  {
    const RemotePredictConfig remoteConfig = RemotePredictConfig::load();
    const RemotePredictionAssembly assembly = installRemotePrediction(
        m_mappingWorkbench, remoteConfig, m_onnxSvc, QString(), this);
    m_remotePredict = assembly.router;
    m_remotePredictHint = assembly.statusHint;
  }
  // 方向51：AI 对话助手编排。端点/模型从用户配置读，密钥由系统钥匙串异步
  // 补齐——补齐前是禁用态（UI 显示禁用原因），不静默跑假回答。
  m_aiChat = new AiChatController(this);
  // 方向61/77：工具执行回路装配（tile 分类起步；工程打开处随 AreaRules 重绑）。
  // 只读工程工具（query/lineage）不依赖 ORT——无 ORT 构建也绑（assist 为空
  // 时 tile/horizon 由执行器按调用如实报错）。
  bindChatToolRunner(m_aiChat, m_aiAssistWf, m_layerSvc,
                     m_import->catalog(), QString());
  {
    LlmConfig llm = LlmConfig::load();
    LlmKeyStore::read(this, [this, llm](bool ok, const QByteArray &key,
                                        const QString &) mutable {
      if (ok)
        llm.apiKey = key;
      if (m_aiChat)
        m_aiChat->setConfig(llm);
    });
  }
  // 方向68：Python 脚本面三件。脚本执行恒为显式用户动作（控制台面板
  // 触发）；无沙箱，脚本安全性用户自担（面板提示 +
  // tools/reference/scripts/README.md 同步声明）。
  m_scriptRunner = new ScriptRunnerService(this);
  m_pythonRepl = new PythonReplSession(this);
  m_pythonConsole = new PythonConsoleController(m_scriptRunner, m_pythonRepl, this);
  m_compositionWf = new CompositionWorkflow(m_procSvc, m_layerSvc, this);
  m_validationWf = new ValidationWorkflow(m_layerSvc, m_store, this);

  // 方向34：井网辅助编排。约束/断层避让面经 provider 注入（随工程打开
  // 刷新）；planned 计划井入 catalog（entityType=="planned"）。
  m_wellsitingWf = new WellSitingWorkflow(m_layerSvc, m_store, this);
  m_wellsitingWf->setConstraintProvider([this]() -> QList<SitingConstraintGeometry> {
    QList<SitingConstraintGeometry> out;
    if (!m_constraintWf)
      return out;
    for (const QVariantMap &row : m_constraintWf->loadConstraints()) {
      SitingConstraintGeometry g;
      g.wkt = row.value(QStringLiteral("wkt")).toString();
      g.type = row.value(QStringLiteral("type")).toString();
      if (!g.wkt.isEmpty())
        out.append(g);
    }
    return out;
  });
  m_wellsitingWf->setFaultCutsProvider([this]() -> QStringList {
    QStringList out;
    if (!m_faultCtl)
      return out;
    for (const auto &fault : m_faultCtl->faultSet().faults())
      for (const auto &cut : fault.cuts)
        if (!cut.wkt.isEmpty())
          out.append(cut.wkt);
    return out;
  });

  // wave/mapping-pipeline 阶段C+E — 读侧门面 / D61 编图链 / 版本状态机。
  // catalog.json 由数据底座包落位；这里只在工程目录里发现它时绑定（未合
  // 并期间手工放置合成 catalog 亦可驱动整条链）。版本存储与 LayerManifest
  // 同库（meta sqlite），路径在 projectOpened 时原地重绑。
  m_projectData = new ProjectDataFacade(this);
  m_mappingWf = new MappingWorkflow(m_constraintWf, m_compositionWf, m_layerSvc, this);
  m_depthWf = new DepthConversionWorkflow(nullptr, QString(), this); // catalog 绑定随工程打开
  m_propModelWf = new PropertyModelWorkflow(nullptr, QString(), this); // catalog 绑定随工程打开
  // goal/facies-automapping：catalog/约束库绑定随工程打开（rebind 块）。
  m_faciesMappingWf = new FaciesMappingWorkflow(nullptr, QString(), this);
  m_faciesMappingWf->attachLayerService(m_layerSvc);
  m_mappingWf->setProjectData(m_projectData);
  m_validationWf->setProjectData(m_projectData); // validate() 增加时间残差
  m_wellsitingWf->setProjectData(m_projectData);   // 方向34：层位井控密度走 tops
  m_validationWf->setResidualThresholdMs(10.0);  // autoplan §5C：D61 残差阈值 10 ms
  m_versionStore = new MapVersionStore(QString());
  m_versionCtl = new MapVersionController(m_versionStore, m_layerSvc, this);
  m_versionCtl->setEditingService(m_editSvc);
  m_versionCtl->setProjectStore(m_store);
  // releaseHorizon 遇编辑中图层时经服务回滚（busy 随会话释放）——与
  // versionCtl 同一编辑服务实例。
  m_layerSvc->setEditingService(m_editSvc);

  // #152/#153 工程生命周期中枢：
  //   * 打开闸门：锁/只读决策在 QgisProjectService 读新工程**之前**做——锁冲突
  //     时用户点「取消」，旧工程（含它的锁、路径、图层）原样保留；
  //   * projectAboutToClose：旧工程仍完整时开新任务会话（在途任务全部取消，
  //     JobRunner/finished 槽据 Cancelled 丢弃旧工程结果）；面板各自接同一信号
  //     清工程作用域状态（PaleoMainWindow::resetProjectScopedState）；
  //   * projectClosed：释放锁与 sqlite 句柄、回落可写默认。
  m_projectSvc->setOpenGate([this](const QString &projectDir, bool creating, QString *error,
                                   bool *cancelled) {
    return acquireProjectLock(projectDir, creating, error, cancelled);
  });
  connect(m_projectSvc, &QgisProjectService::projectAboutToClose, this, [this]() {
    if (m_taskSvc)
      m_taskSvc->beginNewSession();
  });
  connect(m_projectSvc, &QgisProjectService::projectClosed, this,
          [this]() { releaseProjectSession(); });
  connect(m_projectSvc, &QgisProjectService::openAborted, this, [this] {
    m_pendingLock.reset();
    m_pendingLockReuse = false;
  });

  // ensureManifest-on-open: first point a per-project path is derivable.
  connect(m_projectSvc, &QgisProjectService::projectOpened, this,
          [this](const QString &qgzPath) {
            // #152：锁已由打开闸门（acquireProjectLock）在 read() 之前取好；
            // 这里只接管。同目录重开时闸门复用现有锁（QLockFile 同进程不可
            // 重入，先放后取会在窗口期丢锁）。无闸门结果（理论不可达）时
            // 兜底按旧语义就地取锁。
            const QFileInfo fi(qgzPath);
            if (m_pendingLock)
              m_projectLock = std::move(m_pendingLock);
            else if (!m_pendingLockReuse || !m_projectLock)
            {
              auto lock = std::make_unique<ProjectDirLock>(fi.absolutePath());
              QString lockErr;
              if (!lock->tryLock(&lockErr))
                qWarning() << "AppContext: project lock refused:" << lockErr;
              m_projectLock = std::move(lock);
            }
            m_pendingLockReuse = false;
            // T4 单写实例降级：锁失败 = 真只读。四个落盘面全部接线——
            // catalog（save/mutator 回滚）、工程存储（gpkg/qgz/journal）、
            // 图层清单（manifest sqlite）、版本库（map_versions）。读面照常。
            const bool writable = m_projectLock->isHeld();
            m_store->setReadOnly(!writable);
            m_projectSvc->setReadOnly(!writable);
            if (DataCatalog *cat = m_import->catalog())
              cat->setLockedReadOnly(!writable); // 实例级模式，open() 不清除
            // 广播去重：同工程重复打开（理论不可达）不重复弹。
            if (m_lastReadOnlyNotified != !writable)
            {
              m_lastReadOnlyNotified = !writable;
              emit projectReadOnlyChanged(!writable);
            }
            // #80：先关上一会话（旧工程或同路径旧会话）在本线程的 sqlite 连接
            // ——切换工程不留句柄；同路径重开不复用可能指向旧 inode 的连接。
            // 各 store 的连接都是按调用懒开，关掉后下次访问自动重建。
            if (!m_metaPath.isEmpty())
              MetaStore::closeConnectionsFor(m_metaPath);
            const QString metaPath = manifestPathFor(qgzPath);
            m_metaPath = metaPath;
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
            // 工程级地理配准（project.paleo georeference 节）：导入服务带上
            // 局部网格→WGS84 变换——建井时写 coordinateStatus=ok + extra
            // 经纬度；无配准工程保持 untransformed 现状。
            if (m_projectSvc->georeference())
              m_import->setGeoreference(*m_projectSvc->georeference());
            else
              m_import->clearGeoreference();
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
              WellAttributeStore::open(derivedCatalog, fi.absolutePath(), m_layerSvc, false, false);
              WellAttributeStore::open(derivedCatalog, fi.absolutePath(), m_layerSvc, true, false);
#if PALEO_HAVE_ORT
              if (m_aiAssistWf)
                m_aiAssistWf->setCatalog(derivedCatalog, fi.absolutePath());
#endif
              // 方向61/77：工程打开 → 重绑聊天工具上下文（AreaRules 按工区钉
              // targetHorizon，层位名与栅格声明都可能换了；只读工程工具的
              // catalog/projectDir 同步换绑）。无 ORT 构建也重绑（assist 为空）。
              bindChatToolRunner(m_aiChat, m_aiAssistWf, m_layerSvc,
                                 derivedCatalog, fi.absolutePath());
              // goal/time-depth-velocity：同一 catalog 实例纪律（整文件重写，
              // 交错写互覆）——层深转换产物落 artifacts/derived/。
              m_depthWf->rebind(derivedCatalog, fi.absolutePath());
              m_propModelWf->rebind(derivedCatalog, fi.absolutePath());
              // goal/facies-automapping：同 catalog 实例纪律；约束库走
              // ConstraintWorkflow 的共享 store（单写者）。
              m_faciesMappingWf->rebind(derivedCatalog, fi.absolutePath());
              m_faciesMappingWf->setConstraintStore(m_constraintWf->constraintStore());
            }
#if PALEO_HAVE_ORT
            // onnx:* 模型按层位钉在 <工程目录>/models/*.onnx。
            if (m_onnxSvc)
            {
              const QString modelsDir =
                fi.absoluteDir().filePath(QStringLiteral("models"));
              m_onnxSvc->setModelRoot(modelsDir);
              // 模型注册表如实扫描（范围5）：未装模型静默降级；manifest 在而
              // 坏/缺文件/指纹不符 → 消息日志逐条说明，不报错轰炸。
              const ModelRegistryScan registry = ModelRegistry::scan(modelsDir);
              // #145：扫描结果门控模型可见性与加载（只放行 status==Ok；加载时复核钉哈希）。
              m_onnxSvc->setModelRegistry(registry);
              if (!registry.manifestFound)
                QgsMessageLog::logMessage(
                  tr("未装模型：%1 无 manifest.json——AI 辅助按无模型降级").arg(modelsDir),
                  QStringLiteral("Paleo"));
              if (!registry.manifestError.isEmpty())
                QgsMessageLog::logMessage(registry.manifestError, QStringLiteral("Paleo"),
                                          Qgis::Critical);
              for (const ModelRegistryEntry &e : registry.entries)
                if (e.status != ModelRegistryEntry::Status::Ok)
                  QgsMessageLog::logMessage(
                    tr("模型 %1: %2 (%3)").arg(e.name, ModelRegistry::statusLabel(e.status), e.detail),
                    QStringLiteral("Paleo"), Qgis::Warning);
            }
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
            m_layerSvc->refreshBasemaps();
            refreshSurveyLayer();
            refreshWellsLayer(false); // 打开时视野归 .qgz 恢复态，不抢
            refreshWellTrajectoriesLayer();

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

            // 方向34：布井方案集存储值重绑本工程 meta 库；编排器接上
            // catalog（planned 实体）与 store。
            m_wellsitingStore = WellSitingStore(metaPath, m_store);
            m_wellsitingStore.setReadOnly(!writable); // 值重绑带回可写默认——重设
            QString sitingStoreErr;
            if (!m_wellsitingStore.open(&sitingStoreErr))
              qWarning() << "AppContext: well siting store open failed" << metaPath
                         << sitingStoreErr;
            m_wellsitingWf->setSitingStore(&m_wellsitingStore);
            if (m_import && m_import->catalog())
              m_wellsitingWf->setCatalog(m_import->catalog(), fi.absolutePath());
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
              m_faultCtl->ensureMapLayer(DataCatalog::localGridCrsWkt());

            // 图层树规整：manifest/目录都重绑完后跑一次全量归位——旧工程
            // 的平铺树收成「置顶共享 + 层位组」，未实例化层位补齐占位组。
            if (m_layerOrganizer)
              m_layerOrganizer->reorganize();
          });
  StartupTrace::mark(QStringLiteral("services_ready")); // 服务装配完（簇1 仪表）
  connect(m_projectSvc, &QgisProjectService::mapConfigurationChanged, this, [this] {
    if (m_projectSvc->georeference()) m_import->setGeoreference(*m_projectSvc->georeference());
    else m_import->clearGeoreference();
    m_layerSvc->refreshBasemaps();
  });
}

bool AppContext::isProjectReadOnly() const
{
  return m_projectLock && !m_projectLock->isHeld();
}

bool AppContext::acquireProjectLock(const QString &projectDir, bool creating, QString *error,
                                    bool *cancelled)
{
  m_pendingLock.reset();
  m_pendingLockReuse = false;
  auto candidate = std::make_unique<ProjectDirLock>(projectDir);
  if (m_projectLock && m_projectLock->isHeld() &&
      m_projectLock->lockPath() == candidate->lockPath())
  {
    m_pendingLockReuse = true; // 同目录重开：沿用已持有的锁
    return true;
  }
  QString lockErr;
  if (candidate->tryLock(&lockErr))
  {
    m_pendingLock = std::move(candidate);
    return true;
  }
  if (creating)
  {
    if (error)
      *error = tr("工程目录已被另一个实例锁定（%1），创建被拒绝").arg(lockErr);
    return false;
  }

  qWarning() << "AppContext: project lock refused:" << lockErr;
  bool readOnly = true; // 无头/测试：自动只读降级（既有语义）
  if (m_lockConflictResolver)
    readOnly = m_lockConflictResolver(lockErr);
  else
  {
    const bool isHeadless = (QGuiApplication::platformName() == QStringLiteral("offscreen") ||
                             QGuiApplication::platformName() == QStringLiteral("minimal") ||
                             !qobject_cast<QApplication *>(QCoreApplication::instance()));
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
      readOnly = box.clickedButton() != btnCancel;
    }
  }
  if (!readOnly)
  {
    // 取消：什么都不动——不读新工程、不发 projectAboutToClose，当前工程与
    // 它的锁原样保留（#152 旧实现此时已释放旧锁并清空工程）。
    if (cancelled)
      *cancelled = true;
    if (error)
      *error = tr("已取消打开：工程被另一个实例锁定（%1）").arg(lockErr);
    return false;
  }
  QgsMessageLog::logMessage(
      tr("工程已被另一个实例锁定（%1），当前以只读模式打开——"
         "导入/保存/图层清单/版本登记都会被拒绝")
          .arg(lockErr),
      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
  m_pendingLock = std::move(candidate); // 未持有 = 只读
  return true;
}

void AppContext::closeProject()
{
  // 经工程服务关闭：发 projectAboutToClose（面板清状态、在途任务取消）→
  // 清 QgsProject → projectClosed → releaseProjectSession()。工程服务已无
  // 工程时直接释放会话资源（幂等）。
  if (m_projectSvc && !m_projectSvc->projectPath().isEmpty())
    m_projectSvc->closeProject();
  else
    releaseProjectSession();
}

void AppContext::releaseProjectSession()
{
  m_pendingLock.reset();
  m_pendingLockReuse = false;
  // #80：关闭工程即释放本线程持有的 project.sqlite 句柄（Windows 上旧句柄
  // 会阻止删除/移动刚关闭的工程目录）。
  if (!m_metaPath.isEmpty())
  {
    MetaStore::closeConnectionsFor(m_metaPath);
    m_metaPath.clear();
  }
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

void AppContext::refreshWellTrajectoriesLayer()
{
  if (m_projectDir.isEmpty() || !m_import || !m_layerSvc || !m_projectData)
    return;
  DataCatalog *cat = m_import->catalog();
  if (!cat)
    return;
  const bool readOnly = m_projectLock && !m_projectLock->isHeld();

  const QString trajPath = QDir(m_projectDir).filePath(
      QStringLiteral("artifacts/layers/well_trajectories.geojson"));
  if (!readOnly)
  {
    // 组装在功能层纯函数（wellTrajectoriesGeoJson，可测）；这里只落盘 +
    // 声明 + 上图。读坏测斜的井如实进日志（不阻断其余井的层产出）。
    QString terr;
    const QByteArray bytes = paleo::wellTrajectoriesGeoJson(m_projectData, cat, &terr);
    if (!terr.isEmpty())
      qWarning() << "AppContext: well trajectories skipped broken surveys:" << terr;
    const QByteArray finalBytes = bytes.isEmpty()
                                      ? QByteArray() // 无任何已决测斜
                                      : bytes;
    if (!finalBytes.isEmpty())
    {
      QDir().mkpath(QFileInfo(trajPath).absolutePath());
      QSaveFile file(trajPath);
      file.setDirectWriteFallback(false);
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        if (file.write(finalBytes) == finalBytes.size() && file.commit())
        {
          LayerDeclaration decl;
          decl.layerId = QStringLiteral("well_trajectories");
          decl.type = QStringLiteral("vector");
          decl.source = trajPath;
          decl.group = QStringLiteral("00_Data");
          decl.title = tr("井轨迹投影");
          QString derr;
          if (!m_layerSvc->declare(decl, &derr))
            qWarning() << "AppContext: well trajectories layer declare failed:" << derr;
        }
        else
        {
          qWarning() << "AppContext: well trajectories geojson write failed";
        }
      }
    }
    else if (QFile::exists(trajPath))
    {
      // 曾有轨迹、现已全部解除：覆写空 FeatureCollection——声明保留（无
      // undeclare 面），图层如实渲染零要素，不留旧轨迹假象。
      QJsonObject root;
      root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
      QJsonObject crsProps;
      crsProps.insert(QStringLiteral("name"), DataCatalog::localGridCrsWkt());
      QJsonObject crs;
      crs.insert(QStringLiteral("type"), QStringLiteral("name"));
      crs.insert(QStringLiteral("properties"), crsProps);
      root.insert(QStringLiteral("crs"), crs);
      root.insert(QStringLiteral("features"), QJsonArray());
      QSaveFile file(trajPath);
      file.setDirectWriteFallback(false);
      if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        const QByteArray emptyBytes = QJsonDocument(root).toJson();
        if (!(file.write(emptyBytes) == emptyBytes.size() && file.commit()))
          qWarning() << "AppContext: well trajectories geojson rewrite failed";
      }
    }
  }
  else if (!QFile::exists(trajPath))
  {
    return; // 只读且无可复用产物——不造数据，也不算失败
  }
  QString ierr;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layerSvc->instantiate(QStringLiteral("well_trajectories"), &ierr));
  if (!layer)
  {
    // 声明缺位（可写实例首跑前）或文件为空——不算失败，仅无层可上图。
    return;
  }
  layer->reload();
  QgisStyleService::applyTrajectoryLayerStyle(layer);
  layer->triggerRepaint();
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

void AppContext::refreshSurveyLayer()
{
  if (m_projectDir.isEmpty() || !m_import || !m_import->catalog() || !m_layerSvc) return;
  // 无有效测区（角点 ≥3）不落测区图层——空工程的地图/图层树保持空态
  //（T31 契约）；测区被移除时把此前的占位层一并撤下。
  bool hasSurvey = false;
  for (const auto &survey : m_import->catalog()->entities(QStringLiteral("seismic_survey")))
    if (survey.corners.size() >= 3) { hasSurvey = true; break; }
  if (!hasSurvey) {
    QString error;
    m_layerSvc->removeDeclaration(QStringLiteral("survey.area"), &error);
    return;
  }
  const QString path = QDir(m_projectDir).filePath(QStringLiteral("artifacts/layers/survey_area.geojson"));
  QString error;
  if (!isProjectReadOnly()) {
    if (!m_import->catalog()->writeSurveyGeoJson(path, &error)) {
      qWarning() << "Survey layer:" << error; return;
    }
    LayerDeclaration declaration;
    declaration.layerId = QStringLiteral("survey.area");
    declaration.type = QStringLiteral("vector");
    declaration.source = path;
    declaration.group = QStringLiteral("00_Data");
    declaration.title = tr("测区范围");
    if (!m_layerSvc->declare(declaration, &error)) {
      qWarning() << "Survey declaration:" << error; return;
    }
  }
  if (auto *layer = qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(QStringLiteral("survey.area"), &error))) {
    layer->reload();
    // 原生符号保持透明内部，矩形范围和底图同时可读。
    auto symbol = QgsFillSymbol::createSimple({{"color", "transparent"}, {"outline_color", "27,115,208,255"},
                                               {"outline_style", "dash"}, {"outline_width", "0.5"}});
    layer->setRenderer(new QgsSingleSymbolRenderer(symbol.release()));
    layer->triggerRepaint();
  }
}

bool AppContext::drainTasks(int timeoutMs)
{
  if (!m_taskSvc)
    return true;
  const bool ok = m_taskSvc->shutdown(timeoutMs);
  if (!ok)
    qWarning("AppContext: task workers did not drain before teardown");
  return ok;
}

AppContext::~AppContext()
{
  // Services embed Qgs* objects whose destruction can touch providers —
  // they must die while the runtime is still up. They are all direct
  // children; delete them explicitly before shutdown instead of leaving
  // them to ~QObject (which runs after this body).
  //
  // H-2：先排空任务池——worker 闭包捕获的是服务裸指针（DataImportService*、
  // workflow* 等），必须在任何依赖对象析构之前让 worker 全部退出。泵事件让
  // worker 排队回主线程的回包落地（导入链的旧 catInvoke BlockingQueued 已移除）。
  // 正常 GUI 退出时 main() 已先经 drainTasks() 在主窗口析构前排空（#163），
  // 这里是幂等兜底。
  drainTasks();
  // 再按创建的逆序删除：后建者（workflow/linkage）依赖先建者（store/services），
  // 依赖方先走。children() 是创建顺序，正序删除会让 m_taskSvc/m_store 先于
  // DataImportService 等依赖方析构。
  const QObjectList kids = children();
  for (auto it = kids.crbegin(); it != kids.crend(); ++it)
    delete *it;

  delete m_manifest; // not a QObject — plain path-holding value type
  m_manifest = nullptr;
  delete m_versionStore; // same idiom as the manifest (wave/mapping-pipeline)
  m_versionStore = nullptr;
  m_projectLock.reset();

  if (s_runtimeOwners.remove(this))
    QgisRuntime::shutdown();
}
