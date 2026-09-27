#include "paleomainwindow.h"

#include "paleotheme.h" // T32：焦点环/mono 数字面 token 出口
#include "paleoicons.h" // ribbon 图标：QGIS 主题直取 + 自绘补缺
#include "paleoribbon.h" // SARibbon 壳公用件：主题/命令镜像

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisruntime.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "../workflow/workflows.h"
#include "../io/dataimportservice.h"
#include "../io/projectclassifier.h"
#include "../io/arearules.h"
#include "../domain/mappinghorizons.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/threewaylocator.h"
#include "../qgis/qgisprocessingservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/paleoprojectfile.h"
#include "../metadata/layermanifest.h"
#include "../metadata/mapversionstore.h"
#include "../services/projectdata.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/mapexport.h"
#include "../workflow/mapversioncontroller.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "pages/pagepanels.h"
#include "constraintdrawcontroller.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "../catalog/datacatalog.h"
#include "horizonchipbar.h"
#include "layoutdesignershell.h"
#include "webviewpanel.h"
#include "edittools/editingtoolbar.h"
#include "../qgis/qgislayoutservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../services/paleotaskservice.h"
#include "../io/geojsonaffine.h"
#include "decorations/paleodecorations.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgslayertreeview.h>
#include <qgslayertreeviewdefaultactions.h>
#include <qgsmessagelog.h>
#include <qgsmessagelogviewer.h>
#include <qgslocatorwidget.h>
#include <qgslocator.h>
#include <qgsvectorlayer.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgsmarkersymbol.h>
#include <qgssinglesymbolrenderer.h>
#include <qgswkbtypes.h>
#include <qgsrubberband.h>
#include <qgsgeometry.h>
#include <qgsmaptoolpan.h>
#include <qgsmaptoolzoom.h>

#include <QApplication>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QSplitter>
#include <QToolButton>
#include <QSettings>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHash>
#include <QHeaderView>
#include <QSet>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QPointer>
#include <QVBoxLayout>

#include <memory>

// §42 shell: five-page workflow chain as ribbon tabs on top, layer tree left,
// page panel right, log/tasks bottom, startup page stacked under the
// workspace. The header fixes the member set, so the page table and
// cross-widget lookups live at file scope / via objectName (same discipline
// as qgiscanvascontroller.cpp).
namespace
{
  // Tab order = reading order = right-panel stack order.
  const QStringList kPageIds = {
    QStringLiteral("data"),       // 数据管理
    QStringLiteral("predict"),    // 预测编图
    QStringLiteral("constraint"), // 单因素图
    QStringLiteral("compose"),    // 智能编图
    QStringLiteral("validate"),   // 验证
  };
  // ribbon 页签文案（用户裁决 2026-09-27：Ribbon 界面，五页保留验证）。
  const QStringList kPageLabels = {
    QStringLiteral("数据管理"), QStringLiteral("预测编图"), QStringLiteral("单因素图"),
    QStringLiteral("智能编图"), QStringLiteral("验证"),
  };
  // 右侧 dock 标题随页：数据页是属性面，编图页是参数面，验证页是结果面。
  const QStringList kPageDockTitles = {
    QStringLiteral("数据属性"), QStringLiteral("预测参数"), QStringLiteral("单因素参数"),
    QStringLiteral("编图参数"), QStringLiteral("验证结果"),
  };

  // SARibbonMainWindow 构造参数：顺带在基类构造前备好库（qrc + 关掉跟随
  // 系统暗色）。原生边框：Linux X11/Wayland 与 offscreen 测试同一路径，
  // SARibbon 此时自动用紧凑三行布局（页签与右侧按钮同一行）。
  SARibbonMainWindowStyles ribbonWindowStyle()
  {
    PaleoRibbon::prepareLibrary();
    return SARibbonMainWindowStyles(SARibbonMainWindowStyleFlag::UseRibbonMenuBar) |
           SARibbonMainWindowStyleFlag::UseNativeFrame;
  }

  // 页作用域的数字化工具面：编辑/编图工具只属于编图链三页
  // （预测/约束/编图——PALEO_QGIS_PLAN §9 的物源线、相界编辑所在）。
  // 数据管理页是纯数据面（预览+实体视图），验证页是检查面：
  // 两页画布只作展示，编辑 dock 隐藏，且进入时停用活动画布工具。
  const QStringList kEditingToolPages = {
    QStringLiteral("predict"),
    QStringLiteral("constraint"),
    QStringLiteral("compose"),
  };

  bool isOffscreen()
  {
    return QGuiApplication::platformName() == QLatin1String("offscreen");
  }

  // T22/§3 契约句：工区导入统一展示的 CRS 说明（文件夹确认表 + 单文件
  // 导入确认都只读挂这句）。状态栏短句另行，与 PDF 页脚同一文案。
  const QString kEngineeringCrsSentence = QStringLiteral(
      "局部工程坐标，单位米。源文件里的 EPSG:4326 只是标签，不会画到地图上。");

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
    auto *fromAreaBtn =
        new QPushButton(QStringLiteral("从工区文件夹新建"), page);
    fromAreaBtn->setObjectName(QStringLiteral("importFromFolderButton"));
    btnRow->addWidget(newBtn);
    btnRow->addWidget(openBtn);
    btnRow->addWidget(fromAreaBtn);
    btnRow->addStretch(1);

    lay->addWidget(title);
    lay->addWidget(sub);
    lay->addSpacing(16);
    lay->addWidget(recentLabel);
    lay->addWidget(list, 1);
    lay->addLayout(btnRow);
    return page;
  }
// T31 空态标签：宿主（地图画布/图层树）resize 时保持居中；白底半透明卡片
// 承载（DESIGN.md 画布装饰约定），文案永远带下一步动作指引。
class EmptyStateLabel : public QLabel
{
  public:
    EmptyStateLabel(const QString &text, QWidget *host) : QLabel(text, host)
    {
      setAlignment(Qt::AlignCenter);
      setWordWrap(true);
      setStyleSheet(QStringLiteral(
          "background: rgba(255,255,255,0.9); color: #5D6E80; padding: 12px 16px;"
          "border: 1px solid #DFE5EC; border-radius: 8px;"));
      host->installEventFilter(this);
      recenter(host->size());
    }

  protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
      if (ev->type() == QEvent::Resize)
        if (auto *w = qobject_cast<QWidget *>(obj))
          recenter(w->size());
      return QLabel::eventFilter(obj, ev);
    }

  private:
    void recenter(const QSize &host)
    {
      const int maxW = qMax(160, host.width() - 24);
      if (width() > maxW || height() > host.height())
        resize(maxW, qMax(40, heightForWidth(maxW)));
      adjustSize();
      move(qMax(0, (host.width() - width()) / 2),
           qMax(0, (host.height() - height()) / 2));
    }
};
} // namespace

PaleoMainWindow::PaleoMainWindow(QgisCanvasController *canvasCtl,
                                 QgisProjectService *projectSvc,
                                 QgisLayerService *layerSvc,
                                 ToolAvailabilityService *tools,
                                 SelectionContext *selection,
                                 QWidget *parent)
  : SARibbonMainWindow(parent, ribbonWindowStyle())
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

  // ---- center: startup page stacked under the workspace ----
  m_centerStack = new QStackedWidget(this);
  m_centerStack->setObjectName(QStringLiteral("centerStack"));

  QWidget *startup = makeStartupPage();
  m_centerStack->addWidget(startup); // index 0

  // 工作区两面（用户裁决：数据管理是列表面，另外四页以 QGIS 画布为主）：
  //   0 画布面 = 层位 chip 条 + QgsMapCanvas（预测编图/单因素图/智能编图/验证）
  //   1 数据面 = 「数据列表」在上 +「数据预览」在下的竖向分栏（数据管理）
  m_workspaceStack = new QStackedWidget(m_centerStack);
  m_workspaceStack->setObjectName(QStringLiteral("workspaceStack"));

  auto *canvasPane = new QWidget(m_workspaceStack);
  canvasPane->setObjectName(QStringLiteral("canvasPane"));
  auto *canvasLay = new QVBoxLayout(canvasPane);
  canvasLay->setContentsMargins(0, 0, 0, 0);
  canvasLay->setSpacing(0);
  // 层位 chip 条：ribbon 之下、画布之上（DESIGN.md 结构层）。切换 =
  // activeHorizon + 懒加载。
  auto *chipsRow = new QWidget(canvasPane);
  chipsRow->setObjectName(QStringLiteral("horizonChipRow"));
  auto *chipsLay = new QHBoxLayout(chipsRow);
  chipsLay->setContentsMargins(12, 4, 12, 4);
  auto *chips = new HorizonChipBar(m_selection, m_layerSvc, chipsRow);
  chips->setObjectName(QStringLiteral("horizonChips"));
  chips->setAccessibleName(QStringLiteral("层位切换"));
  chipsLay->addWidget(chips);
  chipsLay->addStretch(1);
  canvasLay->addWidget(chipsRow);
  if (m_canvasCtl)
    canvasLay->addWidget(m_canvasCtl->canvas(), 1); // reparents the parentless canvas
  else
    canvasLay->addStretch(1);
  m_workspaceStack->addWidget(canvasPane); // 0

  // §4 预览壳：数据管理区左侧数据列表树、右侧预览标签页；双击树节点在右侧打开预览（D7）。
  m_centerSplit = new QSplitter(Qt::Horizontal, m_workspaceStack);
  m_centerSplit->setObjectName(QStringLiteral("dataListPreviewSplit"));
  m_centerSplit->setChildrenCollapsible(false);
  m_dataListHost = new QWidget(m_centerSplit);
  m_dataListHost->setObjectName(QStringLiteral("dataListPanel"));
  m_dataListHost->setAccessibleName(QStringLiteral("数据列表"));
  m_dataListHost->setMinimumWidth(0);
  m_dataListHost->setMinimumHeight(0);
  auto *listHostLay = new QVBoxLayout(m_dataListHost);
  listHostLay->setContentsMargins(0, 0, 0, 0);
  m_centerSplit->addWidget(m_dataListHost);
  m_previewTabs = new DataPreviewTabs(m_centerSplit);
  m_previewTabs->setObjectName(QStringLiteral("dataPreview"));
  m_previewTabs->setAccessibleName(QStringLiteral("数据预览"));
  m_previewTabs->setMinimumWidth(0);
  m_previewTabs->setMinimumHeight(0);
  m_centerSplit->addWidget(m_previewTabs);
  m_centerSplit->setStretchFactor(0, 0);
  m_centerSplit->setStretchFactor(1, 1);
  m_workspaceStack->addWidget(m_centerSplit); // 1
  m_centerStack->addWidget(m_workspaceStack); // index 1

  // 画布装饰管理器（D11 临时配准水印等）：parent 到 canvas，renderComplete
  // 自连；各项默认关，按需 setEnabled。
  if (m_canvasCtl && m_canvasCtl->canvas())
    m_decorMgr = new PaleoDecorationManager(m_canvasCtl->canvas(), this);

  // 预览空态/首标签的分栏高度（普通 QTabWidget 的 currentChanged 覆盖
  // 0→1 与 1→0 两个迁移；加页时发 0，最后关页发 -1）。
  if (auto *inner = m_previewTabs->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")))
    connect(inner, &QTabWidget::currentChanged, this,
            [this](int) { applyPreviewSplit(); });
  // D7 最大化/还原：最大化前存用户分栏尺寸；还原恢复（无记录按 60% 预算）。
  connect(m_previewTabs, &DataPreviewTabs::previewMaximizeToggled, this,
          [this](bool on) {
            m_previewMaximized = on;
            if (on)
              m_preMaxSplitSizes = m_centerSplit ? m_centerSplit->sizes() : QList<int>{};
            else if (m_centerSplit && !m_preMaxSplitSizes.isEmpty())
            {
              m_centerSplit->setSizes(m_preMaxSplitSizes);
              m_previewExpanded = true; // 已给过预算，不再触碰用户尺寸
            }
            applyPreviewSplit();
          });

  // ---- T31 空态：地图没有图层时画布上的居中指引 ----
  EmptyStateLabel *mapEmpty = nullptr;
  if (m_canvasCtl)
  {
    mapEmpty = new EmptyStateLabel(
        QStringLiteral("地图上还没有图层 — 先在「数据管理」导入工区文件夹，或在「预测编图」运行预测"),
        m_canvasCtl->canvas());
    mapEmpty->setObjectName(QStringLiteral("mapEmptyState"));
    mapEmpty->raise();
  }

  // Startup-page actions: dialogs only exist when a real platform is present;
  // offscreen the buttons exist but stay inert (no modal QFileDialog).
  if (auto *openBtn = startup->findChild<QPushButton *>(QStringLiteral("openProjectButton")))
    connect(openBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getOpenFileName(
          this, QStringLiteral("打开工程"), QString(),
          QStringLiteral("Paleo 工程 (*.paleo);;QGIS 工程 (*.qgz *.qgs)"));
      if (p.isEmpty())
        return;
      // §38 blocking-error contract: a failed open surfaces as a dialog, not
      // a silent no-op on the startup page.
      if (!m_projectSvc->openProject(p))
        QMessageBox::critical(this, QStringLiteral("打开工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
    });
  if (auto *newBtn = startup->findChild<QPushButton *>(QStringLiteral("newProjectButton")))
    connect(newBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getSaveFileName(
          this, QStringLiteral("新建工程"), QString(), QStringLiteral("Paleo 工程 (*.qgz)"));
      if (p.isEmpty())
        return;
      if (!m_projectSvc->createProject(p))
        QMessageBox::critical(this, QStringLiteral("新建工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
    });
  // PROJECT_FILE_DESIGN：从工区文件夹新建——选目录后
  //   已有 project.paleo → 直接打开（幂等，不重建）；
  //   已有 *.qgz → 打开收养（openProject 自动写 project.paleo）；
  //   否则     → 在目录内建 <目录名>.qgz + project.paleo，再跑工区导入。
  if (auto *fromBtn =
          startup->findChild<QPushButton *>(QStringLiteral("importFromFolderButton")))
    connect(fromBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString dir =
          QFileDialog::getExistingDirectory(this, QStringLiteral("从工区文件夹新建工程"));
      if (!dir.isEmpty())
        openPath(dir);
    });
  if (auto *list = startup->findChild<QListWidget *>(QStringLiteral("recentProjectsList")))
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
      const QString p = item ? item->data(Qt::UserRole).toString() : QString();
      if (p.isEmpty() || !m_projectSvc)
        return;
      if (!m_projectSvc->openProject(p))
        QMessageBox::critical(this, QStringLiteral("打开工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
    });

  setCentralWidget(m_centerStack);

  // ---- left dock: layer tree on the project's declared tree ----
  m_leftDock = new QDockWidget(QStringLiteral("图层"), this);
  m_leftDock->setObjectName(QStringLiteral("layerTreeDock"));
  EmptyStateLabel *treeEmpty = nullptr;
  if (m_projectSvc && m_projectSvc->project() && m_projectSvc->project()->layerTreeRoot())
  {
    auto *treeView = new QgsLayerTreeView(m_leftDock);
    treeView->setObjectName(QStringLiteral("layerTreeView"));
    auto *treeModel = new QgsLayerTreeModel(m_projectSvc->project()->layerTreeRoot(), treeView);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeReorder);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeRename);
    treeModel->setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);
    treeView->setModel(treeModel);
    // 图层树默认动作（QGIS_NATIVE_ADOPTION）：QgsLayerTreeViewDefaultActions
    // 在 gui 已安装——用它组右键菜单；QgsLayerTreeViewMenuProvider 是
    // app-only（头不安装），不链接。
    if (QgsMapCanvas *cv = m_canvasCtl ? m_canvasCtl->canvas() : nullptr)
    {
      auto *treeActions = new QgsLayerTreeViewDefaultActions(treeView);
      auto *treeMenu = new QMenu(treeView);
      treeMenu->addAction(
          treeActions->actionZoomToLayers(cv, treeMenu));
      treeMenu->addAction(
          treeActions->actionZoomToSelection(cv, treeMenu));
      treeMenu->addAction(treeActions->actionShowFeatureCount(treeMenu));
      treeMenu->addSeparator();
      treeMenu->addAction(treeActions->actionRenameGroupOrLayer(treeMenu));
      treeMenu->addAction(treeActions->actionRemoveGroupOrLayer(treeMenu));
      treeView->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(treeView, &QWidget::customContextMenuRequested, treeMenu,
              [treeView, treeMenu](const QPoint &p) {
                treeMenu->popup(treeView->viewport()->mapToGlobal(p));
              });
    }
    m_leftDock->setWidget(treeView);
    // T31：图层树空态——工程没有图层时给指引，不留一棵空树。
    treeEmpty = new EmptyStateLabel(
        QStringLiteral("图层树是空的 — 导入数据后图层会出现在这里"), treeView);
    treeEmpty->setObjectName(QStringLiteral("layerTreeEmptyState"));
    treeEmpty->raise();
  }
  else
  {
    m_leftDock->setWidget(new QLabel(QStringLiteral("未打开工程"), m_leftDock));
  }
  addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);

  // 图层增删驱动两个空态（T31）：图层集为空 → 露出指引；否则收起。
  if (QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr)
  {
    const auto updateEmptyStates = [proj, mapEmpty, treeEmpty]() {
      const bool empty = proj->mapLayers().isEmpty();
      if (mapEmpty)
        mapEmpty->setVisible(empty);
      if (treeEmpty)
        treeEmpty->setVisible(empty);
    };
    updateEmptyStates();
    connect(proj, &QgsProject::layersAdded, this,
            [updateEmptyStates](const QList<QgsMapLayer *> &) { updateEmptyStates(); });
    connect(proj, &QgsProject::layersRemoved, this,
            [updateEmptyStates](const QStringList &) { updateEmptyStates(); });
  }

  // ---- right dock: per-page panel stack (placeholders until attachWorkflows) ----
  // 标题随页（kPageDockTitles）；ribbon 里的「参数」钮就是它的 toggleViewAction。
  m_rightDock = new QDockWidget(kPageDockTitles.first(), this);
  m_rightDock->setObjectName(QStringLiteral("pagePanelDock"));
  auto *panelHost = new QWidget(m_rightDock);
  panelHost->setObjectName(QStringLiteral("rightPanelHost"));
  auto *panelStack = new QStackedLayout(panelHost);
  for (const QString &label : kPageLabels)
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

  // ---- web shell dock (goal/webui-host): embed already-built web services.
  // Hidden by default; the WebViewPanel inside is lazily constructed on first
  // show / first URL submit, so app startup never pays QtWebEngineProcess
  // cost (the panel itself lazily instantiates QWebEngineView on setUrl).
  auto *webDock = new QDockWidget(tr("Web 服务"), this);
  webDock->setObjectName(QStringLiteral("webServiceDock"));
  auto *webHost = new QWidget(webDock);
  webHost->setObjectName(QStringLiteral("webServiceHost"));
  auto *webLay = new QVBoxLayout(webHost);
  webLay->setContentsMargins(8, 8, 8, 8); // spacing.sm panel padding
  webLay->setSpacing(4);                  // spacing.xs between bar and view
  auto *addrRow = new QHBoxLayout;
  addrRow->setSpacing(4);
  auto *addrEdit = new QLineEdit(webHost);
  addrEdit->setObjectName(QStringLiteral("webAddressEdit"));
  addrEdit->setAccessibleName(tr("Web 服务地址"));
  addrEdit->setPlaceholderText(tr("http:// 或 https:// 服务地址"));
  addrEdit->setClearButtonEnabled(true);
  auto *addrOpen = new QPushButton(tr("打开"), webHost);
  addrOpen->setObjectName(QStringLiteral("webOpenButton"));
  addrRow->addWidget(addrEdit, 1);
  addrRow->addWidget(addrOpen);
  webLay->addLayout(addrRow);
  webDock->setWidget(webHost);
  addDockWidget(Qt::RightDockWidgetArea, webDock);
  webDock->hide(); // toggle via the top-bar button below

  // The panel below the address row is created on first dock show or first
  // URL submit — whichever comes first.
  const auto ensureWebPanel = [webHost, webLay]() -> WebViewPanel * {
    auto *panel = webHost->findChild<WebViewPanel *>(QStringLiteral("webViewPanel"));
    if (!panel)
    {
      panel = new WebViewPanel(webHost);
      panel->setObjectName(QStringLiteral("webViewPanel"));
      webLay->addWidget(panel, 1);
    }
    return panel;
  };
  connect(webDock, &QDockWidget::visibilityChanged, this,
          [ensureWebPanel](bool visible) {
            if (visible)
              ensureWebPanel();
          });
  const auto openWebUrl = [this, addrEdit, ensureWebPanel] {
    QString text = addrEdit->text().trimmed();
    if (text.isEmpty())
      return;
    // Shell for already-running services: bare host[:port]/host/path → http.
    if (!text.contains(QStringLiteral("://")))
      text.prepend(QStringLiteral("http://"));
    WebViewPanel *panel = ensureWebPanel();
    if (!panel->setUrl(QUrl(text)) && !panel->lastError().isEmpty())
      QgsMessageLog::logMessage(panel->lastError(), QStringLiteral("Paleo"),
                              Qgis::MessageLevel::Warning);
  };
  connect(addrOpen, &QPushButton::clicked, this, openWebUrl);
  connect(addrEdit, &QLineEdit::returnPressed, this, openWebUrl);

  // ribbon 骨架（页签 / 文件菜单 / 右侧按钮组）——右侧组要挂 Web dock 的
  // toggleViewAction，所以放在 dock 建好之后。
  buildRibbon();

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
    coordLabel->setFont(PaleoTheme::monoFont()); // T32：坐标读数是数字面
    auto *scaleLabel = new QLabel(this);
    scaleLabel->setObjectName(QStringLiteral("statusScale"));
    scaleLabel->setFont(PaleoTheme::monoFont()); // T32：比例尺读数是数字面
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

    // §4 预览壳：坐标读数旁标明坐标系——与 PDF 页脚（mapexport.cpp）同一句
    // 「工程坐标 · 米 · 未投影」（DESIGN.md 状态文字 #5D6E80，次级文案同色）。
    auto *crsLabel = new QLabel(this);
    crsLabel->setObjectName(QStringLiteral("statusCrs"));
    crsLabel->setText(QStringLiteral("工程坐标 · 米 · 未投影"));
    crsLabel->setToolTip(QStringLiteral("局部工程坐标，单位米，未投影 — 不是经纬度"));
    crsLabel->setStyleSheet(QStringLiteral("color: #5D6E80;"));
    statusBar()->addPermanentWidget(crsLabel);
  }
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, horizonLabel,
            [horizonLabel, horizonText](const QString &h) { horizonLabel->setText(horizonText(h)); });

  // DESIGN.md tokens on shell chrome only — no custom painting. ribbon 本体
  // 的颜色来自 PaleoTheme::ribbonPaletteJson（office2021 模板）。
  // T32：拼上全局 2px #1B73D0 键盘焦点环（替代 Fusion 虚线框）。
  const QString shellQss =
      QStringLiteral(
          "QMainWindow { background: #EDF1F5; }"
          "QDockWidget::title { background: #EDF1F5; color: #24303E; padding: 6px 10px; }"
          "QStatusBar { background: #EDF1F5; color: #5D6E80; }"
          "QWidget#horizonChipRow { background: #FFFFFF; border-bottom: 1px solid #DFE5EC; }") +
      PaleoTheme::focusRingStyleSheet();
  PaleoRibbon::applyTheme(this, shellQss);
  // SARibbonMainWindow 构造时排了一次 singleShot(0) 重套内置主题（会整体
  // 覆盖样式表）；零时定时器按注册顺序触发——这里再排一次，保证最后落的
  // 是 DESIGN.md 这套。
  QTimer::singleShot(0, this, [this, shellQss] { PaleoRibbon::applyTheme(this, shellQss); });
}

void PaleoMainWindow::buildRibbon()
{
  SARibbonBar *bar = ribbonBar();
  if (!bar)
    return;
  bar->setRibbonStyle(SARibbonBar::RibbonStyleCompactThreeRow);
  bar->setTabDoubleClickToMinimumMode(true); // 双击页签收起/展开 ribbon（Office 惯例）
  if (SARibbonTabBar *tabs = bar->ribbonTabBar())
  {
    tabs->setObjectName(QStringLiteral("workflowTabs"));
    tabs->setAccessibleName(QStringLiteral("工作流步骤"));
    // T32 tab 溢出策略：超宽走滚动按钮（显式钉住防样式/平台漂移）。
    tabs->setUsesScrollButtons(true);
  }

  // ---- 五个页签 = 五个工作流页（页签序 = kPageIds 序）----
  for (int i = 0; i < kPageIds.size(); ++i)
  {
    SARibbonCategory *cat = bar->addCategoryPage(kPageLabels.at(i));
    cat->setObjectName(QStringLiteral("ribbonCategory.") + kPageIds.at(i));
    cat->setProperty("paleo.pageId", kPageIds.at(i));
  }
  connect(bar, &SARibbonBar::currentRibbonTabChanged, this, [this, bar](int idx) {
    SARibbonCategory *cat = bar->categoryByIndex(idx);
    const QString id = cat ? cat->property("paleo.pageId").toString() : QString();
    if (!id.isEmpty() && id != m_currentPage)
      showPage(id);
  });

  // ---- 「文件」应用按钮：工程级动作。起始页按钮是同一批动作的另一入口
  // （点它们的按钮 = 同一条代码路径，offscreen 下同样惰性）。----
  if (auto *appBtn = qobject_cast<QToolButton *>(bar->applicationButton()))
  {
    appBtn->setText(tr("文件"));
    appBtn->setAccessibleName(tr("文件菜单"));
    auto *menu = new SARibbonMenu(appBtn);
    menu->setObjectName(QStringLiteral("fileMenu"));
    const auto viaStartup = [this, menu](const QString &text, const char *iconName,
                                         const char *buttonName) {
      QAction *a = menu->addAction(PaleoIcons::qgisTheme(QLatin1String(iconName)), text);
      connect(a, &QAction::triggered, this, [this, buttonName] {
        if (auto *b = findChild<QPushButton *>(QLatin1String(buttonName)))
          b->click();
      });
    };
    viaStartup(tr("新建工程…"), "mActionFileNew.svg", "newProjectButton");
    viaStartup(tr("打开工程…"), "mActionFileOpen.svg", "openProjectButton");
    viaStartup(tr("从工区文件夹新建…"), "mIconFolderOpen.svg", "importFromFolderButton");
    menu->addSeparator()->setObjectName(QStringLiteral("fileMenuSaveAnchor"));
    QAction *home =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mIconFolderHome.svg")), tr("起始页"));
    connect(home, &QAction::triggered, this, [this] { showStartup(); });
    menu->addSeparator();
    QAction *quit =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileExit.svg")), tr("退出"));
    connect(quit, &QAction::triggered, this, &QWidget::close);
    appBtn->setMenu(menu);
    appBtn->setPopupMode(QToolButton::InstantPopup);
  }

  // ---- 右侧全局按钮组：[搜索][处理算法][面板][Web 服务] ----
  // 搜索（QgsLocatorWidget）与处理算法依赖服务，attachWorkflows 里补进来。
  SARibbonButtonGroupWidget *right = bar->rightButtonGroup();
  right->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *locatorSlot = new QWidget(right);
  locatorSlot->setObjectName(QStringLiteral("locatorSlot"));
  auto *slotLay = new QHBoxLayout(locatorSlot);
  slotLay->setContentsMargins(0, 1, 6, 1);
  right->addWidget(locatorSlot);

  // 面板管理入口（右键 dock 标题栏是同一菜单——contextMenuEvent）。菜单
  // 每次点击现建——createPopupMenu 反映当下 dock 集。
  auto *panelsBtn = new QToolButton(right);
  panelsBtn->setObjectName(QStringLiteral("panelsMenuButton"));
  panelsBtn->setText(tr("面板"));
  panelsBtn->setAccessibleName(tr("面板显隐菜单"));
  panelsBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionShowAllLayers.svg")));
  panelsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(panelsBtn, &QToolButton::clicked, this, [this, panelsBtn] {
    showPanelMenu(panelsBtn->mapToGlobal(QPoint(0, panelsBtn->height())));
  });
  right->addWidget(panelsBtn);

  // Web 服务 dock 的 toggleViewAction：dock 标题栏 ✕ 关掉时按钮态跟随。
  if (auto *webDock = findChild<QDockWidget *>(QStringLiteral("webServiceDock")))
  {
    QAction *webAct = webDock->toggleViewAction();
    webAct->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionAddWmsLayer.svg")));
    right->addAction(webAct);
    if (auto *webToggle = qobject_cast<QToolButton *>(right->widgetForAction(webAct)))
    {
      webToggle->setObjectName(QStringLiteral("webServiceButton"));
      webToggle->setAccessibleName(tr("Web 服务面板"));
    }
  }
}

SARibbonCategory *PaleoMainWindow::categoryForPage(const QString &pageId) const
{
  SARibbonBar *bar = ribbonBar();
  return bar ? bar->categoryByObjectName(QStringLiteral("ribbonCategory.") + pageId) : nullptr;
}

void PaleoMainWindow::showPanelMenu(const QPoint &globalPos)
{
  // createPopupMenu() 是 QMainWindow 原生面板清单：列出每个 dock 的
  // toggleViewAction（+注册的 QToolBar——本壳没有；编辑条是 ribbon 行内
  // 控件，页作用域归 showPage 管，故意不进可关清单）。菜单生命周期归
  // WA_DeleteOnClose。
  if (QMenu *menu = createPopupMenu())
  {
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(globalPos);
  }
}

void PaleoMainWindow::contextMenuEvent(QContextMenuEvent *event)
{
  // 命中件祖先链上有 QDockWidget 且不落在其内容子树里（标题栏/边距）
  // → 弹面板管理菜单；dock 内容件（图层树、表格…）与画布各有自己的
  // 右键语义，一路放行。
  QWidget *hit = childAt(event->pos());
  if (!hit) // 停靠区空白边距
  {
    showPanelMenu(event->globalPos());
    return;
  }
  for (QWidget *w = hit; w; w = w->parentWidget())
  {
    if (auto *dock = qobject_cast<QDockWidget *>(w))
    {
      if (dock->widget() && dock->widget()->isAncestorOf(hit))
        break;
      showPanelMenu(event->globalPos());
      return;
    }
  }
  SARibbonMainWindow::contextMenuEvent(event);
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

  // 页签 = 页：切到对应 ribbon 页签（currentRibbonTabChanged 回到这里时
  // id == m_currentPage，不重入）。
  if (SARibbonCategory *cat = categoryForPage(pageId))
    if (ribbonBar()->currentIndex() != ribbonBar()->categoryIndex(cat))
      ribbonBar()->raiseCategory(cat);

  if (auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost")))
    if (auto *stack = static_cast<QStackedLayout *>(host->layout()))
      stack->setCurrentIndex(idx);
  if (m_rightDock)
    m_rightDock->setWindowTitle(kPageDockTitles.at(idx));

  // A workflow step implies the workspace: leave the startup page once a
  // project exists (with no project the startup page stays — nothing to show).
  if (m_centerStack && m_centerStack->currentIndex() == 0 && m_projectSvc &&
      !m_projectSvc->projectPath().isEmpty())
    m_centerStack->setCurrentIndex(1);

  // 数据管理页是列表面（数据列表 + 数据预览），其余四页是画布面。井上
  // 图层状态不受影响——回到画布页照常看图。
  if (m_workspaceStack)
    m_workspaceStack->setCurrentIndex(pageId == QLatin1String("data") ? 1 : 0);
  if (m_previewTabs)
    m_previewTabs->setVisible(pageId == QLatin1String("data"));

  // 页作用域工具面：编辑命令组只在编图链三页的 ribbon 里。落到非编辑页
  // 时停用活动画布工具——各工具 deactivate() 统一发 abort 信号，约束捕获/
  // 编辑会话经 owner 的 abort 路径拆台（等价 §42.15 的 Esc）。
  if (!kEditingToolPages.contains(pageId) && m_canvasCtl)
    m_canvasCtl->deactivateTool();

  applyPreviewSplit();
}

void PaleoMainWindow::applyPreviewSplit()
{
  if (!m_centerSplit || !m_previewTabs || m_centerSplit->count() < 2)
    return;
  const bool isHoriz = (m_centerSplit->orientation() == Qt::Horizontal);
  const int total = isHoriz ? m_centerSplit->width() : m_centerSplit->height();
  if (total <= 0 || !m_previewTabs->isVisible())
    return; // 预览藏着的页（或未布局时）：尺寸让给地图，不动分栏

  const auto *inner =
      m_previewTabs->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
  const int tabs = inner ? inner->count() : m_previewTabs->tabCount();
  if (tabs > 0)
  {
    // D7 最大化态：数据列表只留 64px 壳（splitter 会按列表最小尺寸兜底），
    // 预览拿走其余；「还原预览」走 previewMaximizeToggled(false) 恢复
    // m_preMaxSplitSizes。
    if (m_previewMaximized)
    {
      const int listFloor = qMin(64, qMax(1, total / 10));
      m_centerSplit->setSizes({listFloor, qMax(1, total - listFloor)});
      return;
    }
    // D7 预算：首个标签出现时给预览 ≥60%；之后由用户拖分栏，不再触碰。
    if (!m_previewExpanded)
    {
      m_previewExpanded = true;
      if (isHoriz)
      {
        const int leftSize = qMax(1, total * 2 / 5);
        m_centerSplit->setSizes({leftSize, qMax(1, total - leftSize)});
      }
      else
      {
        m_centerSplit->setSizes({qMax(1, total * 2 / 5), qMax(1, total * 3 / 5)});
      }
    }
    return;
  }

  m_previewExpanded = false;
  // 空态：最大化/保存的尺寸一并复位——角落钮随 tabs 隐藏，下次重开按预算走。
  m_preMaxSplitSizes.clear();
  m_previewMaximized = false;
  if (auto *maxBtn =
          m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton")))
    maxBtn->setChecked(false);
  if (isHoriz)
  {
    const int leftSize = qMin(380, qMax(260, total * 2 / 5));
    m_centerSplit->setSizes({leftSize, qMax(1, total - leftSize)});
  }
  else
  {
    // 垂直模式空态只留「预览为空」那行次级文字的高度（≈28px），不再给 1/3。
    const int hint = qMax(24, m_previewTabs->sizeHint().height());
    m_centerSplit->setSizes({qMax(0, total - hint), hint});
  }
}

// ---------------------------------------------------------------------------
// T22 文件夹导入确认表：类型下拉从分类器词表构建（label↔type 稳定映射，
// type 存 Qt::UserRole——不靠显示文本反推）；HZ28-6-1 行锁定为参考；
// 「参考资料」目录内井类/未判内容默认显示「参考」（可改，成 override 送达
// 后端）；覆盖只收「合法且不同于分类器原类型」的行；Failed 行给「重试」。
// ---------------------------------------------------------------------------

QString PaleoMainWindow::folderTypeLabel(const QString &type)
{
  static const QHash<QString, QString> kLabels = {
      {QStringLiteral("well_head"), QStringLiteral("井口")},
      {QStringLiteral("well_log"), QStringLiteral("测井")},
      {QStringLiteral("well_stratification"), QStringLiteral("井分层")},
      {QStringLiteral("time_depth"), QStringLiteral("时深")},
      {QStringLiteral("horizon"), QStringLiteral("层位")},
      {QStringLiteral("seismic"), QStringLiteral("地震")},
      {QStringLiteral("tabular"), QStringLiteral("表格")},
      {QStringLiteral("geojson"), QStringLiteral("GeoJSON")},
      {QStringLiteral("document"), QStringLiteral("文档")},
      {QStringLiteral("image_reference"), QStringLiteral("图像")},
      {QStringLiteral("reference"), QStringLiteral("参考资料")},
      {QStringLiteral("unknown"), QStringLiteral("未知")}};
  return kLabels.value(type, type); // 词表外类型裸显 id（type 仍存 item data）
}

QString PaleoMainWindow::folderRowDisplayType(const QString &path,
                                              const QString &classifiedType)
{
  if (isFixedAuxiliaryPath(path))
    return QStringLiteral("reference"); // HZ28-6-1：固定参考（下拉同时锁死）
  if (isDefaultReferencePath(path))
  {
    // 「参考资料」目录内，分类到井类/未判内容的行默认显示「参考」——确认不改
    // 也作为 override=reference 送达后端，保持阶段 D 语义。document/
    // image_reference/geojson/seismic/horizon 等显示真实类型：它们本来就走
    // 辅助实体，且类型名驱动预览分支（document → PDF 预览）。
    static const QSet<QString> kWellish = {QStringLiteral("well_head"),
                                           QStringLiteral("well_log"),
                                           QStringLiteral("unknown")};
    if (kWellish.contains(classifiedType))
      return QStringLiteral("reference");
  }
  return classifiedType.isEmpty() ? QStringLiteral("unknown") : classifiedType;
}

void PaleoMainWindow::populateFolderConfirmTable(
    QTableWidget *table, const QString &rootDir,
    const QVector<DataImportService::FolderPreviewRow> &rows,
    QVector<QComboBox *> *combosOut)
{
  const QDir root(rootDir);
  const QStringList vocab = projectClassifierTypes();
  table->setRowCount(0);
  if (combosOut)
  {
    combosOut->clear();
    combosOut->reserve(rows.size());
  }
  for (const DataImportService::FolderPreviewRow &row : rows)
  {
    const int r = table->rowCount();
    table->insertRow(r);
    auto *pathItem = new QTableWidgetItem(root.relativeFilePath(row.path));
    pathItem->setToolTip(row.path);
    pathItem->setData(Qt::UserRole, row.path);
    table->setItem(r, 0, pathItem);
    table->setItem(r, 1, new QTableWidgetItem);
    table->setItem(r, 2, new QTableWidgetItem);
    table->setItem(r, 3, new QTableWidgetItem);

    auto *combo = new QComboBox(table);
    combo->setObjectName(QStringLiteral("folderType%1").arg(r));
    for (const QString &t : vocab)
      combo->addItem(folderTypeLabel(t), t); // type 存 data，不靠文本反推
    // 分类器给了词表外类型（未来扩展）→ 追加一项保住真实类型可选。
    if (!row.classifiedType.isEmpty() && !vocab.contains(row.classifiedType))
      combo->addItem(row.classifiedType, row.classifiedType);
    const QString disp = folderRowDisplayType(row.path, row.classifiedType);
    int idx = combo->findData(disp);
    if (idx < 0)
      idx = combo->findData(QStringLiteral("unknown"));
    if (idx >= 0)
      combo->setCurrentIndex(idx);

    // 锁定优先级：HZ28-6-1 固定参考（改不动）；跳过行同样禁改。
    const bool locked = isFixedAuxiliaryPath(row.path);
    if (row.skipped || locked)
      combo->setEnabled(false);
    if (locked)
    {
      combo->setToolTip(tr("该文件固定为参考资料"));
      pathItem->setToolTip(tr("%1\n该文件固定为参考资料").arg(row.path));
    }
    table->setCellWidget(r, 1, combo);
    if (combosOut)
      combosOut->append(combo);
    if (row.skipped)
    {
      table->item(r, 3)->setText(tr("跳过：%1").arg(row.skipReason));
      for (int c = 0; c < 4; ++c)
        table->item(r, c)->setFlags(table->item(r, c)->flags() & ~Qt::ItemIsEnabled);
    }
    else
    {
      // C 包 IngestPlan：plan 期决策逐行可见——重复→跳过 / 重复→新版本；
      // 未决行不在此预写（保持既有口径：结果列导入后才写「未决」）。
      const QString decisionText =
          row.decision == QLatin1String("skip")
              ? tr("重复→跳过")
              : row.decision == QLatin1String("as_new_version")
                    ? tr("重复→新版本")
                    : QString();
      if (!decisionText.isEmpty())
        table->item(r, 3)->setText(decisionText);
    }
  }
}

QMap<QString, QString> PaleoMainWindow::collectFolderTypeOverrides(
    const QTableWidget *table, const QVector<DataImportService::FolderPreviewRow> &rows,
    const QVector<QComboBox *> &combos)
{
  QMap<QString, QString> overrides;
  for (int r = 0; r < combos.size() && r < rows.size(); ++r)
  {
    if (!combos[r] || !combos[r]->isEnabled())
      continue; // 跳过行/锁定行不参与导入，改动也不成 override
    const QString t = combos[r]->currentData().toString(); // item data，非显示文本
    const QString path = table->item(r, 0)->data(Qt::UserRole).toString();
    // 只在「合法类型」且「不同于分类器原类型」时发 override——这样不动
    // 下拉/保持默认的行不发出多余覆盖（参考资料默认「参考」属有意覆盖）。
    if (isClassifierType(t) && t != rows.at(r).classifiedType)
      overrides.insert(path, t);
  }
  return overrides;
}

void PaleoMainWindow::writeFolderRowResult(
    QTableWidget *table, int row, const DataImportService::FolderRowResult &res,
    const std::function<void(int)> &onRetry)
{
  using Outcome = DataImportService::FolderRowResult::Outcome;
  QString outcomeText;
  switch (res.outcome)
  {
  case Outcome::Imported:
    outcomeText = tr("已入库");
    break;
  case Outcome::Unresolved:
    outcomeText = tr("未决");
    break;
  case Outcome::Failed:
    outcomeText = tr("失败");
    break;
  case Outcome::Skipped:
    outcomeText = tr("跳过");
    break;
  }
  const QString text =
      res.message.isEmpty() ? outcomeText : tr("%1：%2").arg(outcomeText, res.message);
  table->item(row, 2)->setText(res.entityName);
  // 清掉旧的重试控件——removeCellWidget 只摘不删，控件会活成表内孤儿。
  if (QWidget *old = table->cellWidget(row, 3))
  {
    table->removeCellWidget(row, 3);
    old->setParent(nullptr);
    old->deleteLater();
  }
  table->item(row, 3)->setText(text);
  if (res.outcome == Outcome::Failed && onRetry)
  {
    // 失败行的「重试」：按当前下拉类型只重导这一行。
    auto *cell = new QWidget(table);
    auto *hl = new QHBoxLayout(cell);
    hl->setContentsMargins(4, 0, 4, 0);
    auto *msg = new QLabel(text, cell);
    msg->setWordWrap(true);
    auto *retry = new QPushButton(tr("重试"), cell);
    retry->setObjectName(QStringLiteral("folderRetry"));
    retry->setAccessibleName(tr("重试导入 %1").arg(table->item(row, 0)->text()));
    hl->addWidget(msg, 1);
    hl->addWidget(retry, 0);
    QObject::connect(retry, &QPushButton::clicked, table,
                     [onRetry, row] { onRetry(row); });
    table->setCellWidget(row, 3, cell);
  }
}

QString PaleoMainWindow::folderImportSummaryText(
    const QVector<DataImportService::FolderRowResult> &rows)
{
  using Outcome = DataImportService::FolderRowResult::Outcome;
  int imported = 0, unresolved = 0, failed = 0, skipped = 0;
  for (const auto &res : rows)
    switch (res.outcome)
    {
    case Outcome::Imported:
      ++imported;
      break;
    case Outcome::Unresolved:
      ++unresolved;
      break;
    case Outcome::Failed:
      ++failed;
      break;
    case Outcome::Skipped:
      ++skipped;
      break;
    }
  QString text = tr("入库 %1，未决 %2，失败 %3").arg(imported).arg(unresolved).arg(failed);
  if (skipped > 0) // D3：「跳过」保留为第四计数（符号链接/非普通文件如实报）
    text += tr("，跳过 %1").arg(skipped);
  return text;
}

void PaleoMainWindow::buildFolderConfirmDialog(
    QDialog *dlg, DataImportService *svc, const QString &dir,
    const QVector<DataImportService::FolderPreviewRow> &preview, PaleoMainWindow *self)
{
  dlg->setObjectName(QStringLiteral("folderImportDialog"));
  dlg->setWindowTitle(tr("导入工区文件夹 — %1").arg(dir));
  dlg->resize(760, 420);
  auto *lay = new QVBoxLayout(dlg);
  auto *hint = new QLabel(tr("确认每个文件的类型（可改）后导入；井口文件会先入库。"), dlg);
  hint->setWordWrap(true);
  lay->addWidget(hint);
  // T22：CRS 契约句——只读一行，挂在确认表上方。
  auto *crsNote = new QLabel(kEngineeringCrsSentence, dlg);
  crsNote->setObjectName(QStringLiteral("folderCrsNote"));
  crsNote->setWordWrap(true);
  crsNote->setStyleSheet(QStringLiteral("color: #5D6E80;")); // DESIGN.md text-muted
  lay->addWidget(crsNote);

  auto *table = new QTableWidget(0, 4, dlg);
  table->setObjectName(QStringLiteral("folderTable"));
  // T32 a11y：文件夹确认表报名 + 说明（每行可改类型、锁死行只读）。
  table->setAccessibleName(tr("文件夹导入确认表"));
  table->setAccessibleDescription(
      tr("列出所选文件夹里的每个文件：确认或修改类型后导入，井口文件先入库"));
  table->setHorizontalHeaderLabels({tr("路径"), tr("类型"), tr("实体"), tr("结果")});
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  lay->addWidget(table);

  QVector<QComboBox *> combos;
  populateFolderConfirmTable(table, dir, preview, &combos);

  auto *summary = new QLabel(dlg);
  summary->setObjectName(QStringLiteral("folderSummary"));
  summary->setWordWrap(true);
  summary->hide();
  lay->addWidget(summary);

  auto *buttons = new QDialogButtonBox(dlg);
  auto *confirm = buttons->addButton(tr("确认导入"), QDialogButtonBox::AcceptRole);
  confirm->setObjectName(QStringLiteral("folderConfirmButton"));
  auto *cancel = buttons->addButton(tr("取消"), QDialogButtonBox::RejectRole);
  // T31「查看未决」：导入完成后出现——把数据页资产表过滤到未决行，直接
  // 指向「挂到这口井」的挂接入口（不留「导完了然后呢」的断头路）。
  auto *showUnresolved =
      buttons->addButton(tr("查看未决"), QDialogButtonBox::ActionRole);
  showUnresolved->setObjectName(QStringLiteral("folderShowUnresolvedButton"));
  showUnresolved->setVisible(false);
  showUnresolved->setAccessibleName(tr("查看未决资产"));
  QObject::connect(showUnresolved, &QAbstractButton::clicked, dlg, [dlg, self]() {
    if (self)
    {
      self->showPage(QStringLiteral("data"));
      if (auto *page = self->findChild<DataPage *>())
        page->setUnresolvedFilter(true);
    }
    dlg->accept();
  });
  QObject::connect(cancel, &QAbstractButton::clicked, dlg, &QDialog::reject);

  // 对话框 exec 在 build 返回之后——行结果/重试回调的生存期挂到 shared 状态，
  // 不捕局部引用。
  auto results = std::make_shared<QVector<DataImportService::FolderRowResult>>();
  auto retryFn = std::make_shared<std::function<void(int)>>();
  const std::function<void(int)> retryCb =
      [retryFn](int r) { if (*retryFn) (*retryFn)(r); };

  // 行重试：只重导这一行的文件，类型取当前下拉值（合法且不同于分类器原类
  // 型才成 override；锁定/灰显行不带覆盖）。
  *retryFn = [retryFn, retryCb, svc, table, summary, combos, preview, results,
              self](int r) {
    if (r < 0 || r >= preview.size() || r >= results->size())
      return;
    const QString path = table->item(r, 0)->data(Qt::UserRole).toString();
    QString force;
    if (combos.value(r) && combos[r]->isEnabled())
    {
      const QString t = combos[r]->currentData().toString();
      if (isClassifierType(t) && t != preview.at(r).classifiedType)
        force = t;
    }
    if (self)
      self->m_folderImportActive = true;
    QString rerr;
    const DataImportService::FolderRowResult rowRes =
        svc->importFolderRow(path, force, &rerr);
    if (self)
      self->m_folderImportActive = false;
    (*results)[r] = rowRes;
    writeFolderRowResult(table, r, rowRes, retryCb); // 仍失败 → 重试按钮回挂
    // 行不再是失败：收掉改类型入口；仍失败的保留下拉（可换类型再试）。
    if (rowRes.outcome != DataImportService::FolderRowResult::Outcome::Failed &&
        combos.value(r))
      combos[r]->setEnabled(false);
    summary->setText(folderImportSummaryText(*results));
  };

  QObject::connect(confirm, &QAbstractButton::clicked, dlg,
                   [dlg, svc, dir, table, summary, confirm, cancel, showUnresolved,
                    combos, preview, results, retryCb, self]() {
    const QMap<QString, QString> overrides =
        collectFolderTypeOverrides(table, preview, combos);
    confirm->setEnabled(false); // 确认只走一遍（异步在途也一样）

    // 导入结果回表——同步路径与任务终态共用（在 GUI 线程执行）。
    const auto applyResults =
        [dlg, svc, dir, table, summary, confirm, cancel, showUnresolved, combos,
         preview, results, retryCb,
         self](const QVector<DataImportService::FolderRowResult> &res,
               const QString &importErr) {
      if (res.isEmpty() && !importErr.isEmpty())
      {
        QMessageBox::warning(dlg, tr("导入工区文件夹"), importErr);
        confirm->setEnabled(true); // 整体失败可重试
        return;
      }
      // D5：结果序按生效类型两阶段排——改过类型的行可能换阶段，按「路径」
      // 回行而不是按索引；results 与表行同序存放，供重试回写与汇总重算。
      results->fill(DataImportService::FolderRowResult{}, preview.size());
      using Outcome = DataImportService::FolderRowResult::Outcome;
      QString wellHeadAssetId;
      for (const DataImportService::FolderRowResult &rowRes : res)
      {
        int r = -1;
        for (int i = 0; i < preview.size(); ++i)
          if (preview.at(i).path == rowRes.path)
          {
            r = i;
            break;
          }
        if (r < 0)
          continue;
        (*results)[r] = rowRes;
        writeFolderRowResult(table, r, rowRes, retryCb);
        if (rowRes.outcome == Outcome::Imported &&
            rowRes.classifiedType == QLatin1String("well_head") &&
            wellHeadAssetId.isEmpty())
        {
          // 找回刚入库的井口资产：按文件名在 catalog 里定位。
          if (auto *cat = svc->catalog())
            for (const auto &a : cat->assets())
              if (a.type == QLatin1String("well_head") &&
                  QFileInfo(rowRes.path).fileName() ==
                      cat->currentVersion(a.id).fileName)
                wellHeadAssetId = a.id;
        }
      }
      summary->setText(folderImportSummaryText(*results));
      summary->show();
      // 有未决行才露「查看未决」入口（T31）。
      bool anyUnresolved = false;
      for (const auto &rowRes : *results)
        if (rowRes.outcome == Outcome::Unresolved)
          anyUnresolved = true;
      showUnresolved->setVisible(anyUnresolved);
      // 结果留在表里给用户过目；仍失败的行保留下拉（可换类型再点「重试」），
      // 其余行锁定。
      for (int r = 0; r < combos.size(); ++r)
        if (r >= results->size() ||
            results->at(r).outcome != Outcome::Failed)
          combos[r]->setEnabled(false);
      cancel->setText(tr("关闭"));
      if (self && !wellHeadAssetId.isEmpty())
        QMetaObject::invokeMethod(
            self,
            [self, wellHeadAssetId] {
              if (self->m_previewTabs)
                self->m_previewTabs->openAsset(wellHeadAssetId);
            },
            Qt::QueuedConnection);
      // PROJECT_FILE_DESIGN：「从工区文件夹新建」的工程把本次导入统计写回
      // project.paleo.sourceArea（目录不匹配时 stampSourceArea 自拒，不污染
      // 普通导入路径下的工程）。
      if (self)
      {
        int nImported = 0, nUnresolved = 0, nFailed = 0, nSkipped = 0;
        for (const auto &rr : res)
          switch (rr.outcome)
          {
          case Outcome::Imported: ++nImported; break;
          case Outcome::Unresolved: ++nUnresolved; break;
          case Outcome::Failed: ++nFailed; break;
          case Outcome::Skipped: ++nSkipped; break;
          }
        self->stampSourceArea(dir, QVariantMap{
            {QStringLiteral("files"), res.size()},
            {QStringLiteral("imported"), nImported},
            {QStringLiteral("unresolved"), nUnresolved},
            {QStringLiteral("failed"), nFailed},
            {QStringLiteral("skipped"), nSkipped}});
      }
    };

    // D1b：任务池在场 → 整个文件夹导入（含 LAS 解析/层位装箱/SEG-Y 索引）
    // 跑 worker 线程，行进度回报到任务页；catalog 操作经服务内 marshal 回
    // GUI。worker 的 imported 信号排队顺序先于 finished——槽里抑制标签的
    // 标志在终态回调复位，不会漏开逐文件标签。对话框中途关闭 → 结果弃置
    // （catalog 状态已入库，可重开表看）。
    if (self && self->m_taskSvc)
    {
      self->m_folderImportActive = true;
      auto outRows = std::make_shared<QVector<DataImportService::FolderRowResult>>();
      auto outErr = std::make_shared<QString>();
      QPointer<QDialog> guard(dlg);
      PaleoTask *task = self->m_taskSvc->start(
          tr("导入工区文件夹"),
          [svc, dir, overrides, outRows, outErr](PaleoTask *t) -> QString {
            *outRows = svc->importFolder(
                dir, outErr.get(), overrides,
                [t](int done, int total, const QString &p) {
                  t->reportBytes(done, total);
                  t->reportDetail(p);
                  return !t->cancelRequested();
                });
            return *outErr;
          });
      QObject::connect(task, &PaleoTask::finished, self,
                       [self, guard, applyResults, outRows, outErr] {
                         self->m_folderImportActive = false;
                         if (guard)
                           applyResults(*outRows, *outErr);
                       });
      return;
    }

    QString importErr;
    if (self)
      self->m_folderImportActive = true;
    const auto res = svc->importFolder(dir, &importErr, overrides);
    if (self)
      self->m_folderImportActive = false;
    applyResults(res, importErr);
  });
  lay->addWidget(buttons);
}

void PaleoMainWindow::runFolderImport(DataImportService *svc)
{
  if (!svc)
    return;
  const QString dir =
      QFileDialog::getExistingDirectory(this, tr("导入工区文件夹"));
  if (dir.isEmpty())
    return;
  runFolderImportAt(svc, dir);
}

void PaleoMainWindow::runFolderImportAt(DataImportService *svc,
                                        const QString &dir)
{
  if (!svc)
    return;
  QString err;
  const auto preview = svc->previewFolder(dir, &err);
  if (preview.isEmpty())
  {
    QMessageBox::warning(this, tr("导入工区文件夹"),
                         err.isEmpty() ? tr("目录里没有可导入的文件") : err);
    return;
  }

  QDialog dlg(this);
  buildFolderConfirmDialog(&dlg, svc, dir, preview, this);
  dlg.exec();
}

void PaleoMainWindow::stampSourceArea(const QString &dir,
                                      const QVariantMap &stats)
{
  if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
    return;
  // 只回填「从工区文件夹新建」的工程（工程目录 == 导入目录）；否则静默跳过——
  // 普通「导入工区文件夹」不应改写一个不相关工程的来源字段。
  const QString projDir =
      QFileInfo(m_projectSvc->projectPath()).absolutePath();
  if (QDir(projDir) != QDir(dir))
    return;
  const QString paleo = paleoProjectFilePath(projDir);
  if (!QFile::exists(paleo))
    return;
  bool ok = false;
  QString rerr;
  PaleoProjectFile pf = readProjectFile(paleo, &ok, &rerr);
  if (!ok)
  {
    qWarning() << "paleomainwindow: read project file failed:" << rerr;
    return;
  }
  pf.sourceAreaRoot = dir;
  pf.sourceAreaImportedUtc =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  pf.sourceStats = stats;
  QString werr;
  if (!writeProjectFile(projDir, pf, &werr))
    qWarning() << "paleomainwindow: write project file failed:" << werr;
}

void PaleoMainWindow::flashHorizonLayer(QgsMapLayer *layer)
{
  if (!m_canvasCtl || !layer)
    return;
  QgsMapCanvas *cv = m_canvasCtl->canvas();
  // 闪烁定位（T29 spec ~300–500ms）：#1B73D0 半透明多边形橡皮带盖住图层
  // 范围，100ms 一闪 ×4 后自毁。交互蓝只做交互反馈，不做常驻装饰
  // （DESIGN.md：交互色不兼装饰）。
  auto *band = new QgsRubberBand(cv, Qgis::GeometryType::Polygon);
  band->setToGeometry(QgsGeometry::fromRect(layer->extent()),
                      qobject_cast<QgsVectorLayer *>(layer));
  band->setColor(QColor(27, 115, 208, 60)); // #1B73D0 @ ~24% 填充透明度
  band->setStrokeColor(QColor(QStringLiteral("#1B73D0")));
  band->setWidth(2);
  setProperty("horizonFlashActive", true);
  auto *timer = new QTimer(this);
  timer->setObjectName(QStringLiteral("horizonFlashTimer"));
  int blinks = 4;
  connect(timer, &QTimer::timeout, this, [this, timer, band, blinks]() mutable {
    band->setVisible(band->isVisible() ? false : true);
    if (--blinks <= 0)
    {
      timer->stop();
      timer->deleteLater();
      delete band; // 画布条目直接删——不在信号发送者栈上
      setProperty("horizonFlashActive", false);
    }
  });
  timer->start(100);
}

bool PaleoMainWindow::openPath(const QString &path)
{
  if (path.isEmpty() || !m_projectSvc)
    return false;
  const QFileInfo fi(path);
  if (!fi.exists())
  {
    if (!isOffscreen())
      QMessageBox::warning(this, tr("打开工程"), tr("路径不存在: %1").arg(path));
    return false;
  }
  if (fi.isFile())
  {
    if (!m_projectSvc->openProject(fi.absoluteFilePath()))
    {
      if (!isOffscreen())
        QMessageBox::critical(this, tr("打开工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
      return false;
    }
    return true;
  }
  if (fi.isDir())
  {
    const QString dir = fi.absoluteFilePath();
    const QString paleo = paleoProjectFilePath(dir);
    if (QFile::exists(paleo))
    {
      if (!m_projectSvc->openProject(paleo))
      {
        if (!isOffscreen())
          QMessageBox::critical(this, tr("打开工程失败"),
                                m_projectSvc->lastErrors().join(QLatin1Char('\n')));
        return false;
      }
      return true;
    }
    const QString qgz = QDir(dir).filePath(
        QFileInfo(dir).fileName() + QStringLiteral(".qgz"));
    if (QFile::exists(qgz) || !QDir(dir).entryList(QStringList{QStringLiteral("*.qgz")}).isEmpty())
    {
      const QString adopt = QFile::exists(qgz)
          ? qgz
          : QDir(dir).filePath(
                QDir(dir).entryList(QStringList{QStringLiteral("*.qgz")}).first());
      if (!m_projectSvc->openProject(adopt))
      {
        if (!isOffscreen())
          QMessageBox::critical(this, tr("打开工程失败"),
                                m_projectSvc->lastErrors().join(QLatin1Char('\n')));
        return false;
      }
      return true;
    }
    if (!m_projectSvc->createProject(qgz))
    {
      if (!isOffscreen())
        QMessageBox::critical(this, tr("新建工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
      return false;
    }
    if (QFile::exists(paleoProjectFilePath(dir)))
    {
      bool ok = false;
      QString perr;
      PaleoProjectFile pf = readProjectFile(paleoProjectFilePath(dir), &ok, &perr);
      if (ok)
      {
        pf.sourceAreaRoot = dir;
        QString werr;
        if (!writeProjectFile(dir, pf, &werr))
          qWarning() << "paleomainwindow: stamp sourceArea failed:" << werr;
      }
    }
    if (m_importSvc)
      runFolderImportAt(m_importSvc, dir);
    else if (!isOffscreen())
      QMessageBox::information(
          this, tr("从工区文件夹新建"),
          tr("工程已创建于 %1；导入服务未就绪，请在数据页手动导入该文件夹。").arg(dir));
    return true;
  }
  return false;
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

  // 捕捉配置镜像进工程（QGIS_NATIVE_ADOPTION）：随 .qgz 持久化；画布侧
  // 配置在 canvas() 创建时已装到 snappingUtils（同一 nativeSnappingConfig）。
  if (m_projectSvc && m_projectSvc->project())
    m_projectSvc->project()->setSnappingConfig(
        QgisCanvasController::nativeSnappingConfig());

  restoreCanvasExtent(); // per-project display state from the .qgz

  // 工程级参数驱动的层位 UI（AreaRules 已在 AppContext::projectOpened 装载）：
  // chip 条按新词表重建；标定层位相关文案重写。
  if (auto *bar = findChild<HorizonChipBar *>())
    bar->reloadHorizons();
  {
    const QString tgt = AreaRules::active().targetHorizon;
    const QString base = baseHorizonFor(tgt);
    if (auto *l = findChild<QLabel *>(QStringLiteral("thicknessCaption")))
      l->setText(tr("%1→%2 厚度样本").arg(tgt, base));
    if (auto *t = findChild<QTableWidget *>(QStringLiteral("thicknessTable")))
      t->setHorizontalHeaderLabels(
          {tr("井名"), tr("%1 TVD").arg(tgt), tr("%1 TVD").arg(base),
           tr("层间速度或原因")});
    if (auto *l = findChild<QLabel *>(QStringLiteral("residualCaption")))
      l->setText(tr("%1 时间残差").arg(tgt));
    if (auto *l = findChild<QLabel *>(QStringLiteral("residualSummaryLabel")))
      if (l->text().startsWith(QString::fromUtf8("还没有计算")))
        l->setText(tr("还没有计算 %1 残差").arg(tgt));
    if (auto *t = findChild<QTableWidget *>(QStringLiteral("residualTable")))
      t->setAccessibleName(tr("%1 残差表").arg(tgt));
    if (auto *l = findChild<QLabel *>(QStringLiteral("onnxGridCaption")))
      l->setText(tr("输出固定为 %1 工区网格 %2×%3")
                     .arg(tgt)
                     .arg(AreaRules::active().onnxGrid.rows)
                     .arg(AreaRules::active().onnxGrid.cols));
  }

  // Workflow lands on the last-used page when the session was persisted,
  // otherwise on 数据管理 — first step of the chain.
  const QString last = QSettings(QStringLiteral("paleo"), QStringLiteral("paleo"))
                           .value(QStringLiteral("lastPage")).toString();
  showPage(kPageIds.contains(last) ? last : kPageIds.first());
}

void PaleoMainWindow::closeEvent(QCloseEvent *event)
{
  saveWindowState();
  SARibbonMainWindow::closeEvent(event);
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
                                      QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
                                      PaleoTaskService *taskSvc)
{
  // 幂等守卫：本函数不是增量接线，而是整建——清空右栏页面栈重建、往
  // bottomTabs/状态栏加面板（correlationPanel、releasePanel、任务/属性表、
  // statusCatalogError…）、在 topBar 建按钮（processingButton、designerButton…）
  // 并往 importSvc/preview/pages 上叠信号连接。同一窗口二次执行会重复建
  // dock/按钮（各对象双份），且旧页面 deleteLater 后残留的信号捕获会悬空，
  // 后续用例段错误。测试套件会二次触达同一窗口（tst_ui 单跑用例的补调
  // 路径 + attachWorkflowsIsIdempotent 直证），因此首次完整接线后早退；
  // rightPanelHost 缺席的早退不算完成，不置位。
  if (m_workflowsAttached)
    return;
  auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost"));
  auto *stack = host ? static_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack)
    return;
  m_taskSvc = taskSvc; // D1b：导入任务池（nullptr 时保持同步旧路径）
  m_importSvc = importSvc; // 「从工区文件夹新建」直达入口

  // Replace placeholders in page order (data, predict, constraint, compose, validate).
  while (stack->count() > 0)
  {
    QLayoutItem *item = stack->takeAt(0);
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }

  // 数据管理页（ribbon 布局）：DataPage 本体进中央「数据列表」；实体数据
  // 视图段挂到右 dock 第 0 页「数据属性」；导入段藏起——导入命令在 ribbon
  // 「数据导入」组（镜像这些按钮，门控/原因仍写在按钮上）。
  auto *dataPage = new DataPage(m_dataListHost ? m_dataListHost : host);
  auto *dataProps = new QWidget(host);
  dataProps->setObjectName(QStringLiteral("dataPropertiesPage"));
  {
    auto *pl = new QVBoxLayout(dataProps);
    pl->setContentsMargins(8, 8, 8, 8); // spacing.sm
    if (m_dataListHost)
    {
      m_dataListHost->layout()->addWidget(dataPage);
      if (QWidget *entity = dataPage->entityViewSection())
        pl->addWidget(entity, 1); // 重新挂父：刷新按段查找，不受影响
      if (auto *imports = dataPage->findChild<QWidget *>(QStringLiteral("dataImportSection")))
        imports->hide();
    }
    else
      pl->addWidget(dataPage);
  }
  auto *predictPage = new PredictPage(pred, m_layerSvc, host);
  auto *constraintPage = new ConstraintPage(constraint, host);
  auto *composePage = new ComposePage(compose, m_layerSvc, host);
  auto *validatePage = new ValidatePage(validate, host);
  stack->addWidget(dataProps);
  stack->addWidget(predictPage);
  stack->addWidget(constraintPage);
  stack->addWidget(composePage);
  stack->addWidget(validatePage);

  // 连井剖面 — bottom-dock tab fed by the import pipeline。地震底栏预览
  // 已随 §4 预览壳重排移除（测线预览由数据页预览壳承担，见
  // seismicSectionRequested 接线）；seismicLink 暂留签名内兼容调用方。
  Q_UNUSED(seismicLink);
  WellCorrelationPanel *corrPanel = nullptr;
  if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
  {
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
  {
    // §3/§4 数据契约接线：数据页绑定导入服务，资产表跟 catalog 走，
    // 列表选中在中央预览标签（地图下方分栏）打开（确认入库后才开标签）。
    dataPage->setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(importSvc));
    connect(importSvc->catalog(), &DataCatalog::changed, this,
            [dataPage]() { QMetaObject::invokeMethod(dataPage, "refreshAssetTable"); });
    dataPage->refreshAssetTable();
    // D6 地图→表联动：画布上拾取的实体（WellMapLink → ctx）→ 资产表选中
    // 其已决关联的行；选中走同一条 assetActivated → 预览照开。
    if (m_selection)
      connect(m_selection, &SelectionContext::selectionChanged, dataPage,
              [dataPage](const QStringList &ids, const QString &) {
                dataPage->selectAssetsForEntities(ids);
              });

    // ---- T20 余项：catalogOpenFailed 的状态栏露出 ----
    // 常驻红胶囊（DESIGN.md error token）写打开失败原因；恢复（工程重开且
    // catalog 打开成功）前，数据页的导入类动作保持禁用——失败的 catalog 拒
    // 绝写入，导入必然失败，不给用户一个假入口。
    auto *catalogError =
        PaleoTheme::capsuleLabel(QString(), PaleoTheme::CapsuleKind::Error, this);
    catalogError->setObjectName(QStringLiteral("statusCatalogError"));
    catalogError->hide();
    statusBar()->addPermanentWidget(catalogError);
    const auto setImportsEnabled = [this](bool enabled) {
      for (const char *name : {"importWells", "importSeismic", "importBoundary",
                               "importFolder"})
        if (auto *btn = findChild<QPushButton *>(QLatin1String(name)))
        {
          btn->setEnabled(enabled);
          if (!enabled)
            btn->setToolTip(tr("数据目录打开失败 — 导入暂不可用（见状态栏）"));
          else
            btn->setToolTip(QString());
        }
    };
    connect(importSvc, &DataImportService::catalogOpenFailed, this,
            [catalogError, setImportsEnabled](const QString &error) {
              catalogError->setText(tr("数据目录打开失败：%1").arg(error));
              catalogError->show();
              setImportsEnabled(false);
            });
    // 恢复：projectOpened 后（AppContext 已同步重设 projectDir）错误清空 →
    // 收告警、放开导入。
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::projectOpened, this,
              [dataPage, importSvc, catalogError, setImportsEnabled](const QString &) {
                if (importSvc->catalogOpenError().isEmpty())
                {
                  catalogError->hide();
                  setImportsEnabled(true);
                }
                else
                  setImportsEnabled(false); // 换了个工程仍然失败 → 保持禁用
                dataPage->refreshAssetTable();
              });
    DataPreviewTabs *preview = m_previewTabs;
    if (preview)
    {
      preview->setImportService(importSvc);
      preview->setTaskService(taskSvc); // D1：剖面索引/解码异步化（nullptr 时保持同步）
      // D11 临时配准：GeoJSON 标签手工仿射 → DERIVED + 水印图层。
      connect(preview, &DataPreviewTabs::provisionalRegistrationRequested, this,
              [this, importSvc](const QString &assetId, const QVariantMap &params) {
                applyProvisionalRegistration(importSvc, assetId, params);
              });
      connect(dataPage, &DataPage::assetActivated, preview, &DataPreviewTabs::openAsset);
      connect(dataPage, &DataPage::assetWellActivated, preview, &DataPreviewTabs::openAssetForWell);
      connect(dataPage, &DataPage::seismicLineActivated, preview,
              [preview](const QString &aid, const QString &mode) {
                preview->openSeismicLine(aid, mode, 0, 0.0);
              });
      // well_head 预览 / 井树选中 → 地图高亮该井（§4；Direction B 经 SelectionContext）。
      if (m_selection)
      {
        connect(dataPage, &DataPage::wellSelected, this,
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datatree"));
                });
        connect(preview, &DataPreviewTabs::wellSelected, this,
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datapreview"));
                });
      }
      // horizon 预览「在地图上显示」（§4/T29 双向同步）：实例化派生栅格 →
      // 缩放到该图层 → 闪烁定位 ~400ms → 勾上图层树节点 → 按钮置「已在
      // 地图上」；图层树里取消勾选时按钮态跟随（node visibilityChanged，
      // QGIS 4：可见性归图层树管，不在 QgsMapLayer 上）。
      if (m_layerSvc)
        connect(preview, &DataPreviewTabs::showHorizonOnMapRequested, this,
                [this, preview](const QString &layerId) {
                  QString err;
                  QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
                  if (!layer)
                  {
                    QgsMessageLog::logMessage(tr("Show on map failed: %1").arg(err),
                                              QStringLiteral("Paleo"), Qgis::Critical);
                    return;
                  }
                  if (m_canvasCtl)
                  {
                    m_canvasCtl->zoomToLayer(layerId);
                    flashHorizonLayer(layer);
                  }
                  QgsProject *proj =
                      m_projectSvc ? m_projectSvc->project() : nullptr;
                  if (QgsLayerTreeLayer *node =
                          proj ? proj->layerTreeRoot()->findLayer(layer->id())
                               : nullptr)
                  {
                    node->setItemVisibilityChecked(true); // 显示意图（可能已在）
                    // 重复点击同一图层不叠加 connect：节点属性作去重标记
                    // （UniqueConnection 只支持成员函数槽，lambda 不可用）。
                    if (!node->property("paleo.visSync").toBool())
                    {
                      node->setProperty("paleo.visSync", true);
                      connect(node, &QgsLayerTreeNode::visibilityChanged, preview,
                              [preview, layerId](QgsLayerTreeNode *n) {
                                preview->setHorizonOnMap(
                                    layerId, n->itemVisibilityChecked());
                              });
                    }
                  }
                  preview->setHorizonOnMap(layerId, true);
                });
    }
    connect(dataPage, &DataPage::importRequested, this,
            [this, importSvc, preview](const QString &kind) {
              if (kind == QLatin1String("folder"))
              {
                runFolderImport(importSvc);
                return;
              }
              const QString path = QFileDialog::getOpenFileName(
                  this, tr("Import %1").arg(kind), QString(),
                  kind == QLatin1String("seismic")
                      ? tr("Seismic/vector files (*.sgy *.segy *.las *.csv *.gpkg *.shp);;All files (*)")
                      : tr("Vector/log files (*.las *.csv *.gpkg *.shp *.tif *.img);;All files (*)"));
              if (path.isEmpty())
                return;
              // T22：单文件导入同样展示 CRS 契约句（确认一步，含识别类型）。
              {
                QDialog confirmDlg(this);
                confirmDlg.setObjectName(QStringLiteral("singleImportDialog"));
                confirmDlg.setWindowTitle(tr("导入数据"));
                auto *cl = new QVBoxLayout(&confirmDlg);
                const QString recogType = classifyProjectImport(path).type;
                auto *fileLabel = new QLabel(
                    tr("文件：%1\n识别类型：%2").arg(path, folderTypeLabel(recogType)),
                    &confirmDlg);
                fileLabel->setWordWrap(true);
                cl->addWidget(fileLabel);
                auto *crsNote = new QLabel(kEngineeringCrsSentence, &confirmDlg);
                crsNote->setObjectName(QStringLiteral("singleImportCrsNote"));
                crsNote->setWordWrap(true);
                crsNote->setStyleSheet(QStringLiteral("color: #5D6E80;"));
                cl->addWidget(crsNote);
                auto *bb = new QDialogButtonBox(&confirmDlg);
                bb->addButton(tr("导入"), QDialogButtonBox::AcceptRole);
                bb->addButton(tr("取消"), QDialogButtonBox::RejectRole);
                QObject::connect(bb, &QDialogButtonBox::accepted, &confirmDlg,
                                 &QDialog::accept);
                QObject::connect(bb, &QDialogButtonBox::rejected, &confirmDlg,
                                 &QDialog::reject);
                cl->addWidget(bb);
                if (confirmDlg.exec() != QDialog::Accepted)
                  return;
              }
              // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑——无任务服务时保持
              // 同步旧路径。imported 信号照常排队回 GUI（预览标签在终态后开）。
              if (m_taskSvc)
              {
                auto outErr = std::make_shared<QString>();
                auto outId = std::make_shared<QString>();
                PaleoTask *task = m_taskSvc->start(
                    tr("导入 %1").arg(kind),
                    [importSvc, kind, path, outId, outErr](PaleoTask *) -> QString {
                      *outId = importSvc->importFile(kind, path, outErr.get());
                      return outId->isEmpty() ? *outErr : QString();
                    });
                QObject::connect(task, &PaleoTask::finished, this,
                                 [this, outId, outErr] {
                                   if (outId->isEmpty())
                                     QgsMessageLog::logMessage(
                                         tr("Import failed: %1").arg(*outErr),
                                         QStringLiteral("Paleo"), Qgis::Critical);
                                 });
                return;
              }
              QString err;
              const QString assetId = importSvc->importFile(kind, path, &err);
              if (assetId.isEmpty())
                QgsMessageLog::logMessage(tr("Import failed: %1").arg(err),
                                        QStringLiteral("Paleo"), Qgis::Critical);
            });
  }
    // Imported assets feed the bottom-dock correlation panel and the central
    // preview tabs (§4: the preview tab opens only after the import is
    // confirmed; 地震预览不再走底栏面板，预览壳直接按资产渲染测线控件)。
    DataPreviewTabs *previewForImport = m_previewTabs;
    connect(importSvc, &DataImportService::imported, this,
            [this, importSvc, corrPanel, previewForImport](const QString &kind, const QString &assetId, const QString &) {
              // 文件夹导入期间不逐文件开标签——确认后只开井口标签（§4）。
              if (previewForImport && !m_folderImportActive)
                previewForImport->openAsset(assetId);
              if (corrPanel && kind == QLatin1String("well_log"))
              {
                QList<QPair<QString, QString>> wells;
                for (const QString &id : importSvc->assets(QStringLiteral("well_log")))
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
            [this, constraint, constraintPage](const QString &horizon) {
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };

              QString field = QStringLiteral("z");
              if (auto *edit = constraintPage->findChild<QLineEdit *>(QStringLiteral("idwField")))
              {
                const QString typed = edit->text().trimmed();
                if (!typed.isEmpty())
                  field = typed;
              }
              double cellSize = 1.0;
              if (auto *spin = constraintPage->findChild<QDoubleSpinBox *>(QStringLiteral("idwCellSize")))
                cellSize = spin->value();

              QString pointsId;
              if (!m_layerSvc)
              {
                fail(tr("图层服务未就绪"));
                return;
              }
              QVector<LayerDeclaration> decls;
              QString readErr;
              if (!m_layerSvc->tryDeclared(&decls, &readErr))
              {
                fail(readErr.isEmpty() ? tr("无法读取图层清单") : readErr);
                return;
              }
              QString agnostic;
              for (const LayerDeclaration &d : decls)
              {
                if (d.type.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0)
                  continue;
                if (!d.layerId.startsWith(QStringLiteral("wells")))
                  continue;
                if (d.horizon == horizon)
                {
                  pointsId = d.layerId;
                  break;
                }
                if (agnostic.isEmpty() && d.horizon.isEmpty())
                  agnostic = d.layerId;
              }
              if (pointsId.isEmpty())
                pointsId = agnostic;
              if (pointsId.isEmpty())
              {
                fail(tr("层位 %1 没有井点图层").arg(horizon));
                return;
              }

              QString err;
              if (!constraint->runConstraintIDW(horizon, pointsId, field, cellSize, &err))
                fail(err.isEmpty() ? tr("约束插值失败") : err);
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
    // 问题 → 地图/连井联动（阶段C，预览壳重排）：地图移到井点 + 连井滚到
    // 井/分层；地震测线不再走底栏 gotoLine，由「在数据页看这条剖面」显式
    // 切页打开（threewaylocator.h）。缺面板的字段自动跳过。
    auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
    auto *threeWay = new ThreeWayLocator(m_canvasCtl, corrPanel, bottomTabs, this);
    threeWay->attach(validatePage);
    // 「在数据页看这条剖面」：切到数据管理页（预览分栏随之可见），打开/
    // 聚焦地震资产标签并把测线拨到载荷里的 inline/time_ms。载荷没有
    // asset_id——测线号落在哪个 survey 的 inline 范围就开它的链接资产
    // （主关联优先）；一条地震资产都没有就不跳页，不造假定位。
    connect(validatePage, &ValidatePage::seismicSectionRequested, this,
            [this, importSvc](const QVariantMap &payload) {
              if (!m_previewTabs || !importSvc)
                return;
              const int line = payload.value(QStringLiteral("inline"), -1).toInt();
              if (line < 0)
                return;
              const double timeMs =
                  payload.value(QStringLiteral("time_ms"), -1.0).toDouble();
              DataCatalog *cat = importSvc->catalog();
              QString assetId, firstSeismic;
              for (const EntityAssetLink &l : cat->links())
              {
                if (l.unresolved || l.role != QLatin1String("seismic_volume") ||
                    l.assetId.isEmpty())
                  continue;
                if (firstSeismic.isEmpty())
                  firstSeismic = l.assetId;
                const CatalogEntity s = cat->entityById(l.entityId);
                if (s.entityType == QLatin1String("seismic_survey") &&
                    line >= s.inlineMin && line <= s.inlineMax &&
                    (assetId.isEmpty() || l.isPrimary))
                  assetId = l.assetId;
              }
              if (assetId.isEmpty())
                assetId = firstSeismic;
              if (assetId.isEmpty())
                return;
              showPage(QStringLiteral("data"));
              m_previewTabs->openSeismicLine(
                  assetId, QStringLiteral("inline"), line, timeMs);
            });
    // 阶段E 发布门：验证跑完 → 重算逐井残差覆盖（attachMapping 装的钩子，
    // 未装则无事发生）。
    connect(validate, &ValidationWorkflow::validationDone, this,
            [this](int) { if (m_refreshPublishGate) m_refreshPublishGate(); });
  }

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
          QgsMessageLog::logMessage(tr("Horizon locator: manifest read failed: %1")
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
      // 保存 = 快速访问栏第一颗（Office 惯例）+「文件」菜单；Ctrl+S 走同一函数。
      auto *saveAct = new QAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileSave.svg")),
                                  tr("保存工程"), this);
      saveAct->setObjectName(QStringLiteral("saveProjectAction"));
      saveAct->setToolTip(tr("保存工程（Ctrl+S）"));
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
      connect(saveAct, &QAction::triggered, this, saveFn);
      auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
      connect(saveShortcut, &QShortcut::activated, this, saveFn);
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
              QgsMessageLog::logMessage(tr("Release panel: manifest read failed: %1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return declared;
          });
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
      auto *taskPanel = new TaskPanel(store, taskSvc, bottomTabs);
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
          QVector<LayerDeclaration> declared;
          QString manifestErr;
          if (!m_layerSvc->tryDeclared(&declared, &manifestErr))
          {
            QgsMessageLog::logMessage(tr("Attribute panel: manifest read failed: %1")
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
        connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                [refreshIds](const QString &) { refreshIds(); });
        bottomTabs->addTab(attrPanel, QStringLiteral("属性表"));
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
    editTb->hide(); // 逻辑宿主，不进布局
    if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
    {
      qab->addAction(editTb->actionUndo());
      qab->addAction(editTb->actionRedo());
    }
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
    }

  buildRibbonPanels(dataPage, predictPage, constraintPage, composePage, validatePage, editTb,
                    corrPanel);

  // Re-sync visible page index with current tab.
  const int idx = kPageIds.indexOf(m_currentPage);
  stack->setCurrentIndex(idx >= 0 ? idx : 0);
  m_workflowsAttached = true; // 走到末尾才算接线完成（幂等守卫置位）
}

// ---------------------------------------------------------------------------
// Ribbon 命令组：每个页签按「做事的顺序」排组——数据管理（导入 → 列表 →
// 预览）、预测编图（运行 → 叠加对照 → 编辑 → 地图 → 结果）、单因素图（约束
// → 插值 → 连井 → 编辑 → 地图 → 结果）、智能编图（编图链 → 编辑 → 地图 →
// 图件输出 → 版本）、验证（验证 → 联动定位 → 地图 → 发布）。
// 页面板按钮是状态源：ribbon 动作经 PaleoRibbon::mirror 镜像它们。
// ---------------------------------------------------------------------------
void PaleoMainWindow::buildRibbonPanels(DataPage *data, PredictPage *predict,
                                        ConstraintPage *constraint, ComposePage *compose,
                                        ValidatePage *validate, PaleoEditingToolbar *editTb,
                                        WellCorrelationPanel *corrPanel)
{
  SARibbonBar *bar = ribbonBar();
  if (!bar)
    return;
  bar->beginUpdate();

  const auto icon = [](const char *name) { return PaleoIcons::qgisTheme(QLatin1String(name)); };
  const auto newAction = [this](const QString &text, const QIcon &ic, const QString &tip,
                                const char *name) {
    auto *a = new QAction(ic, text, this);
    a->setObjectName(QLatin1String(name));
    if (!tip.isEmpty())
      a->setToolTip(tip);
    return a;
  };
  // 镜像命令：面板按钮存在才建（空工作流的测试壳照样能跑）。
  const auto mirrored = [&newAction](QWidget *page, const char *buttonName, const QString &text,
                                     const QIcon &ic, const char *actionName,
                                     bool syncText = false) -> QAction * {
    auto *b = page ? page->findChild<QAbstractButton *>(QLatin1String(buttonName)) : nullptr;
    if (!b)
      return nullptr;
    QAction *a = newAction(text, ic, QString(), actionName);
    PaleoRibbon::mirror(a, b, syncText);
    return a;
  };
  const auto large = [](SARibbonPanel *p, QAction *a, bool run = false) {
    if (!p || !a)
      return;
    p->addLargeAction(a);
    if (run)
      PaleoRibbon::markRun(PaleoRibbon::buttonFor(p, a));
  };
  const auto small = [](SARibbonPanel *p, QAction *a) {
    if (p && a)
      p->addSmallAction(a);
  };
  const auto panel = [](SARibbonCategory *cat, const QString &title, const char *name) {
    SARibbonPanel *p = cat->addPanel(title);
    p->setObjectName(QLatin1String(name));
    return p;
  };

  // ---- 共享动作（多个页签复用同一颗 QAction）----
  // 「参数」= 右侧 dock 的 toggleViewAction：文案随页（预测参数/单因素参数…）。
  QAction *paramsAct = m_rightDock ? m_rightDock->toggleViewAction() : nullptr;
  if (paramsAct)
  {
    paramsAct->setIcon(icon("mActionOptions.svg"));
    paramsAct->setToolTip(tr("显示/隐藏右侧参数面板"));
  }
  auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
  const auto bottomAction = [this, bottomTabs, &newAction](QWidget *tab, const QString &text,
                                                           const QIcon &ic, const QString &tip,
                                                           const char *name) -> QAction * {
    if (!tab || !bottomTabs)
      return nullptr;
    QAction *a = newAction(text, ic, tip, name);
    connect(a, &QAction::triggered, this, [this, bottomTabs, tab] {
      m_bottomDock->show();
      m_bottomDock->raise();
      bottomTabs->setCurrentWidget(tab);
    });
    return a;
  };
  QAction *corrAct = bottomAction(corrPanel, tr("连井剖面"), icon("mActionElevationProfile.svg"),
                                  tr("在底部面板打开连井剖面（与地图联动）"),
                                  "ribbonCorrelationAction");
  QAction *attrAct = bottomAction(findChild<QWidget *>(QStringLiteral("attributeTablePanel")),
                                  tr("属性表"), icon("mActionOpenTable.svg"),
                                  tr("在底部面板打开图层属性表"), "ribbonAttributeTableAction");
  QAction *releaseAct = bottomAction(findChild<QWidget *>(QStringLiteral("releasePanel")),
                                     tr("发布记录"), icon("mActionHistory.svg"),
                                     tr("在底部面板查看已发布的版本"), "ribbonReleaseAction");
  QAction *toValidate = newAction(tr("送交验证"), icon("mActionSharingExport.svg"),
                                  tr("切到验证页，用井点残差检查当前成果"), "ribbonToValidateAction");
  connect(toValidate, &QAction::triggered, this, [this] { showPage(QStringLiteral("validate")); });

  // 地图导航：原生 QGIS 工具；setAction 让工具激活/停用时自动勾选/取消。
  QAction *panAct = nullptr, *zoomInAct = nullptr, *zoomOutAct = nullptr, *fullAct = nullptr;
  if (m_canvasCtl)
  {
    QgsMapCanvas *cv = m_canvasCtl->canvas();
    const auto toolAction = [this, &newAction](QgsMapTool *tool, const QString &text,
                                               const QIcon &ic, const QString &tip,
                                               const char *name) {
      QAction *a = newAction(text, ic, tip, name);
      a->setCheckable(true);
      tool->setAction(a);
      connect(a, &QAction::triggered, this, [this, tool] { m_canvasCtl->setMapTool(tool); });
      return a;
    };
    panAct = toolAction(new QgsMapToolPan(cv), tr("平移"), icon("mActionPan.svg"),
                        tr("拖动平移地图"), "ribbonPanAction");
    zoomInAct = toolAction(new QgsMapToolZoom(cv, false), tr("放大"), icon("mActionZoomIn.svg"),
                           tr("点击或框选放大"), "ribbonZoomInAction");
    zoomOutAct = toolAction(new QgsMapToolZoom(cv, true), tr("缩小"), icon("mActionZoomOut.svg"),
                            tr("点击或框选缩小"), "ribbonZoomOutAction");
    fullAct = newAction(tr("全图"), icon("mActionZoomFullExtent.svg"), tr("缩放到全部图层"),
                        "ribbonZoomFullAction");
    connect(fullAct, &QAction::triggered, this, [this] { m_canvasCtl->zoomToFullExtent(); });
  }
  const auto navPanel = [&](SARibbonCategory *cat) {
    if (!panAct)
      return;
    SARibbonPanel *p = panel(cat, tr("地图"), "ribbonNavPanel");
    large(p, panAct);
    small(p, zoomInAct);
    small(p, zoomOutAct);
    small(p, fullAct);
  };

  // ================= 数据管理 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("data")))
  {
    SARibbonPanel *p = panel(cat, tr("数据导入"), "ribbonPanel.data.import");
    large(p, mirrored(data, "importFolder", tr("导入工区文件夹"), icon("mIconFolderOpen.svg"),
                      "ribbonImportFolder"));
    small(p, mirrored(data, "importWells", tr("导入井数据"), icon("mIconPointLayer.svg"),
                      "ribbonImportWells"));
    small(p, mirrored(data, "importSeismic", tr("导入地震数据"), icon("mIconRasterLayer.svg"),
                      "ribbonImportSeismic"));
    small(p, mirrored(data, "importBoundary", tr("导入边界数据"), icon("mIconPolygonLayer.svg"),
                      "ribbonImportBoundary"));

    if (data)
    {
      SARibbonPanel *lp = panel(cat, tr("数据列表"), "ribbonPanel.data.list");
      QAction *unresolved = newAction(tr("只看未决"), icon("mActionFilter2.svg"),
                                      tr("只显示还有未决关联的数据"), "ribbonUnresolvedFilter");
      unresolved->setCheckable(true);
      connect(unresolved, &QAction::triggered, data, &DataPage::setUnresolvedFilter);
      // 过滤条显隐（「清除过滤」、导入确认框的「查看未决」）回写勾选态。
      if (auto *filterBar = data->findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
        PaleoRibbon::followVisibility(unresolved, filterBar);
      large(lp, unresolved);
      QAction *refresh = newAction(tr("刷新列表"), icon("mActionRefresh.svg"),
                                   tr("按数据目录重建列表"), "ribbonRefreshList");
      connect(refresh, &QAction::triggered, data, &DataPage::refreshAssetTable);
      large(lp, refresh);
    }

    auto *maxBtn = m_previewTabs
                       ? m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton"))
                       : nullptr;
    auto *inner = m_previewTabs
                      ? m_previewTabs->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"))
                      : nullptr;
    if (maxBtn && inner)
    {
      SARibbonPanel *vp = panel(cat, tr("数据预览"), "ribbonPanel.data.preview");
      QAction *maxAct = newAction(tr("最大化预览"), PaleoIcons::maximize(), QString(),
                                  "ribbonPreviewMaximize");
      maxAct->setCheckable(true);
      connect(maxAct, &QAction::triggered, maxBtn, [maxBtn](bool on) { maxBtn->setChecked(on); });
      connect(maxBtn, &QAbstractButton::toggled, maxAct, [maxAct](bool on) {
        maxAct->setChecked(on);
        maxAct->setText(on ? tr("还原预览") : tr("最大化预览"));
        maxAct->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
      });
      // 没有打开的预览时不可用（禁用必带原因，§35）。
      const auto syncMax = [maxAct, inner] {
        const bool any = inner->count() > 0;
        maxAct->setEnabled(any);
        maxAct->setToolTip(any ? tr("预览占满数据面（列表留一行）")
                               : tr("先在数据列表里选一条数据打开预览"));
      };
      connect(inner, &QTabWidget::currentChanged, maxAct, syncMax);
      syncMax();
      large(vp, maxAct);
    }
  }

  // ================= 预测编图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("predict")))
  {
    SARibbonPanel *p = panel(cat, tr("预测运行"), "ribbonPanel.predict.run");
    large(p, mirrored(predict, "runButton", tr("运行预测"), icon("mActionStart.svg"),
                      "ribbonRunPrediction"),
          true);
    large(p, paramsAct);
    if (corrAct || attrAct)
    {
      SARibbonPanel *cp = panel(cat, tr("叠加对照"), "ribbonPanel.predict.compare");
      large(cp, corrAct);
      large(cp, attrAct);
    }
    addEditingPanel(cat, editTb);
    navPanel(cat);
    large(panel(cat, tr("结果"), "ribbonPanel.predict.result"), toValidate);
  }

  // ================= 单因素图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("constraint")))
  {
    SARibbonPanel *p = panel(cat, tr("约束编辑"), "ribbonPanel.constraint.draw");
    auto *drawBtn = constraint ? constraint->findChild<QPushButton *>(QStringLiteral("drawButton"))
                               : nullptr;
    auto *shape = constraint ? constraint->findChild<QComboBox *>(QStringLiteral("shapeCombo"))
                             : nullptr;
    if (QAction *draw = mirrored(constraint, "drawButton", tr("绘制约束"),
                                 icon("mActionCaptureLine.svg"), "ribbonDrawConstraint"))
    {
      // 拆分按钮：主体按参数面板当前形状画；下拉直接挑形状开画（改的是
      // 同一个 shapeCombo，参数面板随之显示）。
      if (shape && drawBtn)
      {
        auto *menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("ribbonConstraintShapeMenu"));
        for (int i = 0; i < shape->count(); ++i)
          menu->addAction(shape->itemText(i), this, [shape, drawBtn, i] {
            shape->setCurrentIndex(i);
            drawBtn->click();
          });
        draw->setMenu(menu);
        p->addLargeAction(draw, QToolButton::MenuButtonPopup);
      }
      else
        large(p, draw);
    }
    large(p, paramsAct);
    SARibbonPanel *ip = panel(cat, tr("插值计算"), "ribbonPanel.constraint.idw");
    large(ip, mirrored(constraint, "runIdwButton", tr("计算单因素"), icon("mActionStart.svg"),
                       "ribbonRunIdw"),
          true);
    if (corrAct)
      large(panel(cat, tr("连井分析"), "ribbonPanel.constraint.correlation"), corrAct);
    addEditingPanel(cat, editTb);
    navPanel(cat);
    large(panel(cat, tr("结果"), "ribbonPanel.constraint.result"), toValidate);
  }

  // ================= 智能编图 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("compose")))
  {
    SARibbonPanel *p = panel(cat, tr("编图链"), "ribbonPanel.compose.chain");
    large(p, mirrored(compose, "thicknessChainButton", tr("生成等厚图"),
                      icon("processingAlgorithm.svg"), "ribbonThicknessChain", true),
          true);
    small(p, mirrored(compose, "fuseButton", tr("合成编图"), icon("processingModel.svg"),
                      "ribbonFuse"));
    small(p, mirrored(compose, "polygonizeButton", tr("转为相多边形"),
                      icon("mActionCapturePolygon.svg"), "ribbonPolygonize"));
    large(p, paramsAct);
    addEditingPanel(cat, editTb);
    navPanel(cat);

    SARibbonPanel *op = panel(cat, tr("图件输出"), "ribbonPanel.compose.output");
    large(op, mirrored(compose, "exportPdfButton", tr("导出 PDF"), icon("mActionSaveAsPDF.svg"),
                       "ribbonExportPdf"));
    if (QAction *designer = findChild<QAction *>(QStringLiteral("ribbonDesignerAction")))
    {
      large(op, designer);
      if (auto *b = PaleoRibbon::buttonFor(op, designer))
      {
        b->setObjectName(QStringLiteral("designerButton"));
        b->setAccessibleName(tr("打开图件设计器"));
      }
    }

    SARibbonPanel *vp = panel(cat, tr("版本"), "ribbonPanel.compose.version");
    large(vp, mirrored(compose, "saveVersionButton", tr("保存版本"), icon("mActionFileSaveAs.svg"),
                       "ribbonSaveVersion", true));
    large(vp, mirrored(compose, "publishButton", tr("发布"), icon("mActionSharing.svg"),
                       "ribbonPublish"));
    small(vp, toValidate);
  }

  // ================= 验证 =================
  if (SARibbonCategory *cat = categoryForPage(QStringLiteral("validate")))
  {
    SARibbonPanel *p = panel(cat, tr("验证"), "ribbonPanel.validate.run");
    large(p, mirrored(validate, "runButton", tr("运行验证"), icon("mActionStart.svg"),
                      "ribbonRunValidation"),
          true);
    large(p, paramsAct);
    QAction *section = mirrored(validate, "openSeismicSectionButton", tr("看这条剖面"),
                                icon("mIconRasterLayer.svg"), "ribbonOpenSection");
    if (section || corrAct)
    {
      SARibbonPanel *lp = panel(cat, tr("联动定位"), "ribbonPanel.validate.locate");
      large(lp, section);
      large(lp, corrAct);
    }
    navPanel(cat);
    if (releaseAct)
      large(panel(cat, tr("发布"), "ribbonPanel.validate.release"), releaseAct);
  }

  bar->endUpdate();
  bar->updateRibbonGeometry();
}

void PaleoMainWindow::addEditingPanel(SARibbonCategory *category, PaleoEditingToolbar *editTb)
{
  if (!category || !editTb)
    return;
  SARibbonPanel *p = category->addPanel(tr("要素编辑"));
  p->setObjectName(QStringLiteral("ribbonEditPanel"));

  // 目标图层：共享编辑条下拉的 model（●标记编辑中图层，同一份数据）；用户
  // 选择经 setCurrentLayer 回写——与编辑条自身 activated 同一路径。
  QComboBox *master = editTb->layerCombo();
  auto *combo = new QComboBox(p);
  combo->setObjectName(QStringLiteral("ribbonEditLayerCombo"));
  combo->setAccessibleName(tr("编辑图层"));
  combo->setToolTip(tr("要编辑的矢量图层"));
  combo->setPlaceholderText(tr("选择可编辑图层"));
  combo->setModel(master->model());
  auto *state = new QLabel(p);
  state->setObjectName(QStringLiteral("ribbonEditState"));
  connect(combo, &QComboBox::activated, editTb, [editTb, combo](int i) {
    editTb->setCurrentLayer(qvariant_cast<QgsVectorLayer *>(combo->itemData(i)));
  });
  const auto sync = [combo, state, master, editTb] {
    combo->setCurrentIndex(master->currentIndex());
    state->setText(editTb->stateLabel()->text());
  };
  connect(master, &QComboBox::currentIndexChanged, combo, sync);
  // 编辑条重建下拉时屏蔽了自身信号——model 变化与会话切换后排队再对一次。
  QAbstractItemModel *model = master->model();
  connect(model, &QAbstractItemModel::modelReset, combo, sync, Qt::QueuedConnection);
  connect(model, &QAbstractItemModel::rowsInserted, combo, sync, Qt::QueuedConnection);
  connect(model, &QAbstractItemModel::rowsRemoved, combo, sync, Qt::QueuedConnection);
  connect(editTb, &PaleoEditingToolbar::editingStarted, combo, sync, Qt::QueuedConnection);
  connect(editTb, &PaleoEditingToolbar::editingStopped, combo, sync, Qt::QueuedConnection);
  sync();
  p->addSmallWidget(combo);
  p->addSmallWidget(state);

  p->addLargeAction(editTb->actionSelect());
  p->addLargeAction(editTb->actionAddFeature(), QToolButton::InstantPopup);
  p->addSmallAction(editTb->actionReshape());
  p->addSmallAction(editTb->actionMove());
  p->addSmallAction(editTb->actionDeleteFeatures());
  p->addSmallAction(editTb->actionVertexEdit());
  p->addSmallAction(editTb->actionSave());
  p->addSmallAction(editTb->actionCancel());
}

// ---------------------------------------------------------------------------
// wave/mapping-pipeline 阶段C+E — 编图链 / 层位图导出 / 版本状态机接线
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                                    MapVersionStore *versionStore, ProjectDataFacade *projectData,
                                    DataCatalog *catalog)
{
  auto *composePage = findChild<ComposePage *>();
  if (!composePage || !mapping || !versions)
    return;

  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };

  // 发布门（§177/§260）：版本行上的 PDF 资产 id + 逐井残差摘要完整性共同
  // 决定按钮状态；tooltip 写缺的那条。导出成功 / 保存 / 发布 / 验证跑完 /
  // 工程打开后都重算 —— 门是「当前状态」而不是一次性开关。
  const auto refreshPublishGate = [this, versionStore, projectData, catalog]() {
    auto *page = findChild<ComposePage *>();
    if (!page)
      return;
    const QString h = m_selection ? m_selection->activeHorizon() : QString();
    const MapVersion v = (versionStore && !h.isEmpty()) ? versionStore->latest(h)
                                                       : MapVersion();
    const QString summary = h.isEmpty()
        ? QString()
        : MapVersionController::residualSummaryJson(projectData, h);
    int covered = -1, total = -1;
    MapVersionStore::residualSummaryComplete(summary, &covered, &total);
    page->setPublishState(!v.pdfAssetId.isEmpty(), covered, total);
    page->setVersionState(v.version, v.state == QLatin1String("Published"));
    // B 包 staleness-lite advisory：目标工程里有过时下游产物 → 发布按钮
    // tooltip 如实列出（可见但不阻断——enable 态仍由 setPublishState 决定）。
    const QString advisory = MapVersionController::stalePublishAdvisory(catalog);
    if (!advisory.isEmpty())
      if (auto *btn = page->findChild<QPushButton *>(QStringLiteral("publishButton")))
        btn->setToolTip(btn->toolTip().isEmpty()
                            ? advisory
                            : btn->toolTip() + QLatin1Char('\n') + advisory);
  };
  m_refreshPublishGate = refreshPublishGate;

  // 链路状态文案走 statusLabel（成功/失败都落页面，再进日志）。
  connect(mapping, &MappingWorkflow::chainDone, this,
          [this, composePage, status](const QString &h, const QString &layerId) {
            status(tr("编图链完成：%1 → %2").arg(h, layerId));
            composePage->refreshFactors();
          });
  connect(mapping, &MappingWorkflow::chainFailed, this,
          [status](const QString &, const QString &error) { status(error); });

  // 「选层位 → 算厚度 → IDW → 转相面」：层位来自 chip 的 activeHorizon，
  // 提示里的目标层位随工程参数（AreaRules targetHorizon）。
  connect(composePage, &ComposePage::thicknessChainRequested, this,
          [this, mapping, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先在顶部 chip 选择层位（本阶段目标 %1）")
                         .arg(AreaRules::active().targetHorizon));
              return;
            }
            QString err;
            if (!mapping->runThicknessChain(h, &err))
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          });

  // D8 使能态：无层位时按钮禁用（tooltip 写原因）；chip 切换即时联动命名。
  composePage->setThicknessHorizon(activeHorizon());
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, composePage,
            &ComposePage::setThicknessHorizon);

  // 层位图 PDF（阶段C+E）：导出 → catalog OUTPUT 资产登记 → 布局产物记录 →
  // 发布门重算。失败弹「导出失败 + 原因 + 重试」；成功弹路径 + SHA-256。
  connect(composePage, &ComposePage::exportPdfRequested, this,
          [this, versionStore, projectData, catalog, activeHorizon, status,
           refreshPublishGate]() {
            const QString h = activeHorizon();
            if (h.isEmpty() || !m_layerSvc)
            {
              status(tr("先在顶部 chip 选择层位再导出"));
              return;
            }
            const QString projectDir = m_projectSvc
                                           ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                           : QDir::temp().absolutePath();
            const QString target = QDir(projectDir).filePath(
                QStringLiteral("%1_map.pdf").arg(h));
            QString pdf;
            while (true) // 失败 → 重试 / 取消（§215：导出失败要写原因）
            {
              QString err;
              pdf = exportHorizonMapPdf(m_layerSvc, m_projectSvc, h, target, &err);
              if (!pdf.isEmpty())
                break;
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              const auto choice = QMessageBox::warning(
                  this, tr("导出失败"),
                  tr("%1\n\n导出目标：%2").arg(err, target),
                  QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
              if (choice != QMessageBox::Retry)
              {
                status(err);
                return;
              }
            }

            // 阶段E：登记 catalog OUTPUT 受管资产 —— 登记失败的导出不算完成
            // （发布门要求的是「已登记的 PDF」，不是「写出过文件」）。
            QString sha, managedPath, regErr;
            const QString assetId =
                registerMapPdfAsset(catalog, projectDir, pdf, &sha, &managedPath, &regErr);
            if (assetId.isEmpty())
            {
              status(regErr.isEmpty() ? tr("PDF 资产登记失败") : regErr);
              QgsMessageLog::logMessage(regErr, QStringLiteral("Paleo"),
                                        Qgis::MessageLevel::Warning);
              return;
            }
            if (versionStore)
            {
              QString productErr;
              const QString recorded = managedPath.isEmpty() ? pdf : managedPath;
              if (!versionStore->recordLayoutProduct(h, recorded, assetId, sha, &productErr))
                QgsMessageLog::logMessage(productErr, QStringLiteral("Paleo"),
                                          Qgis::MessageLevel::Warning);
            }
            status(tr("层位图已导出：%1").arg(pdf));
            QMessageBox::information(this, tr("导出成功"),
                                     tr("已导出层位图：\n%1\n\nSHA-256：%2")
                                         .arg(pdf, sha));
            refreshPublishGate();
          });

  // 保存版本：commit + 版本号递增（undo 清空在 controller 内，§1223）；
  // 新版本行继承最近登记的 PDF 产物引用 —— 保存后发布门可能开闸。
  connect(composePage, &ComposePage::saveVersionRequested, this,
          [versions, composePage, activeHorizon, status, refreshPublishGate]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再保存版本"));
              return;
            }
            QVariantMap provenance;
            provenance.insert(QStringLiteral("saved_from"),
                              QStringLiteral("compose_page"));
            QString err;
            const MapVersion v = versions->saveVersion(h, provenance, &err);
            if (v.version > 0)
            {
              status(tr("已保存版本：%1 v%2").arg(h).arg(v.version));
              composePage->setVersionState(v.version, false);
            }
            else
              status(err.isEmpty() ? tr("保存版本失败") : err);
            refreshPublishGate();
          });

  // 发布：确认对话列版本号 / PDF 文件名 / 覆盖井数（§177），确认后把
  // 逐井残差摘要随发布冻结进版本行。B 包 staleness-lite：有过时下游产物
  // 时确认文案如实列出（advisory——不阻断，Ok/Cancel 照常由人决断）。
  connect(composePage, &ComposePage::publishRequested, this,
          [this, versions, versionStore, projectData, catalog, composePage,
           activeHorizon, status, refreshPublishGate]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再发布"));
              return;
            }
            const QString summary =
                MapVersionController::residualSummaryJson(projectData, h);
            int covered = -1, total = -1;
            MapVersionStore::residualSummaryComplete(summary, &covered, &total);
            const MapVersion v = versionStore ? versionStore->latest(h) : MapVersion();
            const QString pdfName =
                QFileInfo(versionStore ? versionStore->latestLayoutProduct(h) : QString())
                    .fileName();
            const QString advisory =
                MapVersionController::stalePublishAdvisory(catalog);
            const auto choice = QMessageBox::question(
                this, tr("发布版本"),
                tr("发布 %1 v%2？\n\nPDF：%3\n覆盖井数：%4/%5\n\n发布后快照只读，"
                   "继续编辑请保存新版本。")
                    .arg(h)
                    .arg(v.version)
                    .arg(pdfName.isEmpty() ? tr("（未登记）") : pdfName)
                    .arg(covered < 0 ? 0 : covered)
                    .arg(total < 0 ? 0 : total)
                    + (advisory.isEmpty()
                           ? QString()
                           : QStringLiteral("\n\n注意：") + advisory),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice != QMessageBox::Ok)
              return;
            QString err;
            const QString dir = versions->publish(h, summary, &err);
            if (!dir.isEmpty())
            {
              status(tr("已发布：%1 v%2 → %3").arg(h).arg(v.version).arg(dir));
              composePage->setVersionState(v.version, true);
            }
            else
              status(err.isEmpty() ? tr("发布失败") : err);
            refreshPublishGate();
          });

  // 工程打开时恢复发布门状态（版本行的 PDF 资产 + 残差覆盖重算）。
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [refreshPublishGate]() { refreshPublishGate(); });
  refreshPublishGate(); // 初始态：缺什么写什么，按钮禁用
}

// ---------------------------------------------------------------------------
// D11 临时配准（手工仿射 → DERIVED + 水印图层）
// ---------------------------------------------------------------------------
void PaleoMainWindow::applyProvisionalRegistration(DataImportService *svc,
                                                   const QString &assetId,
                                                   const QVariantMap &params)
{
  const auto fail = [this](const QString &why) {
    const QString text = tr("临时配准失败：%1").arg(why);
    if (statusBar())
      statusBar()->showMessage(text, 8000);
    QgsMessageLog::logMessage(text, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
  };

  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  if (!cat || !m_layerSvc)
  {
    fail(tr("目录或图层服务未就绪"));
    return;
  }
  const CatalogVersion src = cat->currentVersion(assetId);
  if (src.id.isEmpty())
  {
    fail(tr("资产没有可用版本"));
    return;
  }
  const QString srcAbs = svc->absolutePathForVersion(src);
  if (srcAbs.isEmpty() || !QFile::exists(srcAbs))
  {
    fail(tr("找不到源文件 %1").arg(srcAbs));
    return;
  }

  // 工程目录：catalogPath() = <projectDir>/artifacts/metadata/catalog.json。
  const QDir projectDir = QFileInfo(cat->catalogPath())
                              .absoluteDir()
                              .absoluteFilePath(QStringLiteral("../.."));
  const QString verId = cat->nextVersionId();
  const QString relDir = QStringLiteral("artifacts/derived/%1/%2").arg(assetId, verId);
  const QString outDir = projectDir.absoluteFilePath(relDir);
  if (!QDir().mkpath(outDir))
  {
    fail(tr("派生目录创建失败"));
    return;
  }
  const QString outName =
      QFileInfo(src.fileName).completeBaseName() + QStringLiteral(".provisional.geojson");
  const QString outAbs = QDir(outDir).filePath(outName);

  GeoAffineParams p;
  p.tx = params.value(QStringLiteral("tx")).toDouble();
  p.ty = params.value(QStringLiteral("ty")).toDouble();
  p.sx = params.value(QStringLiteral("sx"), 1.0).toDouble();
  p.sy = params.value(QStringLiteral("sy"), 1.0).toDouble();
  p.rotDeg = params.value(QStringLiteral("rotDeg")).toDouble();

  QString terr;
  int featureCount = 0;
  double bounds[4] = {0, 0, 0, 0};
  if (!geoAffineTransformFile(srcAbs, outAbs, p, &terr, &featureCount, bounds))
  {
    fail(terr);
    return;
  }
  QFile::setPermissions(outAbs, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther);

  CatalogVersion d;
  d.id = verId;
  d.assetId = assetId;
  d.stage = QStringLiteral("DERIVED");
  d.versionNumber = src.versionNumber + 1;
  d.managed = true;
  d.path = relDir + QLatin1Char('/') + outName;
  d.sourceUri = srcAbs;
  d.sha256 = DataCatalog::sha256FileHex(outAbs, &terr);
  d.fileName = outName;
  d.parentVersionIds = QStringList{src.id};
  d.extra.insert(QStringLiteral("provisional"), true);
  d.extra.insert(QStringLiteral("affine"), geoAffineToMap(p));
  QString verr;
  if (!cat->addVersion(d, &verr))
  {
    fail(verr);
    return;
  }

  // 「临时配准 · 名」矢量图层：告警橙描边虚线——与正式图层视觉隔离。
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("provisional.%1").arg(assetId);
  decl.type = QStringLiteral("vector");
  decl.source = outAbs;
  decl.group = QStringLiteral("00_Data");
  decl.title = tr("临时配准 · %1").arg(cat->assetById(assetId).displayName);
  QString derr;
  if (!m_layerSvc->declare(decl, &derr))
  {
    fail(derr);
    return;
  }
  QgsMapLayer *ml = m_layerSvc->instantiate(decl.layerId, &derr);
  auto *vl = qobject_cast<QgsVectorLayer *>(ml);
  if (!vl)
  {
    fail(derr.isEmpty() ? tr("图层实例化失败") : derr);
    return;
  }
  QVariantMap symProps;
  symProps.insert(QStringLiteral("color"), QStringLiteral("242,153,0,60"));      // warning 25%
  symProps.insert(QStringLiteral("outline_color"), QStringLiteral("#F29900"));
  symProps.insert(QStringLiteral("outline_width"), QStringLiteral("0.8"));
  symProps.insert(QStringLiteral("outline_style"), QStringLiteral("dash"));
  QgsSymbol *sym = nullptr;
  switch (vl->geometryType())
  {
    case Qgis::GeometryType::Line:
      symProps.remove(QStringLiteral("color"));
      symProps.insert(QStringLiteral("line_color"), QStringLiteral("#F29900"));
      symProps.insert(QStringLiteral("line_width"), QStringLiteral("0.8"));
      symProps.insert(QStringLiteral("line_style"), QStringLiteral("dash"));
      sym = QgsLineSymbol::createSimple(symProps).release();
      break;
    case Qgis::GeometryType::Point:
      symProps.insert(QStringLiteral("name"), QStringLiteral("triangle"));
      symProps.insert(QStringLiteral("size"), QStringLiteral("4"));
      sym = QgsMarkerSymbol::createSimple(symProps).release();
      break;
    default:
      sym = QgsFillSymbol::createSimple(symProps).release();
      break;
  }
  if (sym)
    vl->setRenderer(new QgsSingleSymbolRenderer(sym));

  ++m_provisionalLayers;
  if (m_decorMgr)
    m_decorMgr->setWatermarkEnabled(true); // 有临时配准图层期间一直压水印
  if (m_canvasCtl)
    m_canvasCtl->zoomToLayer(decl.layerId);
  if (statusBar())
    statusBar()->showMessage(
        tr("临时配准已上图：%1 · %2 个要素（手工仿射，非权威坐标）")
            .arg(decl.title)
            .arg(featureCount),
        8000);
}
