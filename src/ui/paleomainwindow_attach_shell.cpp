// 层：视图
// paleomainwindow_attach_shell — 壳面与全局组件接线（W4 拆分段）
#include "paleomainwindow.h"

#include "paleoicons.h"
#include "paleoribbon.h"
#include "notifications/errorhistorypanel.h"   // 方向64
#include "notifications/notificationcenter.h"  // 方向64
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../qgis/qgislayoutservice.h"
#include "../services/paleotaskservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "edittools/editingtoolbar.h"
#include "layout/mapbookpanel.h"
#include "layout/mapbookcontroller.h"
#include "layoutdesignershell.h"
#include "ai/aiassistdock.h"                    // 方向51：AI 助手 dock
#include "../workflow/aichatcontroller.h"
#include "../ai/chat/llmclient.h"               // LlmConfig::path()（配置说明用）

#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>
#include <qgslocatorwidget.h>
#include <qgslocator.h>
#include <qgslayout.h>
#include <qgsprintlayout.h>
#include <qgsapplication.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingregistry.h>

#include <QAction>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include "notifications/paleonotify.h"
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>

// ---------------------------------------------------------------------------
// 壳面接线：locator / 保存 / 底栏面板 / 处理算法 / 编辑工具 / 图件设计
// （W4 拆分段）——返回逻辑宿主编辑条供 buildRibbonPanels 镜像进 ribbon。
// ---------------------------------------------------------------------------
PaleoEditingToolbar *PaleoMainWindow::attachShellSurfaces(
    PaleoProjectStore *store, QgisProcessingService *procSvc,
    QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
    PaleoTaskService *taskSvc)
{
  m_editSvc = editSvc; // closeEvent 的保存/放弃编辑走服务（busy 挂账随终态清）
  // W2 长任务可见性：任务进场/终态时重估底栏露出（本体在主 TU，
  // syncBottomDockForTasks——露出/恢复都走程序化显隐，不动用户意愿）。
  if (taskSvc)
    connect(taskSvc, &PaleoTaskService::taskAdded, this, [this](PaleoTask *task) {
      if (task)
        connect(task, &PaleoTask::finished, this,
                [this] { syncBottomDockForTasks(); });
      syncBottomDockForTasks();
    });

  // ---- ribbon 右侧组的搜索槽 + 快速访问栏的保存 ----
  if (auto *topBar = findChild<QWidget *>(QStringLiteral("locatorSlot")))
  {
    if (m_layerSvc && m_selection && m_canvasCtl)
    {
      auto *locatorWidget = new QgsLocatorWidget(topBar);
      locatorWidget->setObjectName(QStringLiteral("paleoLocator"));
      locatorWidget->setMapCanvas(m_canvasCtl->canvas());
      locatorWidget->setPlaceholderText(tr("搜索井位/层位  Ctrl+K"));
      locatorWidget->setMinimumWidth(220);

      // Wells filter: resolve the declared "wells" layer lazily (instantiate
      // on demand — the search must not force materialization at open time).
      WellLocatorFilter::WellLayerProvider wellProvider = [this]() {
        QPair<QgsVectorLayer *, QString> out{nullptr, QString()};
        QgsMapLayer *l = m_layerSvc->layer(QStringLiteral("wells"));
        if (!l)
          l = m_layerSvc->instantiate(QStringLiteral("wells"));
        auto *vl = qobject_cast<QgsVectorLayer *>(l);
        if (!vl)
          return out;
        const int nameIdx = vl->fields().lookupField(QStringLiteral("name"));
        out.first = vl;
        out.second = nameIdx >= 0 ? QStringLiteral("name")
                                  : (vl->fields().isEmpty() ? QString()
                                                            : vl->fields().at(0).name());
        return out;
      };
      locatorWidget->locator()->registerFilter(
          new WellLocatorFilter(wellProvider, m_canvasCtl->canvas()));

      // Horizons filter: manifest horizon set → activate + materialize.
      HorizonLocatorFilter::HorizonListProvider horizonProvider = [this]() {
        QStringList hs;
        QVector<LayerDeclaration> declared;
        QString manifestErr;
        if (!m_layerSvc || !m_layerSvc->tryDeclared(&declared, &manifestErr))
        {
          QgsMessageLog::logMessage(tr("层位搜索：图层清单读取失败：%1")
                                        .arg(manifestErr),
                                    QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return hs;
        }
        for (const LayerDeclaration &d : declared)
          if (!d.horizon.isEmpty() && !hs.contains(d.horizon))
            hs << d.horizon;
        hs.sort();
        return hs;
      };
      HorizonLocatorFilter::ActivateFn activate = [this](const QString &h) {
        // 与 HorizonChipBar 同一拦截口径：编辑中切层位会在 releaseHorizon
        // 里静默回滚丢编辑成果（历史上还绕过 busy 释放）。定位器入口必须
        // 同样拒绝并说明原因，而不是开一条丢数据的旁路。
        QString editingName;
        if (m_layerSvc && m_layerSvc->isEditingAnyLayer(&editingName))
        {
          QgsMessageLog::logMessage(
              tr("正在编辑「%1」——先保存或放弃编辑，再切换层位（定位器切换已拒绝）")
                  .arg(editingName),
              QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        if (m_selection)
          m_selection->setActiveHorizon(h);
        if (m_layerSvc)
          m_layerSvc->setActiveHorizon(h);
      };
      locatorWidget->locator()->registerFilter(
          new HorizonLocatorFilter(horizonProvider, activate));

      // Note: no IssueLocatorFilter registration — the validation workflow
      // keeps no queryable issue store, so the filter could only ever sit on
      // a permanently-empty provider. Issue navigation lives on the
      // validation page's issueTable → ThreeWayLocator path instead.

      topBar->layout()->addWidget(locatorWidget);
      auto *focus = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
      connect(focus, &QShortcut::activated, locatorWidget,
              [locatorWidget] { locatorWidget->search(QString()); });
    }

    if (store && m_projectSvc)
    {
      // 保存 = 快速访问栏第一颗（Office 惯例）+「文件」菜单；Ctrl+S 挂在
      // action 上（菜单里可见快捷键），不再单独立 QShortcut。
      auto *saveAct = new QAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileSave.svg")),
                                  tr("保存工程"), this);
      saveAct->setObjectName(QStringLiteral("saveProjectAction"));
      saveAct->setShortcut(m_currentPage == QLatin1String("correlation") ? QKeySequence()
                                                                        : QKeySequence(QKeySequence::Save));
      saveAct->setToolTip(tr("保存工程（Ctrl+S）"));
      // §41.2 ordering through the write queue: gpkg commit (no-op until edit
      // buffers report dirty state) then the atomic .qgz write.
      auto saveFn = [this, store]() {
        if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
        {
          QgsMessageLog::logMessage(tr("无打开工程 — 无法保存"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        const auto res = store->saveAll(
            [] { return PaleoProjectStore::WriteResult{true, QString()}; },
            [this] {
              saveCanvasExtent(); // display state travels inside the .qgz
              const bool ok = m_projectSvc->writeProject();
              return PaleoProjectStore::WriteResult{
                  ok, ok ? QString() : m_projectSvc->lastErrors().join(QLatin1Char(';'))};
            });
        // W3 保存反馈：成功落状态栏（与打开工程失败的 §38 弹框契约对齐——
        // 失败是阻断级，弹框如实给原因），同时照记日志。
        QgsMessageLog::logMessage(
            res.ok ? tr("工程已保存") : tr("保存失败：%1").arg(res.error),
            QStringLiteral("Paleo"),
            res.ok ? Qgis::MessageLevel::Info : Qgis::MessageLevel::Critical);
        if (res.ok)
        {
          if (statusBar())
            statusBar()->showMessage(tr("工程已保存：%1").arg(m_projectSvc->projectPath()), 5000);
          updateWindowTitle();
        }
        else if (QGuiApplication::platformName() != QLatin1String("offscreen"))
          PaleoNotify::critical(this, tr("保存工程失败"), res.error);
      };
      connect(saveAct, &QAction::triggered, this, saveFn);
      if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
      {
        qab->addAction(saveAct);
        if (auto *saveBtn = qobject_cast<QToolButton *>(qab->widgetForAction(saveAct)))
        {
          saveBtn->setObjectName(QStringLiteral("saveButton"));
          saveBtn->setAccessibleName(tr("保存工程"));
        }
      }
      if (auto *fileMenu = findChild<QMenu *>(QStringLiteral("fileMenu")))
        for (QAction *a : fileMenu->actions())
          if (a->objectName() == QLatin1String("fileMenuSaveAnchor"))
            {
            fileMenu->insertAction(a, saveAct);
            fileMenu->insertSeparator(saveAct);
            break;
          }
    }
  }

  // Release management tab in the bottom dock.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      auto *releasePanel = new ReleasePanel(bottomTabs);
      releasePanel->setObjectName(QStringLiteral("releasePanel"));
      releasePanel->setProviders(
          [store]() { return store->metaDbPath(); },
          [this]() {
            QVector<LayerDeclaration> declared;
            QString manifestErr;
            if (m_layerSvc && !m_layerSvc->tryDeclared(&declared, &manifestErr))
              QgsMessageLog::logMessage(tr("发布面板：图层清单读取失败：%1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return declared;
          });
      connect(releasePanel, &ReleasePanel::statusMessage, this,
              [](const QString &msg) {
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Info);
              });
      if (m_projectSvc)
        connect(m_projectSvc, &QgisProjectService::projectOpened, releasePanel,
                &ReleasePanel::refresh);
      bottomTabs->addTab(releasePanel, tr("发布"));
    }

  // Task panel replaces the placeholder in the 任务 tab (index 1); attribute
  // table joins as its own tab, fed by the instantiated-layer set.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      const int idx = bottomTabs->indexOf(
          bottomTabs->findChild<QTextEdit *>(QStringLiteral("tasksPlaceholder")));
      auto *taskPanel = new TaskPanel(store, taskSvc, bottomTabs);
      if (idx >= 0)
      {
        QWidget *old = bottomTabs->widget(idx);
        bottomTabs->removeTab(idx);
        delete old;
        bottomTabs->insertTab(idx, taskPanel, tr("任务"));
      }
      else
        bottomTabs->addTab(taskPanel, tr("任务"));

      if (m_layerSvc && m_canvasCtl)
      {
        auto *attrPanel = new AttributeTablePanel(
            m_canvasCtl->canvas(),
            [this](const QString &layerId) -> QgsVectorLayer * {
              QgsMapLayer *l = m_layerSvc->layer(layerId);
              if (!l)
                l = m_layerSvc->instantiate(layerId);
              return qobject_cast<QgsVectorLayer *>(l);
            },
            bottomTabs);
        attrPanel->setObjectName(QStringLiteral("attributeTablePanel"));
        auto refreshIds = [this, attrPanel] {
          QVector<LayerDeclaration> declared;
          QString manifestErr;
          if (!m_layerSvc->tryDeclared(&declared, &manifestErr))
          {
            QgsMessageLog::logMessage(tr("属性表面板：图层清单读取失败：%1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return; // keep the existing layer list instead of blanking it
          }
          QStringList ids;
          for (const LayerDeclaration &d : declared)
            ids << d.layerId;
          attrPanel->setLayerIds(ids);
        };
        refreshIds();
        connect(m_layerSvc, &QgisLayerService::layerInstantiated, this,
                [refreshIds](const QString &) { refreshIds(); });
        connect(m_layerSvc, &QgisLayerService::horizonReleased, this,
                [this, attrPanel, refreshIds](const QString &) {
                  refreshIds();
                  const QString cur = attrPanel->currentLayerId();
                  if (!cur.isEmpty() && (!m_layerSvc->isInstantiated(cur) || !m_layerSvc->layer(cur)))
                    attrPanel->showLayer(cur);
                });
        if (m_projectSvc)
          connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                  [refreshIds](const QString &) { refreshIds(); });
        bottomTabs->addTab(attrPanel, tr("属性表"));
      }
    }

  // Processing entry point in the ribbon's right group (global tool, like the
  // QGIS Processing Toolbox): paleo:* algorithms first-class, the full
  // registry grouped under per-provider submenus. Each item opens the native
  // QGIS algorithm dialog (non-blocking, offscreen-safe).
  if (procSvc)
  {
    if (SARibbonButtonGroupWidget *topBar = ribbonBar()->rightButtonGroup())
    {
      auto *btn = new QToolButton(topBar);
      btn->setObjectName(QStringLiteral("processingButton"));
      btn->setText(tr("处理算法"));
      btn->setAccessibleName(tr("处理算法选择"));
      btn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("processingAlgorithm.svg")));
      btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      btn->setPopupMode(QToolButton::InstantPopup);
      auto *menu = new QMenu(btn);

      // W6：菜单显示算法的 displayName（人读名），机器 id 只留 tooltip——
      // 此前 addAction(id) 直接把 "paleo:paleo_constraint_idw" 甩给用户。
      const auto displayNameFor = [](const QString &id) {
        const QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
        const QgsProcessingAlgorithm *alg = reg ? reg->algorithmById(id) : nullptr;
        return alg ? alg->displayName() : id;
      };
      const auto addAlgorithmAction = [this, procSvc, &displayNameFor](QMenu *m,
                                                                       const QString &id) {
        QAction *a = m->addAction(displayNameFor(id), this,
                                  [this, procSvc, id]() {
                                    QString err;
                                    if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                                      QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                                Qgis::MessageLevel::Warning);
                                  });
        a->setToolTip(id);
      };

      for (const QString &id : procSvc->paleoAlgorithmIds())
        addAlgorithmAction(menu, id);

      QMap<QString, QMenu *> providerMenus;
      for (const QString &id : procSvc->algorithmIds())
      {
        if (id.startsWith(QStringLiteral("paleo:")))
          continue;
        const QString provider = id.section(QLatin1Char(':'), 0, 0);
        QMenu *&sub = providerMenus[provider];
        if (!sub)
          sub = menu->addMenu(provider);
        addAlgorithmAction(sub, id);
      }

      btn->setMenu(menu);
      // 插在「面板」钮之前：[搜索][处理算法][面板][Web 服务]。
      QAction *before = nullptr;
      if (auto *panelsBtn = topBar->findChild<QToolButton *>(QStringLiteral("panelsMenuButton")))
        for (QAction *a : topBar->actions())
          if (topBar->widgetForAction(a) == panelsBtn)
            before = a;
      topBar->insertWidget(before, btn);
    }
  }

  // 编辑工具 (wave/edit-tools): digitizing toolset — add/reshape/move/delete +
  // vertex editing routed through the editing service; undo/redo follows the
  // selected layer. Ribbon 形态：PaleoEditingToolbar 只当逻辑宿主（工具
  // 生命周期、图层下拉、门控/原因），本体不上屏；它的 QAction 进三个编图页
  // 的「要素编辑」组（buildRibbonPanels），撤销/重做进快速访问栏。页作用域
  // = 只有编图链三页的页签里有这组命令。
  PaleoEditingToolbar *editTb = nullptr;
  if (m_canvasCtl)
  {
    editTb = new PaleoEditingToolbar(m_canvasCtl->canvas(), this);
    editTb->setObjectName(QStringLiteral("editingToolbar"));
    if (editSvc)
      editTb->setEditingService(editSvc);
    if (m_projectSvc)
      editTb->setProject(m_projectSvc->project());
    editTb->refreshFromProject();
    if (m_projectSvc)
    {
      connect(m_projectSvc, &QgisProjectService::projectOpened, editTb,
              [editTb](const QString &) { editTb->refreshFromProject(); });
      if (QgsProject *proj = m_projectSvc->project())
      {
        connect(proj, &QgsProject::layersAdded, editTb,
                [editTb](const QList<QgsMapLayer *> &) { editTb->refreshFromProject(); });
        connect(proj, &QgsProject::layersRemoved, editTb,
                [editTb](const QStringList &) { editTb->refreshFromProject(); });
      }
    }
    // The selected tree layer, canvas target and ribbon target form one context.
    QgsMapCanvas *canvas = m_canvasCtl->canvas();
    connect(canvas, &QgsMapCanvas::mapToolSet, this, [this, canvas](QgsMapTool *tool, QgsMapTool *) {
      if (!tool || !tool->property("paleo-action").isValid() || !canvas->currentLayer() ||
          !m_projectSvc || !m_projectSvc->project() || !m_projectSvc->project()->layerTreeRoot())
        return;
      auto *node = m_projectSvc->project()->layerTreeRoot()->findLayer(canvas->currentLayer()->id());
      if (node && !node->isVisible())
      {
        node->setItemVisibilityCheckedParentRecursive(true);
        statusBar()->showMessage(tr("已显示操作图层：%1").arg(canvas->currentLayer()->name()), 5000);
      }
    });
    auto *tree = findChild<QgsLayerTreeView *>(QStringLiteral("layerTreeView"));
    if (tree)
    {
      connect(tree, &QgsLayerTreeView::currentLayerChanged, editTb,
              [editTb, canvas, tree](QgsMapLayer *layer) {
        auto *vector = qobject_cast<QgsVectorLayer *>(layer);
        editTb->setCurrentLayer(vector);
        if (!editTb->isEditing() && !vector)
          canvas->setCurrentLayer(layer); // a raster is a valid browsing target
        // A refused switch restores the tree as well as the ribbon.
        const QSignalBlocker block(tree);
        tree->setCurrentLayer(canvas->currentLayer());
      });
      connect(editTb, &PaleoEditingToolbar::stateChanged, tree, [canvas, tree] {
        const QSignalBlocker block(tree);
        tree->setCurrentLayer(canvas->currentLayer());
      });
    }
    connect(editTb, &PaleoEditingToolbar::editRefused, this, [this](const QString &reason) {
      statusBar()->showMessage(reason, 8000);
    });
    editTb->hide(); // 逻辑宿主，不进布局
    if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
    {
      qab->addAction(editTb->actionUndo());
      qab->addAction(editTb->actionRedo());
    }
  }

  // #148 地图册批量导出：面板挂右侧 dock（与属性建模同区 tab），入口在
  // 「智能编图 › 图件输出」组（ribbonMapBookAction）。catalog 在 attachMapping
  // 补注入；工程上下文全部经 provider 现取，工程关闭由 resetProjectScopedState
  // 调 resetProject（取消在途、丢弃迟到结果）。
  if (taskSvc && !m_mapBookDock)
  {
    m_mapBookPanel = new PaleoMapBookPanel(this);
    m_mapBookPanel->setObjectName(QStringLiteral("mapBookPanel"));
    m_mapBookDock = new QDockWidget(tr("地图册"), this);
    m_mapBookDock->setObjectName(QStringLiteral("mapBookDock"));
    m_mapBookDock->setWidget(m_mapBookPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_mapBookDock);
    if (m_rightDock)
      tabifyDockWidget(m_rightDock, m_mapBookDock);
    m_mapBookDock->hide();

    m_mapBookCtl = new PaleoMapBookController(m_mapBookPanel, taskSvc, this);
    m_mapBookCtl->setObjectName(QStringLiteral("mapBookController"));
    m_mapBookCtl->setProjectProvider([this]() -> QgsProject * {
      return m_projectSvc && !m_projectSvc->projectPath().isEmpty() ? m_projectSvc->project()
                                                                     : nullptr;
    });
    m_mapBookCtl->setProjectDirProvider([this]() {
      return m_projectSvc && !m_projectSvc->projectPath().isEmpty()
                 ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                 : QString();
    });
    m_mapBookCtl->setLayersProvider([this]() {
      return m_canvasCtl && m_canvasCtl->canvas() ? m_canvasCtl->canvas()->layers()
                                                  : QList<QgsMapLayer *>();
    });
    m_mapBookCtl->setCrsTextProvider([this]() {
      return m_canvasCtl && m_canvasCtl->canvas()
                 ? m_canvasCtl->canvas()->mapSettings().destinationCrs().authid()
                 : QString();
    });
    m_mapBookCtl->setHorizonProvider(
        [this]() { return m_selection ? m_selection->activeHorizon() : QString(); });
    connect(m_mapBookCtl, &PaleoMapBookController::statusMessage, this,
            [this](const QString &msg) {
              if (statusBar())
                statusBar()->showMessage(msg, 6000);
            });

    auto *mapBookAct = new QAction(
        PaleoIcons::qgisTheme(QStringLiteral("mActionAtlasSettings.svg")), tr("地图册"), this);
    mapBookAct->setObjectName(QStringLiteral("ribbonMapBookAction"));
    mapBookAct->setToolTip(tr("按网格分幅批量导出地图册"));
    connect(mapBookAct, &QAction::triggered, this, [this] {
      if (!m_mapBookDock || !m_mapBookPanel)
        return;
      // 首次打开（或工程切换后）用画布当前范围与工程目录预填参数。
      if (!m_mapBookCtl->busy() && !m_mapBookPanel->area().valid() && m_canvasCtl &&
          m_canvasCtl->canvas())
      {
        const QgsRectangle e = m_canvasCtl->canvas()->extent();
        PaleoMapBook::Area area;
        area.xMin = e.xMinimum();
        area.yMin = e.yMinimum();
        area.xMax = e.xMaximum();
        area.yMax = e.yMaximum();
        m_mapBookPanel->setArea(area);
      }
      if (m_mapBookPanel->outputDir().isEmpty() && m_projectSvc &&
          !m_projectSvc->projectPath().isEmpty())
        m_mapBookPanel->setOutputDir(QDir(QFileInfo(m_projectSvc->projectPath()).absolutePath())
                                         .filePath(QStringLiteral("exports/mapbook")));
      m_mapBookDock->show();
      m_mapBookDock->raise();
    });
  }

  // 图件设计 entry (wave/layout-designer): create a print layout via the
  // layout service and open the designer shell dialog non-modally. 入口在
  // 「智能编图 › 图件输出」组（buildRibbonPanels 按 objectName 取这颗动作）。
  if (layoutSvc)
  {
    auto *designerAct = new QAction(
        PaleoIcons::qgisTheme(QStringLiteral("mActionNewLayout.svg")), tr("图件设计"), this);
    designerAct->setObjectName(QStringLiteral("ribbonDesignerAction"));
    designerAct->setToolTip(tr("新建布局并打开图件设计器"));
    connect(designerAct, &QAction::triggered, this, [this, layoutSvc] {
      if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
      {
        QgsMessageLog::logMessage(tr("无打开工程 — 无法创建布局"),
                                QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
        return;
      }
      static int s_layoutSeq = 0;
      QString err;
      QgsLayout *layout = layoutSvc->createLayout(
          tr("布局 %1").arg(++s_layoutSeq), &err);
      if (!layout)
      {
        QgsMessageLog::logMessage(tr("创建布局失败：%1").arg(err),
                                QStringLiteral("Paleo"), Qgis::MessageLevel::Critical);
        return;
      }
      auto *shell = new PaleoLayoutDesignerShell(layout, this);
      shell->setTaskService(m_taskSvc); // #85：导出走任务池 worker
      shell->setAttribute(Qt::WA_DeleteOnClose);
      shell->setModal(false);
      shell->show();
    });
  }
  return editTb;
}

// ---------------------------------------------------------------------------
// 方向51：AI 助手 dock 装配。幂等（页签已建则只重挂控制器）。
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachAiAssistant(AiChatController *controller)
{
  if (!controller)
    return;
  auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  if (!bottomTabs)
    return;
  auto *existing = bottomTabs->findChild<AiAssistDock *>(
      QStringLiteral("aiAssistantDock"));
  if (existing)
  {
    // 幂等分支：只把新控制器交给面板（面本身不重建，避免丢滚动位置）。
    existing->attachController(controller);
    return;
  }
  auto *dock = new AiAssistDock(controller, bottomTabs);
  dock->setObjectName(QStringLiteral("aiAssistantDock"));
  bottomTabs->addTab(dock, tr("AI 助手"));
  // 「配置…」是意图不是动作：面板不管配置存储，这里如实告诉用户配置在
  // 哪儿（文件 + 环境变量）——图形化配置对话框递延（TODOS.md）。
  connect(dock, &AiAssistDock::configureRequested, this, [this] {
    const QString hint =
      tr("大模型配置：%1（端点/模型/开关）；密钥经系统钥匙串或 "
         "PALEO_LLM_API_KEY 环境变量提供。图形化配置对话框尚未接入。")
        .arg(LlmConfig::path());
    QgsMessageLog::logMessage(hint, QStringLiteral("Paleo"),
                              Qgis::MessageLevel::Info);
    if (statusBar())
      statusBar()->showMessage(hint, 8000);
  });
}

// 方向64：错误呈现接线。ErrorHub 由 AppContext 持有并 installGlobal；这里只把
// 呈现层与历史面板挂到本窗口（视图层不持有服务生命周期）。
void PaleoMainWindow::attachErrorHub(ErrorHub *hub)
{
  if (!hub || m_notifications)
    return;
  m_notifications = new NotificationCenter(this, hub);
  m_notifications->setStatusBar(statusBar());
  auto *panel = new ErrorHistoryPanel(hub, this);
  m_errorHistoryDock = new PaleoDockWidget(tr("错误历史"), this);
  m_errorHistoryDock->setObjectName(QStringLiteral("errorHistoryDock"));
  m_errorHistoryDock->setWidget(panel);
  addDockWidget(Qt::BottomDockWidgetArea, m_errorHistoryDock);
  m_errorHistoryDock->hide(); // 按需唤出（布局与面板菜单 / showErrorHistory）
}

void PaleoMainWindow::showErrorHistory()
{
  if (!m_errorHistoryDock)
    return;
  m_errorHistoryDock->show();
  m_errorHistoryDock->raise();
}
