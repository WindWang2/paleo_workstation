#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisruntime.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "../workflow/workflows.h"
#include "../io/dataimportservice.h"
#include "../linkage/seismicmaplink.h"
#include "../qgis/qgisprocessingservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "pages/pagepanels.h"
#include "constraintdrawcontroller.h"
#include "correlationpanel.h"
#include "seismicpreviewpanel.h"
#include "layoutdesignershell.h"
#include "edittools/editingtoolbar.h"
#include "../qgis/qgislayoutservice.h"
#include "../qgis/qgiseditingservice.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgslayertreeview.h>
#include <qgsmessagelog.h>
#include <qgsmessagelogviewer.h>
#include <qgslocatorwidget.h>
#include <qgslocator.h>
#include <qgsvectorlayer.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSettings>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>

// §42 shell: five-page workflow chain on top, layer tree left, page panel
// right, log/tasks bottom, startup page stacked under the canvas. The header
// fixes the member set, so the page table and cross-widget lookups live at
// file scope / via objectName (same discipline as qgiscanvascontroller.cpp).
namespace
{
  // Tab order = reading order = right-panel stack order.
  const QStringList kPageIds = {
    QStringLiteral("data"),       // 数据管理
    QStringLiteral("predict"),    // ①预测
    QStringLiteral("constraint"), // ②约束
    QStringLiteral("compose"),    // ③编图
    QStringLiteral("validate"),   // ④验证
  };

  bool isOffscreen()
  {
    return QGuiApplication::platformName() == QLatin1String("offscreen");
  }

  QStringList readRecentProjects()
  {
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    QStringList recent = s.value(QStringLiteral("recentProjects")).toStringList();
    if (recent.isEmpty()) // alternate flat-key spelling, kept as a courtesy
      recent = QSettings().value(QStringLiteral("paleo/recentProjects")).toStringList();
    return recent;
  }

  void writeRecentProjects(const QStringList &recent)
  {
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    s.setValue(QStringLiteral("recentProjects"), recent);
  }

  // §42.1 startup page: recent-projects list + 新建/打开. Named children so the
  // shell (and tests) can find them: recentProjectsList, newProjectButton,
  // openProjectButton.
  QWidget *makeStartupPage()
  {
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("startupPage"));
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(64, 48, 64, 48);
    lay->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Paleo Workbench"), page);
    QFont f = title->font();
    f.setPointSize(15); // DESIGN.md 图件标题/页级标题 token
    f.setBold(true);
    title->setFont(f);
    auto *sub = new QLabel(QStringLiteral("古地理编图工作台 — 新建工程或打开最近工程开始"), page);

    auto *recentLabel = new QLabel(QStringLiteral("最近工程"), page);
    auto *list = new QListWidget(page);
    list->setObjectName(QStringLiteral("recentProjectsList"));
    list->setAccessibleName(QStringLiteral("最近工程列表"));
    const QStringList recent = readRecentProjects();
    for (const QString &p : recent)
    {
      auto *item = new QListWidgetItem(p, list);
      item->setData(Qt::UserRole, p); // activation handler reads the path from here
    }
    if (recent.isEmpty()) // §42.4 empty state — guidance, not a blank panel
    {
      auto *item = new QListWidgetItem(QStringLiteral("（暂无最近工程）"), list);
      item->setFlags(Qt::NoItemFlags);
    }

    auto *btnRow = new QHBoxLayout;
    auto *newBtn = new QPushButton(QStringLiteral("新建工程"), page);
    newBtn->setObjectName(QStringLiteral("newProjectButton"));
    auto *openBtn = new QPushButton(QStringLiteral("打开工程"), page);
    openBtn->setObjectName(QStringLiteral("openProjectButton"));
    btnRow->addWidget(newBtn);
    btnRow->addWidget(openBtn);
    btnRow->addStretch(1);

    lay->addWidget(title);
    lay->addWidget(sub);
    lay->addSpacing(16);
    lay->addWidget(recentLabel);
    lay->addWidget(list, 1);
    lay->addLayout(btnRow);
    return page;
  }
} // namespace

PaleoMainWindow::PaleoMainWindow(QgisCanvasController *canvasCtl,
                                 QgisProjectService *projectSvc,
                                 QgisLayerService *layerSvc,
                                 ToolAvailabilityService *tools,
                                 SelectionContext *selection,
                                 QWidget *parent)
  : QMainWindow(parent)
  , m_canvasCtl(canvasCtl)
  , m_projectSvc(projectSvc)
  , m_layerSvc(layerSvc)
  , m_tools(tools)
  , m_selection(selection)
{
  buildShell();

  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this](const QString &) { onProjectOpened(); });

  showStartup(); // §42.1: first-run lands on the startup page
  restoreWindowState();
}

void PaleoMainWindow::buildShell()
{
  setWindowTitle(QStringLiteral("Paleo Workbench"));
  setMinimumSize(1280, 800); // §42.11 a11y floor

  const QStringList labels = {
    QStringLiteral("数据管理"), QStringLiteral("①预测"), QStringLiteral("②约束"),
    QStringLiteral("③编图"),   QStringLiteral("④验证"),
  };

  // ---- top: workflow chain tab bar ----
  m_workflowTabs = new QTabBar(this);
  m_workflowTabs->setObjectName(QStringLiteral("workflowTabs"));
  m_workflowTabs->setExpanding(false);
  m_workflowTabs->setDrawBase(false);
  m_workflowTabs->setAccessibleName(QStringLiteral("工作流步骤"));
  for (int i = 0; i < kPageIds.size(); ++i)
  {
    m_workflowTabs->addTab(labels.at(i));
    m_workflowTabs->setTabData(i, kPageIds.at(i));
  }
  connect(m_workflowTabs, &QTabBar::currentChanged, this, [this](int idx) {
    if (idx >= 0)
      showPage(m_workflowTabs->tabData(idx).toString());
  });

  auto *topWidget = new QWidget(this);
  topWidget->setObjectName(QStringLiteral("workflowTopBar"));
  auto *topLay = new QHBoxLayout(topWidget);
  topLay->setContentsMargins(12, 6, 12, 0);
  topLay->addWidget(m_workflowTabs);
  topLay->addStretch(1);

  // ---- center: startup page stacked under the canvas ----
  m_centerStack = new QStackedWidget(this);
  m_centerStack->setObjectName(QStringLiteral("centerStack"));

  QWidget *startup = makeStartupPage();
  m_centerStack->addWidget(startup); // index 0

  if (m_canvasCtl)
    m_centerStack->addWidget(m_canvasCtl->canvas()); // index 1 — reparents the parentless canvas
  else
    m_centerStack->addWidget(new QWidget(m_centerStack));

  // Startup-page actions: dialogs only exist when a real platform is present;
  // offscreen the buttons exist but stay inert (no modal QFileDialog).
  if (auto *openBtn = startup->findChild<QPushButton *>(QStringLiteral("openProjectButton")))
    connect(openBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getOpenFileName(
          this, QStringLiteral("打开工程"), QString(), QStringLiteral("Paleo 工程 (*.qgz *.qgs)"));
      if (!p.isEmpty())
        m_projectSvc->openProject(p); // projectOpened -> onProjectOpened()
    });
  if (auto *newBtn = startup->findChild<QPushButton *>(QStringLiteral("newProjectButton")))
    connect(newBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getSaveFileName(
          this, QStringLiteral("新建工程"), QString(), QStringLiteral("Paleo 工程 (*.qgz)"));
      if (!p.isEmpty())
        m_projectSvc->createProject(p);
    });
  if (auto *list = startup->findChild<QListWidget *>(QStringLiteral("recentProjectsList")))
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
      const QString p = item ? item->data(Qt::UserRole).toString() : QString();
      if (!p.isEmpty() && m_projectSvc)
        m_projectSvc->openProject(p);
    });

  auto *central = new QWidget(this);
  auto *clay = new QVBoxLayout(central);
  clay->setContentsMargins(0, 0, 0, 0);
  clay->setSpacing(0);
  clay->addWidget(topWidget);
  clay->addWidget(m_centerStack, 1);
  setCentralWidget(central);

  // ---- left dock: layer tree on the project's declared tree ----
  m_leftDock = new QDockWidget(QStringLiteral("图层"), this);
  m_leftDock->setObjectName(QStringLiteral("layerTreeDock"));
  if (m_projectSvc && m_projectSvc->project() && m_projectSvc->project()->layerTreeRoot())
  {
    auto *treeView = new QgsLayerTreeView(m_leftDock);
    treeView->setObjectName(QStringLiteral("layerTreeView"));
    auto *treeModel = new QgsLayerTreeModel(m_projectSvc->project()->layerTreeRoot(), treeView);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeReorder);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeRename);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);
    treeView->setModel(treeModel);
    m_leftDock->setWidget(treeView);
  }
  else
  {
    m_leftDock->setWidget(new QLabel(QStringLiteral("未打开工程"), m_leftDock));
  }
  addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);

  // ---- right dock: per-page panel stack (placeholders until §42.2 lands) ----
  m_rightDock = new QDockWidget(QStringLiteral("页面面板"), this);
  m_rightDock->setObjectName(QStringLiteral("pagePanelDock"));
  auto *panelHost = new QWidget(m_rightDock);
  panelHost->setObjectName(QStringLiteral("rightPanelHost"));
  auto *panelStack = new QStackedLayout(panelHost);
  for (const QString &label : labels)
  {
    auto *placeholder = new QLabel(label + QStringLiteral(" — 面板待实现"), panelHost);
    placeholder->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    placeholder->setMargin(12);
    panelStack->addWidget(placeholder);
  }
  m_rightDock->setWidget(panelHost);
  addDockWidget(Qt::RightDockWidgetArea, m_rightDock);

  // ---- bottom dock: log + tasks tabs ----
  m_bottomDock = new QDockWidget(QStringLiteral("日志 / 任务"), this);
  m_bottomDock->setObjectName(QStringLiteral("bottomDock"));
  auto *bottomTabs = new QTabWidget(m_bottomDock);
  bottomTabs->setObjectName(QStringLiteral("bottomTabs"));
  // Qt::Widget flags keep the QDialog-based viewer embeddable as a tab page.
  bottomTabs->addTab(new QgsMessageLogViewer(bottomTabs, Qt::Widget), QStringLiteral("日志"));
  auto *tasks = new QTextEdit(bottomTabs);
  tasks->setObjectName(QStringLiteral("tasksPlaceholder"));
  tasks->setReadOnly(true);
  tasks->setPlaceholderText(QStringLiteral("任务队列 — 待实现"));
  bottomTabs->addTab(tasks, QStringLiteral("任务"));
  m_bottomDock->setWidget(bottomTabs);
  addDockWidget(Qt::BottomDockWidgetArea, m_bottomDock);

  // ---- status bar: active horizon + provider count ----
  auto *horizonLabel = new QLabel(this);
  horizonLabel->setObjectName(QStringLiteral("statusHorizon"));
  const auto horizonText = [](const QString &h) {
    return QStringLiteral("层位：%1").arg(h.isEmpty() ? QStringLiteral("—") : h);
  };
  horizonLabel->setText(horizonText(m_selection ? m_selection->activeHorizon() : QString()));
  auto *providerLabel = new QLabel(this);
  providerLabel->setObjectName(QStringLiteral("statusProviders"));
  providerLabel->setText(QStringLiteral("数据提供器：%1")
                             .arg(QgisRuntime::isInitialized() ? QgisRuntime::providerCount() : 0));
  statusBar()->addPermanentWidget(horizonLabel);
  statusBar()->addPermanentWidget(providerLabel);

  // Canvas-fed status readouts (the dedicated QGIS statusbar coordinate/scale
  // widgets are app-only in 4.2 — plain labels fed by canvas signals instead).
  if (m_canvasCtl)
  {
    QgsMapCanvas *cv = m_canvasCtl->canvas();
    auto *coordLabel = new QLabel(this);
    coordLabel->setObjectName(QStringLiteral("statusCoords"));
    auto *scaleLabel = new QLabel(this);
    scaleLabel->setObjectName(QStringLiteral("statusScale"));
    connect(cv, &QgsMapCanvas::xyCoordinates, this,
            [coordLabel](const QgsPointXY &p) {
              coordLabel->setText(QStringLiteral("%1, %2").arg(p.x()).arg(p.y()));
            });
    auto updateScale = [scaleLabel, cv] {
      scaleLabel->setText(QStringLiteral("1:%1").arg(static_cast<qlonglong>(cv->scale())));
    };
    connect(cv, &QgsMapCanvas::scaleChanged, this, [updateScale](double) { updateScale(); });
    connect(cv, &QgsMapCanvas::extentsChanged, this, updateScale);
    updateScale();
    statusBar()->addPermanentWidget(coordLabel);
    statusBar()->addPermanentWidget(scaleLabel);
  }
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, horizonLabel,
            [horizonLabel, horizonText](const QString &h) { horizonLabel->setText(horizonText(h)); });

  // DESIGN.md tokens on shell chrome only — no custom painting.
  setStyleSheet(QStringLiteral(
      "QMainWindow { background: #EDF1F5; }"
      "QTabBar#workflowTabs::tab { color: #5D6E80; padding: 8px 18px; }"
      "QTabBar#workflowTabs::tab:selected { color: #1B73D0; border-bottom: 2px solid #1B73D0; }"
      "QTabBar#workflowTabs::tab:hover { color: #24303E; background: #EDF1F5; }"
      "QDockWidget::title { background: #EDF1F5; color: #24303E; padding: 6px 10px; }"
      "QStatusBar { background: #EDF1F5; color: #5D6E80; }"));
}

void PaleoMainWindow::showPage(const QString &pageId)
{
  const int idx = kPageIds.indexOf(pageId);
  if (idx < 0)
  {
    qWarning() << "PaleoMainWindow::showPage — unknown page id:" << pageId;
    return;
  }
  m_currentPage = pageId;

  if (m_workflowTabs && m_workflowTabs->currentIndex() != idx)
    m_workflowTabs->setCurrentIndex(idx); // re-enters via currentChanged, idempotent

  if (auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost")))
    if (auto *stack = static_cast<QStackedLayout *>(host->layout()))
      stack->setCurrentIndex(idx);

  // A workflow step implies the workspace: leave the startup page once a
  // project exists (with no project the startup page stays — nothing to show).
  if (m_centerStack && m_centerStack->currentIndex() == 0 && m_projectSvc &&
      !m_projectSvc->projectPath().isEmpty())
    m_centerStack->setCurrentIndex(1);
}

void PaleoMainWindow::showStartup()
{
  m_currentPage = QStringLiteral("startup");
  if (m_centerStack)
    m_centerStack->setCurrentIndex(0);
}

void PaleoMainWindow::onProjectOpened()
{
  if (m_centerStack)
    m_centerStack->setCurrentIndex(1); // startup -> workspace canvas

  if (m_projectSvc && !m_projectSvc->projectPath().isEmpty())
  {
    QStringList recent = readRecentProjects();
    recent.removeAll(m_projectSvc->projectPath());
    recent.prepend(m_projectSvc->projectPath());
    while (recent.size() > 10)
      recent.removeLast();
    writeRecentProjects(recent);

    if (auto *list = findChild<QListWidget *>(QStringLiteral("recentProjectsList")))
    {
      list->clear();
      for (const QString &p : recent)
      {
        auto *item = new QListWidgetItem(p, list);
        item->setData(Qt::UserRole, p);
      }
    }
  }

  restoreCanvasExtent(); // per-project display state from the .qgz

  // Workflow lands on the last-used page when the session was persisted,
  // otherwise on 数据管理 — first step of the chain.
  const QString last = QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
                           .value(QStringLiteral("lastPage")).toString();
  showPage(kPageIds.contains(last) ? last : kPageIds.first());
}

void PaleoMainWindow::closeEvent(QCloseEvent *event)
{
  saveWindowState();
  QMainWindow::closeEvent(event);
}

void PaleoMainWindow::saveWindowState()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("windowGeometry"), saveGeometry());
  s.setValue(QStringLiteral("windowState"), saveState());
  s.setValue(QStringLiteral("lastPage"), m_currentPage);
}

void PaleoMainWindow::restoreWindowState()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QByteArray geom = s.value(QStringLiteral("windowGeometry")).toByteArray();
  if (!geom.isEmpty())
    restoreGeometry(geom);
  const QByteArray state = s.value(QStringLiteral("windowState")).toByteArray();
  if (!state.isEmpty())
    restoreState(state);
  // Page restore happens in onProjectOpened (a page without a project is empty).
}

void PaleoMainWindow::saveCanvasExtent()
{
  if (!m_canvasCtl || !m_projectSvc || m_projectSvc->projectPath().isEmpty())
    return;
  QgsMapCanvas *cv = m_canvasCtl->canvas();
  const QgsRectangle e = cv->extent();
  if (e.isEmpty())
    return;
  const QString encoded = QStringLiteral("%1,%2,%3,%4")
                              .arg(e.xMinimum()).arg(e.yMinimum())
                              .arg(e.xMaximum()).arg(e.yMaximum());
  m_projectSvc->project()->writeEntry(QStringLiteral("paleo"),
                                      QStringLiteral("canvasExtent"), encoded);
}

void PaleoMainWindow::restoreCanvasExtent()
{
  if (!m_canvasCtl || !m_projectSvc || !m_projectSvc->project())
    return;
  bool ok = false;
  const QString raw = m_projectSvc->project()->readEntry(
      QStringLiteral("paleo"), QStringLiteral("canvasExtent"), QString(), &ok);
  if (!ok)
    return;
  const QStringList parts = raw.split(QLatin1Char(','));
  if (parts.size() != 4)
    return;
  bool conv[4] = {false, false, false, false};
  const double xmin = parts.at(0).toDouble(&conv[0]);
  const double ymin = parts.at(1).toDouble(&conv[1]);
  const double xmax = parts.at(2).toDouble(&conv[2]);
  const double ymax = parts.at(3).toDouble(&conv[3]);
  if (!conv[0] || !conv[1] || !conv[2] || !conv[3])
    return;
  const QgsRectangle e(xmin, ymin, xmax, ymax);
  if (e.isEmpty())
    return;
  m_canvasCtl->canvas()->setExtent(e);
  m_canvasCtl->canvas()->refresh();
}

void PaleoMainWindow::attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                                      CompositionWorkflow *compose, ValidationWorkflow *validate,
                                      DataImportService *importSvc, SeismicMapLink *seismicLink,
                                      QgisProcessingService *procSvc, PaleoProjectStore *store,
                                      QgisEditingService *editSvc, QgisLayoutService *layoutSvc)
{
  auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost"));
  auto *stack = host ? static_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack)
    return;

  // Replace placeholders in page order (data, predict, constraint, compose, validate).
  while (stack->count() > 0)
  {
    QLayoutItem *item = stack->takeAt(0);
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }

  auto *dataPage = new DataPage(host);
  auto *predictPage = new PredictPage(pred, m_layerSvc, host);
  auto *constraintPage = new ConstraintPage(constraint, host);
  auto *composePage = new ComposePage(compose, m_layerSvc, host);
  auto *validatePage = new ValidatePage(validate, host);
  stack->addWidget(dataPage);
  stack->addWidget(predictPage);
  stack->addWidget(constraintPage);
  stack->addWidget(composePage);
  stack->addWidget(validatePage);

  // 地震预览 + 连井剖面 — bottom-dock tabs fed by the import pipeline.
  SeismicPreviewPanel *seismicPanel = nullptr;
  WellCorrelationPanel *corrPanel = nullptr;
  if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
  {
    if (seismicLink)
    {
      seismicPanel = new SeismicPreviewPanel(seismicLink, bottomTabs);
      seismicPanel->setObjectName(QStringLiteral("seismicPreviewPanel"));
      bottomTabs->addTab(seismicPanel, QStringLiteral("地震"));
    }
    if (m_selection)
    {
      corrPanel = new WellCorrelationPanel(m_selection, bottomTabs);
      corrPanel->setObjectName(QStringLiteral("correlationPanel"));
      bottomTabs->addTab(corrPanel, QStringLiteral("连井剖面"));
    }
  }

  // Panel intents → workflows / selection. Params stay minimal for the shell
  // milestone — full parameter dialogs are per-panel follow-up work.
  if (importSvc && dataPage)
    connect(dataPage, &DataPage::importRequested, this,
            [this, importSvc](const QString &kind) {
              const QString path = QFileDialog::getOpenFileName(
                  this, tr("Import %1").arg(kind), QString(),
                  kind == QLatin1String("seismic")
                      ? tr("Seismic/vector files (*.sgy *.segy *.las *.csv *.gpkg *.shp);;All files (*)")
                      : tr("Vector/log files (*.las *.csv *.gpkg *.shp *.tif *.img);;All files (*)"));
              if (path.isEmpty())
                return;
              QString err;
              const QString assetId = importSvc->importFile(kind, path, &err);
              if (assetId.isEmpty())
                QgsMessageLog::logMessage(tr("Import failed: %1").arg(err),
                                        QStringLiteral("Paleo"), Qgis::Critical);
            });
    // Imported assets feed the bottom-dock panels.
    connect(importSvc, &DataImportService::imported, this,
            [this, importSvc, seismicPanel, corrPanel](const QString &kind, const QString &assetId, const QString &) {
              if (seismicPanel && kind == QLatin1String("seismic"))
              {
                const QString src = importSvc->assetSource(assetId);
                const QString abs = QDir(m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                                      : QString()).absoluteFilePath(src);
                if (src.endsWith(QLatin1String(".sgy"), Qt::CaseInsensitive) ||
                    src.endsWith(QLatin1String(".segy"), Qt::CaseInsensitive))
                  seismicPanel->loadLineFromFile(assetId, abs); // real SEG-Y traces
                else
                  seismicPanel->addSeismicAsset(assetId, src);
              }
              if (corrPanel && kind == QLatin1String("wells"))
              {
                QList<QPair<QString, QString>> wells;
                for (const QString &id : importSvc->assets(QStringLiteral("wells")))
                  wells.append({id, importSvc->assetSource(id)});
                corrPanel->setWells(wells);
                // LAS imports pull the GR curve into the column when present.
                const QString src = importSvc->assetSource(assetId);
                if (src.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
                  corrPanel->loadWellLas(assetId,
                                         QDir(m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                                           : QString()).absoluteFilePath(src),
                                         QStringLiteral("GR"));
              }
            });

  if (pred && predictPage)
    connect(predictPage, &PredictPage::runRequested, this,
            [this, pred](const QString &horizon, const QString &algId,
                         const QVariantMap &params) {
              QString err;
              pred->runPrediction(horizon, algId, params, &err);
            });

  if (constraint && constraintPage)
  {
    // 约束页端到端: 绘制请求 → 画布上的捕获工具 → workflow 提交 (§42).
    if (m_canvasCtl)
    {
      auto *drawCtl = new ConstraintDrawController(m_canvasCtl, constraint, this);
      connect(constraintPage, &ConstraintPage::drawConstraintRequested, drawCtl,
              [drawCtl](const QString &horizon, const QString &shape, int faciesCode) {
                drawCtl->startCapture(horizon, shape, faciesCode);
              });
      connect(drawCtl, &ConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    connect(constraintPage, &ConstraintPage::runIdwRequested, this,
            [this, constraint](const QString &horizon) {
              QString err;
              constraint->runConstraintIDW(horizon, QString(), QString(), 0.0, &err);
            });
  }
  if (compose && composePage)
  {
    connect(composePage, &ComposePage::fuseRequested, this,
            [this, compose, composePage](const QStringList &factorIds) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              if (compose->fuseFactors(horizon, factorIds, &err))
                composePage->refreshFactors();
            });
    connect(composePage, &ComposePage::polygonizeRequested, this,
            [this, compose, composePage](const QString &rasterId, double minArea, double simplify) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              QVariantMap params;
              params.insert(QStringLiteral("MIN_AREA"), minArea);
              params.insert(QStringLiteral("SIMPLIFY"), simplify);
              if (compose->deriveFaciesPolygons(horizon, rasterId, params, &err))
                composePage->refreshFactors();
              else if (!err.isEmpty())
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            });
  }
  if (validate && validatePage)
  {
    connect(validatePage, &ValidatePage::locateRequested, this,
            [this](const QString &layerId, const QString &) {
              if (m_canvasCtl)
                m_canvasCtl->zoomToLayer(layerId);
            });
  }

  // ---- top bar: domain locator + save ----
  if (auto *topBar = findChild<QWidget *>(QStringLiteral("workflowTopBar")))
  {
    if (m_layerSvc && m_selection && m_canvasCtl)
    {
      auto *locatorWidget = new QgsLocatorWidget(topBar);
      locatorWidget->setObjectName(QStringLiteral("paleoLocator"));
      locatorWidget->setMapCanvas(m_canvasCtl->canvas());
      locatorWidget->setPlaceholderText(tr("搜索井位/层位/问题…"));
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
        for (const LayerDeclaration &d : m_layerSvc->declared())
          if (!d.horizon.isEmpty() && !hs.contains(d.horizon))
            hs << d.horizon;
        hs.sort();
        return hs;
      };
      HorizonLocatorFilter::ActivateFn activate = [this](const QString &h) {
        if (m_selection)
          m_selection->setActiveHorizon(h);
        if (m_layerSvc)
          m_layerSvc->setActiveHorizon(h);
      };
      locatorWidget->locator()->registerFilter(
          new HorizonLocatorFilter(horizonProvider, activate));

      // Issues filter: provider is an empty list until the validation workflow
      // exposes its issue store; wiring the intent is still useful now.
      IssueLocatorFilter::IssueProvider issueProvider = []() {
        return QList<IssueLocatorFilter::IssueRef>();
      };
      IssueLocatorFilter::LocateFn locate = [this](const IssueLocatorFilter::IssueRef &ref) {
        if (m_canvasCtl && !ref.layerId.isEmpty())
          m_canvasCtl->zoomToLayer(ref.layerId);
      };
      locatorWidget->locator()->registerFilter(
          new IssueLocatorFilter(issueProvider, locate));

      topBar->layout()->addWidget(locatorWidget);
      auto *focus = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
      connect(focus, &QShortcut::activated, locatorWidget,
              [locatorWidget] { locatorWidget->search(QString()); });
    }

    if (store && m_projectSvc)
    {
      auto *saveBtn = new QToolButton(topBar);
      saveBtn->setObjectName(QStringLiteral("saveButton"));
      saveBtn->setText(tr("保存"));
      saveBtn->setAccessibleName(tr("保存工程"));
      // §41.2 ordering through the write queue: gpkg commit (no-op until edit
      // buffers report dirty state) then the atomic .qgz write.
      auto saveFn = [this, store]() {
        if (m_projectSvc->projectPath().isEmpty())
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
        QgsMessageLog::logMessage(
            res.ok ? tr("工程已保存") : tr("保存失败：%1").arg(res.error),
            QStringLiteral("Paleo"),
            res.ok ? Qgis::MessageLevel::Info : Qgis::MessageLevel::Critical);
      };
      connect(saveBtn, &QToolButton::clicked, this, saveFn);
      auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
      connect(saveShortcut, &QShortcut::activated, this, saveFn);
      topBar->layout()->addWidget(saveBtn);
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
          [this]() { return m_layerSvc ? m_layerSvc->declared() : QVector<LayerDeclaration>(); });
      connect(releasePanel, &ReleasePanel::statusMessage, this,
              [](const QString &msg) {
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Info);
              });
      connect(m_projectSvc, &QgisProjectService::projectOpened, releasePanel,
              &ReleasePanel::refresh);
      bottomTabs->addTab(releasePanel, QStringLiteral("发布"));
    }

  // Task panel replaces the placeholder in the 任务 tab (index 1); attribute
  // table joins as its own tab, fed by the instantiated-layer set.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      const int idx = bottomTabs->indexOf(
          bottomTabs->findChild<QTextEdit *>(QStringLiteral("tasksPlaceholder")));
      auto *taskPanel = new TaskPanel(store, bottomTabs);
      if (idx >= 0)
      {
        QWidget *old = bottomTabs->widget(idx);
        bottomTabs->removeTab(idx);
        delete old;
        bottomTabs->insertTab(idx, taskPanel, QStringLiteral("任务"));
      }
      else
        bottomTabs->addTab(taskPanel, QStringLiteral("任务"));

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
          QStringList ids;
          for (const LayerDeclaration &d : m_layerSvc->declared())
            ids << d.layerId;
          attrPanel->setLayerIds(ids);
        };
        refreshIds();
        connect(m_layerSvc, &QgisLayerService::layerInstantiated, this,
                [refreshIds](const QString &) { refreshIds(); });
        connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                [refreshIds](const QString &) { refreshIds(); });
        bottomTabs->addTab(attrPanel, QStringLiteral("属性表"));
      }
    }

  // Processing entry point on the top bar: paleo:* algorithms first-class,
  // the full registry grouped under per-provider submenus. Each item opens
  // the native QGIS algorithm dialog (non-blocking, offscreen-safe).
  if (procSvc)
  {
    if (auto *topBar = findChild<QWidget *>(QStringLiteral("workflowTopBar")))
    {
      auto *btn = new QToolButton(topBar);
      btn->setObjectName(QStringLiteral("processingButton"));
      btn->setText(tr("处理算法"));
      btn->setAccessibleName(tr("处理算法选择"));
      btn->setPopupMode(QToolButton::InstantPopup);
      auto *menu = new QMenu(btn);

      for (const QString &id : procSvc->paleoAlgorithmIds())
        menu->addAction(id, this,
                        [this, procSvc, id]() {
                          QString err;
                          if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                            QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                      Qgis::MessageLevel::Warning);
                        });

      QMap<QString, QMenu *> providerMenus;
      for (const QString &id : procSvc->algorithmIds())
      {
        if (id.startsWith(QStringLiteral("paleo:")))
          continue;
        const QString provider = id.section(QLatin1Char(':'), 0, 0);
        QMenu *&sub = providerMenus[provider];
        if (!sub)
          sub = menu->addMenu(provider);
        const QString algName = id.section(QLatin1Char(':'), 1);
        sub->addAction(algName.isEmpty() ? id : algName, this,
                       [this, procSvc, id]() {
                         QString err;
                         if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                           QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                     Qgis::MessageLevel::Warning);
                       });
      }

      btn->setMenu(menu);
      topBar->layout()->addWidget(btn);
    }
  }

  // 编辑 toolbar dock (wave/edit-tools): hosts the digitizing toolset —
  // add/reshape/move/delete + vertex editing routed through the editing
  // service; undo/redo follows the selected layer.
  if (m_canvasCtl)
  {
    auto *editTb = new PaleoEditingToolbar(m_canvasCtl->canvas(), this);
    editTb->setObjectName(QStringLiteral("editingToolbar"));
    if (editSvc)
      editTb->setEditingService(editSvc);
    editTb->refreshFromProject();
    connect(m_projectSvc, &QgisProjectService::projectOpened, editTb,
            [editTb](const QString &) { editTb->refreshFromProject(); });
    auto *editDock = new QDockWidget(tr("编辑"), this);
    editDock->setObjectName(QStringLiteral("editToolbarDock"));
    editDock->setWidget(editTb);
    addDockWidget(Qt::TopDockWidgetArea, editDock);
  }

  // 图件设计 entry (wave/layout-designer): create a print layout via the
  // layout service and open the designer shell dialog non-modally.
  if (layoutSvc)
    if (auto *topBar = findChild<QWidget *>(QStringLiteral("workflowTopBar")))
    {
      auto *designerBtn = new QToolButton(topBar);
      designerBtn->setObjectName(QStringLiteral("designerButton"));
      designerBtn->setText(tr("图件设计"));
      designerBtn->setAccessibleName(tr("打开图件设计器"));
      connect(designerBtn, &QToolButton::clicked, this, [this, layoutSvc] {
        if (m_projectSvc->projectPath().isEmpty())
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
        shell->setAttribute(Qt::WA_DeleteOnClose);
        shell->setModal(false);
        shell->show();
      });
      topBar->layout()->addWidget(designerBtn);
    }

  // Re-sync visible page index with current tab.
  const int idx = kPageIds.indexOf(m_currentPage);
  stack->setCurrentIndex(idx >= 0 ? idx : 0);
}
