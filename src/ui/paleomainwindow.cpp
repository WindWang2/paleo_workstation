// 层：视图
#include "paleomainwindow.h"
#include "paleodockmanager.h"
#include "paleoviewport.h"

#include "paleotheme.h" // T32：焦点环/mono 数字面 token 出口；暗色翻案 token 全集
#include "paleoemptystate.h" // T31 空态卡片共享组件（本文件旧匿名类收敛于此）
#include "paleoicons.h" // ribbon 图标：QGIS 主题直取（暗色再着色）+ 自绘补缺
#include "paleoribbon.h" // SARibbon 壳公用件：主题/命令镜像

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "../workflow/workflows.h"
#include "../domain/projectclassifier.h"
#include "../domain/arearules.h"
#include "../domain/mappinghorizons.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/threewaylocator.h"
#include "../qgis/qgisprocessingservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "../metadata/mapversionstore.h"
#include "../services/projectdata.h"
#include "../services/previewdoc.h"
#include "../workflow/folderimport.h"
#include "../workflow/registration.h"
#include "../workflow/projectopen.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/mapexport.h"
#include "../workflow/mapversioncontroller.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "pages/pagepanels.h"
#include "pages/pageshared.h" // kPageIds（W4 跨 TU 页序表）
#include "constraintdrawcontroller.h"
#include "dialogs/folderconfirm.h"
#include "typedconstraintdrawcontroller.h" // ---- m2(B)：物源线/展布线/控制点（块内接线用）----
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "../catalog/datacatalog.h"
#include "horizonchipbar.h"
#include "layers/layertreepanel.h"
#include "layers/layerpropertiesdialog.h"
#include "layers/layerprofilebar.h"
#include "../qgis/qgislayerprofile.h"
#include "layoutdesignershell.h"
#include "webviewpanel.h"
#include "edittools/editingtoolbar.h"
#include "layout/layoutexportactions.h" // ---- m2(C)：导出前版面地图项钉主题 ----
#include "../qgis/qgislayerprofile.h" // ---- m2(C)：setLayoutMapTheme（m1 接缝）----
#include "../qgis/qgislayoutservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../services/paleotaskservice.h"
#include "decorations/paleodecorations.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "ui/seismic3d/seismic3dviewportwidget.h"
#include "services/seismictaskservice.h"
#include "domain/seismic/sgyvolume.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgslayout.h> // ---- m2(C)：设计器/导出的版面地图项主题钉定 ----
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
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
#include <qgswkbtypes.h>
#include <qgsrubberband.h>
#include <qgsgeometry.h>
#include <qgsmaptoolpan.h>
#include <qgsmaptoolzoom.h>

#include <QApplication>
#include <QCoreApplication>
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
#include <QLocale>
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
  // Tab order = reading order = right-panel stack order：页序表收敛到
  // pages/pageshared.h 的 kPageIds（W4：attach 接线 TU 同查页序）。
  // ribbon 页签文案（用户裁决 2026-09-27：Ribbon 界面，五页保留验证）。
  // 首调时构建（首次调用发生在 buildShell——QApplication 已在场，翻译
  // 系统可用）；命名空间级常量会在 main 前静态初始化，漏翻译。
  const QStringList &pageLabels()
  {
    static const QStringList labels = {
        QCoreApplication::translate("PaleoMainWindow", "数据管理"),
        QCoreApplication::translate("PaleoMainWindow", "预测编图"),
        QCoreApplication::translate("PaleoMainWindow", "单因素图"),
        QCoreApplication::translate("PaleoMainWindow", "智能编图"),
        QCoreApplication::translate("PaleoMainWindow", "验证"),
    };
    return labels;
  }
  // 右侧 dock 标题随页：数据页是属性面，编图页是参数面，验证页是结果面。
  const QStringList &pageDockTitles()
  {
    static const QStringList titles = {
        QCoreApplication::translate("PaleoMainWindow", "数据属性"),
        QCoreApplication::translate("PaleoMainWindow", "预测参数"),
        QCoreApplication::translate("PaleoMainWindow", "单因素参数"),
        QCoreApplication::translate("PaleoMainWindow", "编图参数"),
        QCoreApplication::translate("PaleoMainWindow", "验证结果"),
    };
    return titles;
  }

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

    auto *title = new QLabel(QCoreApplication::translate("PaleoMainWindow", "Paleo Workbench"), page);
    QFont f = title->font();
    f.setPointSize(15); // DESIGN.md 图件标题/页级标题 token
    f.setBold(true);
    title->setFont(f);
    auto *sub = new QLabel(QCoreApplication::translate("PaleoMainWindow", "古地理编图工作台 — 新建工程或打开最近工程开始"), page);

    auto *recentLabel = new QLabel(QCoreApplication::translate("PaleoMainWindow", "最近工程"), page);
    auto *list = new QListWidget(page);
    list->setObjectName(QStringLiteral("recentProjectsList"));
    list->setAccessibleName(QCoreApplication::translate("PaleoMainWindow", "最近工程列表"));
    const QStringList recent = readRecentProjects();
    for (const QString &p : recent)
    {
      auto *item = new QListWidgetItem(p, list);
      item->setData(Qt::UserRole, p); // activation handler reads the path from here
    }
    if (recent.isEmpty()) // §42.4 empty state — guidance, not a blank panel
    {
      auto *item = new QListWidgetItem(QCoreApplication::translate("PaleoMainWindow", "（暂无最近工程）"), list);
      item->setFlags(Qt::NoItemFlags);
    }

    auto *btnRow = new QHBoxLayout;
    auto *newBtn = new QPushButton(QCoreApplication::translate("PaleoMainWindow", "新建工程"), page);
    newBtn->setObjectName(QStringLiteral("newProjectButton"));
    auto *openBtn = new QPushButton(QCoreApplication::translate("PaleoMainWindow", "打开工程"), page);
    openBtn->setObjectName(QStringLiteral("openProjectButton"));
    auto *fromAreaBtn =
        new QPushButton(QCoreApplication::translate("PaleoMainWindow", "从工区文件夹新建"), page);
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
// T31 空态标签已收敛为共享组件 ui/paleoemptystate（本文件旧匿名类删除）；
// layertreepanel 的复制版迁移登记在 docs/progress/ux.md seam 表。
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
  // 暗色翻案（DESIGN.md 决策日志 2026-09-28）：main() 落的浅色只是无用户
  // 设置时的缺省；窗口构造按 QSettings 显式钉一次（读取无副作用——写只
  // 发生在用户切换主题时，测试路径不产生新写者）。
  PaleoTheme::applyTheme(PaleoTheme::themeFromSettings());
  buildShell();

  // ---- m2(D): 页面图层档案（m1 接缝消费）----
  // 四个编图页各有一份档案（QgisLayerProfileService 的页面档案表）；页切换
  // 与层位 chip 切换都会重应用。档案应用后画布随图层树勾选态刷新
  // （QgsLayerTreeMapCanvasBridge 节律），这里再补一次显式 refresh 兜底。
  // 档案服务本体在 buildShell 左 dock 组装处创建（m1 实装）；这里只叠
  // m2 的信号接线：档案应用后补一次显式 refresh 兜底。
  if (m_profileSvc)
    connect(m_profileSvc, &QgisLayerProfileService::profileApplied, this, [this] {
      if (m_canvasCtl && m_canvasCtl->canvas())
        m_canvasCtl->canvas()->refresh();
    });
  if (m_selection)
  {
    // chip 切换 = setActiveHorizon（chipbar 里 selection 先、layerSvc 后）+
    // 重应用当前页档案。deferred 到下一拍：等 chipbar 把 layerSvc 的激活
    // 层位也拨完，档案的层位过滤才读到新值。
    connect(m_selection, &SelectionContext::activeHorizonChanged, this,
            [this](const QString &) {
              QMetaObject::invokeMethod(
                  this, [this] { applyCurrentPageProfile(); }, Qt::QueuedConnection);
            });
  }

  if (m_projectSvc)
  {
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this](const QString &) { onProjectOpened(); });
    // 窗口标题/修改标记：dirty → [*] 显示，保存/改名 → 刷新工程名。
    if (QgsProject *proj = m_projectSvc->project())
    {
      connect(proj, &QgsProject::isDirtyChanged, this,
              [this](bool dirty) { setWindowModified(dirty); });
      connect(proj, &QgsProject::projectSaved, this, [this] {
        setWindowModified(false);
        updateWindowTitle();
      });
      connect(proj, &QgsProject::fileNameChanged, this,
              [this] { updateWindowTitle(); });
    }
  }

  m_dockManager->captureDefaultLayout();
  showStartup(); // §42.1: first-run lands on the startup page
  restoreWindowState();
}

PaleoMainWindow::~PaleoMainWindow() = default;

void PaleoMainWindow::applyCurrentPageProfile()
{
  if (!m_profileSvc ||
      !paleo::pagesinternal::kPageIds.contains(m_currentPage))
    return;
  m_profileSvc->applyPageProfile(m_currentPage);
}

void PaleoMainWindow::addDockWidget(Qt::DockWidgetArea area, QDockWidget *dock)
{
  m_dockManager->addDock(area, dock);
}

void PaleoMainWindow::buildShell()
{
  m_dockManager = new PaleoDockManager(this, QStringLiteral("ui/layout/workbench"));
  updateWindowTitle(); // 「<工程名> — Paleo Workbench [*]」（无工程时只有产品名）
  setMinimumSize(1280, 800); // §42.11 a11y floor

  // ---- center: startup page stacked under the workspace ----
  m_centerStack = new PaleoViewportStack(this);
  m_centerStack->setObjectName(QStringLiteral("centerStack"));

  QWidget *startup = makeStartupPage();
  m_centerStack->addWidget(startup); // index 0

  // 工作区两面（用户裁决：数据管理是列表面，另外四页以 QGIS 画布为主）：
  //   0 画布面 = 层位 chip 条 + QgsMapCanvas（预测编图/单因素图/智能编图/验证）
  //   1 数据面 = 可视化预览；数据列表独立停靠在主窗口左侧（数据管理）
  m_workspaceStack = new PaleoViewportStack(m_centerStack);
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
  chips->setAccessibleName(tr("层位切换"));
  // C2：编辑中拒切层位的原因此前无人接（信号发出去就丢了）——落状态栏。
  connect(chips, &HorizonChipBar::horizonSwitchRefused, this,
          [this](const QString &reason) {
            if (statusBar())
              statusBar()->showMessage(reason, 8000);
          });
  chipsLay->addWidget(chips);
  chipsLay->addStretch(1);
  canvasLay->addWidget(chipsRow);
  if (m_canvasCtl)
  {
    QgsMapCanvas *canvas = m_canvasCtl->canvas();
    // Context belongs to the map surface, so it disappears on the data page.
    auto *contextRow = new QWidget(canvasPane);
    contextRow->setObjectName(QStringLiteral("mapInteractionContext"));
    auto *contextLayout = new QHBoxLayout(contextRow);
    contextLayout->setContentsMargins(8, 4, 8, 4);
    contextLayout->setSpacing(8);
    auto *context = new QLabel(contextRow);
    context->setObjectName(QStringLiteral("mapInteractionHint"));
    context->setWordWrap(true);
    context->setTextFormat(Qt::PlainText);
    context->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *stop = new QToolButton(contextRow);
    stop->setObjectName(QStringLiteral("stopMapToolButton"));
    stop->setText(tr("结束工具"));
    stop->setToolTip(tr("结束当前地图操作，保留尚未保存的图层编辑"));
    stop->setAccessibleName(stop->toolTip());
    connect(stop, &QToolButton::clicked, this, [this] { m_canvasCtl->deactivateTool(); });
    const auto syncContext = [context, stop, canvas] {
      QgsMapTool *tool = canvas->mapTool();
      QAction *action = tool ? tool->action() : nullptr;
      const QString mode = action ? action->text().remove(QLatin1Char('&'))
                                  : tool ? tool->toolName() : tr("浏览");
      const QString target = canvas->currentLayer() ? canvas->currentLayer()->name() : tr("未选择图层");
      const QString hint = action ? action->toolTip()
                                 : tool ? tr("在地图中操作；结束工具后保留图层编辑")
                                        : tr("选择上方地图工具开始操作");
      const QString text = tr("%1 · %2 — %3").arg(mode, target, hint);
      context->setText(text);
      context->setToolTip(text);
      context->setAccessibleName(text);
      stop->setEnabled(tool != nullptr);
      stop->setToolTip(tool ? tr("结束当前地图操作，保留尚未保存的图层编辑")
                            : tr("当前没有活动地图工具"));
    };
    connect(canvas, &QgsMapCanvas::mapToolSet, contextRow, syncContext);
    connect(canvas, &QgsMapCanvas::currentLayerChanged, contextRow, syncContext);
    syncContext();
    contextLayout->addWidget(context, 1);
    contextLayout->addWidget(stop);
    canvasLay->addWidget(contextRow);
    canvasLay->addWidget(canvas, 1);
  }
  else
    canvasLay->addStretch(1);
  m_workspaceStack->addWidget(canvasPane); // 0

  // Data navigation participates in the same native dock layout as map panels.
  m_dataListDock = new PaleoDockWidget(tr("数据列表"), this);
  m_dataListDock->setObjectName(QStringLiteral("dataListDock"));
  m_dataListHost = new QWidget(m_dataListDock);
  m_dataListHost->setObjectName(QStringLiteral("dataListPanel"));
  auto *listHostLay = new QVBoxLayout(m_dataListHost);
  listHostLay->setContentsMargins(0, 0, 0, 0);
  m_dataListDock->setWidget(m_dataListHost);
  addDockWidget(Qt::LeftDockWidgetArea, m_dataListDock);
  m_previewTabs = new DataPreviewTabs(m_workspaceStack);
  m_previewTabs->setObjectName(QStringLiteral("dataPreview"));
  m_previewTabs->setAccessibleName(tr("数据预览"));
  m_previewTabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  m_workspaceStack->addWidget(m_previewTabs); // 1: all remaining space is visualization
  m_centerStack->addWidget(m_workspaceStack);

  // 画布装饰管理器（D11 临时配准水印等）：parent 到 canvas，renderComplete
  // 自连；各项默认关，按需 setEnabled。
  if (m_canvasCtl && m_canvasCtl->canvas())
    m_decorMgr = new PaleoDecorationManager(m_canvasCtl->canvas(), this);

  connect(m_previewTabs, &DataPreviewTabs::previewMaximizeToggled, this,
          [this](bool on) {
    m_previewMaximized = on;
    if (on) {
      m_preMaxWindowState = saveState();
      m_dataListDock->setProgrammaticVisible(false);
      m_rightDock->hide();
    } else if (!m_preMaxWindowState.isEmpty()) {
      restoreState(m_preMaxWindowState);
      m_preMaxWindowState.clear();
    }
  });
  if (auto *inner = m_previewTabs->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")))
    connect(inner, &QTabWidget::currentChanged, this, [this, inner](int) {
      if (inner->count() == 0 && m_previewMaximized)
        if (auto *button = m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton")))
          button->setChecked(false);
    });

  // ---- T31 空态：地图没有图层时画布上的居中指引（共享组件）----
  PaleoEmptyStateLabel *mapEmpty = nullptr;
  if (m_canvasCtl)
  {
    mapEmpty = new PaleoEmptyStateLabel(
        QCoreApplication::translate("PaleoMainWindow",
                                    "地图上还没有图层 — 先在「数据管理」导入工区文件夹，或在「预测编图」运行预测"),
        m_canvasCtl->canvas());
    mapEmpty->setObjectName(QStringLiteral("mapEmptyState")); // tst_ui 依赖的稳定名
    mapEmpty->raise();
  }

  // Startup-page actions: dialogs only exist when a real platform is present;
  // offscreen the buttons exist but stay inert (no modal QFileDialog).
  if (auto *openBtn = startup->findChild<QPushButton *>(QStringLiteral("openProjectButton")))
    connect(openBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getOpenFileName(
          this, tr("打开工程"), QString(),
          QStringLiteral("Paleo 工程 (*.paleo);;QGIS 工程 (*.qgz *.qgs)"));
      if (p.isEmpty())
        return;
      // §38 blocking-error contract: a failed open surfaces as a dialog, not
      // a silent no-op on the startup page.
      if (!m_projectSvc->openProject(p))
        QMessageBox::critical(this, tr("打开工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
    });
  if (auto *newBtn = startup->findChild<QPushButton *>(QStringLiteral("newProjectButton")))
    connect(newBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      const QString p = QFileDialog::getSaveFileName(
          this, tr("新建工程"), QString(), QStringLiteral("Paleo 工程 (*.qgz)"));
      if (p.isEmpty())
        return;
      if (!m_projectSvc->createProject(p))
        QMessageBox::critical(this, tr("新建工程失败"),
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
          QFileDialog::getExistingDirectory(this, tr("从工区文件夹新建工程"));
      if (!dir.isEmpty())
        openPath(dir);
    });
  if (auto *list = startup->findChild<QListWidget *>(QStringLiteral("recentProjectsList")))
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
      const QString p = item ? item->data(Qt::UserRole).toString() : QString();
      if (p.isEmpty() || !m_projectSvc)
        return;
      if (!m_projectSvc->openProject(p))
        QMessageBox::critical(this, tr("打开工程失败"),
                              m_projectSvc->lastErrors().join(QLatin1Char('\n')));
    });

  setCentralWidget(m_centerStack);

  // ---- left dock: 图层平台（档案工具条 + 图层树面板），替换裸 QgsLayerTreeView ----
  m_leftDock = new PaleoDockWidget(tr("图层"), this);
  m_leftDock->setObjectName(QStringLiteral("layerTreeDock"));
  if (m_projectSvc && m_projectSvc->project() && m_projectSvc->project()->layerTreeRoot())
  {
    // 页面档案服务 + 属性对话框 + 面板组装（objectName 兼容由面板自持：
    // layerTreeView / layerTreeEmptyState，tst_ui 依赖）。
    m_profileSvc = new QgisLayerProfileService(m_projectSvc->project(), this);
    m_profileSvc->setLayerService(m_layerSvc);
    m_layerProps = new LayerPropertiesDialog(
        m_layerSvc,
        {m_canvasCtl ? m_canvasCtl->canvas() : nullptr, nullptr}, this);
    m_layerPanel = new LayerTreePanel(
        m_projectSvc->project(), m_canvasCtl ? m_canvasCtl->canvas() : nullptr,
        m_layerSvc, m_leftDock);
    m_profileSvc->setLayerTreeModel(m_layerPanel->layerTreeModel());
    m_profileBar = new LayerProfileBar(m_profileSvc, m_leftDock);
    // 主题应用失败等面板内提示落到状态栏（用户可见反馈回路）。
    connect(m_profileBar, &LayerProfileBar::statusMessage, this,
            [this](const QString &text) {
              if (statusBar())
                statusBar()->showMessage(text, 8000);
            });
    connect(m_layerPanel, &LayerTreePanel::propertiesRequested, m_layerProps,
            &LayerPropertiesDialog::openLayerProperties);
    connect(m_layerPanel, &LayerTreePanel::mappingPageRequested, this,
            &PaleoMainWindow::showPage);
    connect(m_layerProps, &LayerPropertiesDialog::assetInspectionRequested, this,
            [this](const QString &) { showPage(QStringLiteral("data")); });
    // 层位切换 → 重放页面档案：horizonReleased 早于 activeHorizon 落位
    //（qgislayerservice 实测），排队到事件循环尾，钉死「先换层位（实例化/
    // 释放）再应用主题」的时序；多信号去抖合并为一次重放。
    if (m_layerSvc)
    {
      const auto queueProfileReplay = [this]() {
        if (m_profileReplayQueued)
          return;
        m_profileReplayQueued = true;
        QMetaObject::invokeMethod(
            this,
            [this]() {
              m_profileReplayQueued = false;
              if (m_profileSvc)
                m_profileSvc->applyCurrentPageProfile();
            },
            Qt::QueuedConnection);
      };
      connect(m_layerSvc, &QgisLayerService::horizonReleased, this, queueProfileReplay);
      connect(m_layerSvc, &QgisLayerService::layerInstantiated, this, queueProfileReplay);
    }
    auto *layerHost = new QWidget(m_leftDock);
    auto *layerLayout = new QVBoxLayout(layerHost);
    layerLayout->setContentsMargins(0, 0, 0, 0);
    layerLayout->setSpacing(0);
    layerLayout->addWidget(m_profileBar);
    layerLayout->addWidget(m_layerPanel, 1);
    m_leftDock->setWidget(layerHost);
  }
  else
  {
    m_leftDock->setWidget(new QLabel(tr("未打开工程"), m_leftDock));
  }
  addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);

  // 地图空态随工程图层集显隐（T31）；图层树空态由 LayerTreePanel 自持。
  if (QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr)
  {
    const auto updateEmptyStates = [proj, mapEmpty]() {
      if (mapEmpty)
        mapEmpty->setVisible(proj->mapLayers().isEmpty());
    };
    updateEmptyStates();
    connect(proj, &QgsProject::layersAdded, this,
            [updateEmptyStates](const QList<QgsMapLayer *> &) { updateEmptyStates(); });
    connect(proj, &QgsProject::layersRemoved, this,
            [updateEmptyStates](const QStringList &) { updateEmptyStates(); });
  }

  // ---- right dock: per-page panel stack (placeholders until attachWorkflows) ----
  // 标题随页（pageDockTitles()）；ribbon 里的「参数」钮就是它的 toggleViewAction。
  m_rightDock = new QDockWidget(pageDockTitles().first(), this);
  m_rightDock->setObjectName(QStringLiteral("pagePanelDock"));
  auto *panelHost = new QWidget(m_rightDock);
  panelHost->setObjectName(QStringLiteral("rightPanelHost"));
  auto *panelStack = new QStackedLayout(panelHost);
  for (const QString &label : pageLabels())
  {
    auto *placeholder = new QLabel(label + tr(" — 面板待实现"), panelHost);
    placeholder->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    placeholder->setMargin(12);
    panelStack->addWidget(placeholder);
  }
  m_rightDock->setWidget(panelHost);
  addDockWidget(Qt::RightDockWidgetArea, m_rightDock);

  // ---- bottom dock: log + tasks tabs ----
  m_bottomDock = new PaleoDockWidget(tr("日志 / 任务"), this);
  m_bottomDock->setObjectName(QStringLiteral("bottomDock"));
  auto *bottomTabs = new QTabWidget(m_bottomDock);
  bottomTabs->setObjectName(QStringLiteral("bottomTabs"));
  bottomTabs->setAccessibleName(tr("底部面板页签"));
  // Qt::Widget flags keep the QDialog-based viewer embeddable as a tab page.
  bottomTabs->addTab(new QgsMessageLogViewer(bottomTabs, Qt::Widget), tr("日志"));
  auto *tasks = new QTextEdit(bottomTabs);
  tasks->setObjectName(QStringLiteral("tasksPlaceholder"));
  tasks->setReadOnly(true);
  tasks->setPlaceholderText(tr("任务队列 — 待实现"));
  bottomTabs->addTab(tasks, tr("任务"));
  m_bottomDock->setWidget(bottomTabs);
  addDockWidget(Qt::BottomDockWidgetArea, m_bottomDock);
  m_bottomDock->setUserWantsVisible(false);
  m_bottomDock->setProgrammaticVisible(false);

  // ---- seismic section dock (Phase 4: 2D arbitrary line & well section dock) ----
  m_seismicSectionDock = new seismic::SeismicSectionDockWidget(tr("地震剖面 / 井震综合"), this);
  m_seismicSectionDock->setObjectName(QStringLiteral("seismicSectionDock"));
  addDockWidget(Qt::BottomDockWidgetArea, m_seismicSectionDock);
  tabifyDockWidget(m_bottomDock, m_seismicSectionDock);
  m_seismicSectionDock->hide();

  // ---- seismic 3D viewport dock ----
  m_seismic3dDock = new QDockWidget(tr("三维地震视口 (3D)"), this);
  m_seismic3dDock->setObjectName(QStringLiteral("seismic3dDock"));
  m_seismic3dPanel = new seismic::Seismic3DViewPanel(m_seismic3dDock);
  m_seismic3dPanel->setObjectName(QStringLiteral("seismic3dPanel"));
  m_seismic3dDock->setWidget(m_seismic3dPanel);
  addDockWidget(Qt::RightDockWidgetArea, m_seismic3dDock);
  m_seismic3dDock->hide();

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

  // ---- status bar: active horizon ----
  //（「数据提供器：N」常驻诊断标签已移除——provider 计数是启动期自检信息，
  // 不属于用户态状态栏；诊断仍可从日志/QgisRuntime 读。）
  auto *horizonLabel = new QLabel(this);
  horizonLabel->setObjectName(QStringLiteral("statusHorizon"));
  const auto horizonText = [](const QString &h) {
    return tr("层位：%1").arg(h.isEmpty() ? QStringLiteral("—") : h);
  };
  horizonLabel->setText(horizonText(m_selection ? m_selection->activeHorizon() : QString()));
  statusBar()->addPermanentWidget(horizonLabel);

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
              // 固定 3 位小数（默认 arg(double) 只有 6 位有效数字，读数
              // 位数随量级跳动）；tnum 等宽数字面下宽度稳定。
              coordLabel->setText(QStringLiteral("%1, %2")
                                      .arg(QLocale().toString(p.x(), 'f', 3),
                                           QLocale().toString(p.y(), 'f', 3)));
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
    crsLabel->setText(tr("工程坐标 · 米 · 未投影"));
    crsLabel->setToolTip(tr("局部工程坐标，单位米，未投影 — 不是经纬度"));
    PaleoTheme::applyThemedStyleSheet(
        crsLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    statusBar()->addPermanentWidget(crsLabel);
  }
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, horizonLabel,
            [horizonLabel, horizonText](const QString &h) { horizonLabel->setText(horizonText(h)); });

  // DESIGN.md tokens on shell chrome only — no custom painting. ribbon 本体
  // 的颜色来自 PaleoTheme::ribbonPaletteJson（office2021 模板）。
  // T32：拼上全局 2px 键盘焦点环（替代 Fusion 虚线框）。壳 QSS 与焦点环
  // 都从 PaleoTheme 取（缺省=当前主题）——暗色下整套壳 chrome 跟随。
  const QString shellQss =
      PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet();
  PaleoRibbon::applyTheme(this, shellQss);
  // SARibbonMainWindow 构造时排了一次 singleShot(0) 重套内置主题（会整体
  // 覆盖样式表）；零时定时器按注册顺序触发——这里再排一次，保证最后落的
  // 是 DESIGN.md 这套。
  QTimer::singleShot(0, this, [this] { reapplyThemeChrome(); });
}

void PaleoMainWindow::reapplyThemeChrome()
{
  const QString shellQss =
      PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet();
  PaleoRibbon::applyTheme(this, shellQss);
}

void PaleoMainWindow::setDarkThemeEnabled(bool dark)
{
  // 唯一主题写者：只有用户显式切换（面板菜单「深色模式」勾选）才写盘。
  PaleoTheme::writeThemeToSettings(
      dark ? PaleoTheme::Theme::Dark : PaleoTheme::Theme::Light);
  PaleoTheme::applyTheme(
      dark ? PaleoTheme::Theme::Dark : PaleoTheme::Theme::Light);
  reapplyThemeChrome(); // SARibbon 调色板不跟 palette 事件，显式重套
  if (statusBar())
    statusBar()->showMessage(dark ? tr("已切换到深色模式") : tr("已切换到浅色模式"),
                             4000);
}

void PaleoMainWindow::buildRibbon()
{
  SARibbonBar *bar = ribbonBar();
  if (!bar)
    return;
  bar->setRibbonStyle(SARibbonBar::RibbonStyleCompactThreeRow);
  bar->setPanelSpacing(8);
  bar->setPanelToolButtonIconSize(QSize(16, 16), QSize(24, 24));
  bar->setEnableWordWrap(false);
  bar->setTabDoubleClickToMinimumMode(true); // 双击页签收起/展开 ribbon（Office 惯例）
  if (SARibbonTabBar *tabs = bar->ribbonTabBar())
  {
    tabs->setObjectName(QStringLiteral("workflowTabs"));
    tabs->setAccessibleName(tr("工作流步骤"));
    // T32 tab 溢出策略：超宽走滚动按钮（显式钉住防样式/平台漂移）。
    tabs->setUsesScrollButtons(true);
  }

  // ---- 五个页签 = 五个工作流页（页签序 = paleo::pagesinternal::kPageIds 序）----
  for (int i = 0; i < paleo::pagesinternal::kPageIds.size(); ++i)
  {
    SARibbonCategory *cat = bar->addCategoryPage(pageLabels().at(i));
    cat->setObjectName(QStringLiteral("ribbonCategory.") + paleo::pagesinternal::kPageIds.at(i));
    cat->setProperty("paleo.pageId", paleo::pagesinternal::kPageIds.at(i));
  }
  // W5 键盘可达：Ctrl+1..5 直切五个工作流页（页序 = 工作流链序）。
  for (int i = 0; i < paleo::pagesinternal::kPageIds.size(); ++i)
  {
    auto *sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
    sc->setObjectName(QStringLiteral("pageShortcut.") + paleo::pagesinternal::kPageIds.at(i));
    connect(sc, &QShortcut::activated, this, [this, i] {
      showPage(paleo::pagesinternal::kPageIds.at(i));
    });
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
    viaStartup(tr("新建工程(&N)…"), "mActionFileNew.svg", "newProjectButton");
    viaStartup(tr("打开工程(&O)…"), "mActionFileOpen.svg", "openProjectButton");
    viaStartup(tr("从工区文件夹新建(&I)…"), "mIconFolderOpen.svg", "importFromFolderButton");
    menu->addSeparator()->setObjectName(QStringLiteral("fileMenuSaveAnchor"));
    QAction *home =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mIconFolderHome.svg")), tr("起始页(&H)"));
    connect(home, &QAction::triggered, this, [this] { showStartup(); });
    menu->addSeparator();
    QAction *quit =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileExit.svg")), tr("退出(&X)"));
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

  // 布局管理入口：右键 dock 标题栏使用同一菜单，包含显隐与布局命令。
  auto *panelsBtn = new QToolButton(right);
  panelsBtn->setObjectName(QStringLiteral("panelsMenuButton"));
  panelsBtn->setText(tr("布局"));
  panelsBtn->setAccessibleName(tr("布局与面板管理"));
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
  // 每次现建，反映动态注册的面板与已保存的布局。
  if (QMenu *menu = m_dockManager->createMenu(this))
  {
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSeparator();
    QAction *dark = menu->addAction(tr("深色模式"));
    dark->setObjectName(QStringLiteral("themeToggleAction"));
    dark->setCheckable(true);
    dark->setChecked(PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark);
    connect(dark, &QAction::toggled, this, &PaleoMainWindow::setDarkThemeEnabled);
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
  const int idx = paleo::pagesinternal::kPageIds.indexOf(pageId);
  if (idx < 0)
  {
    qWarning() << "PaleoMainWindow::showPage — unknown page id:" << pageId;
    return;
  }
  if (pageId != QLatin1String("data") && m_previewMaximized)
    if (auto *button = m_previewTabs->findChild<QToolButton *>(QStringLiteral("previewMaxButton")))
      button->setChecked(false);
  m_currentPage = pageId;

  // 页签 = 页：切到对应 ribbon 页签（currentRibbonTabChanged 回到这里时
  // id == m_currentPage，不重入）。
  if (SARibbonCategory *cat = categoryForPage(pageId))
    if (ribbonBar()->currentIndex() != ribbonBar()->categoryIndex(cat))
      ribbonBar()->raiseCategory(cat);

  // Scroll hosts isolate content hints; switching a page never resizes a dock.
  if (auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost")))
    if (auto *stack = static_cast<QStackedLayout *>(host->layout()))
      stack->setCurrentIndex(idx);
  if (m_rightDock)
    m_rightDock->setWindowTitle(pageDockTitles().at(idx));

  // ---- m2(D): 页面图层档案——切到编图页即应用该页档案（数据页 no-op）。
  // 档案表在 QgisLayerProfileService（predict/constraint/compose/validate）。
  applyCurrentPageProfile();

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

  m_dataListDock->setProgrammaticVisible(pageId == QLatin1String("data") &&
                                        m_dataListDock->userWantsVisible() && !m_previewMaximized);

  // 数据页面纯三栏布局（数据导航树 | 预览可视化 | 属性面板），不要图层树与底部的横向面板（日志/任务等）。
  // 离开数据页面切到编图页面时，恢复用户期望的图层树与底栏状态。
  if (pageId == QLatin1String("data"))
  {
    if (m_leftDock)
      m_leftDock->setProgrammaticVisible(false);
    if (m_bottomDock)
      m_bottomDock->setProgrammaticVisible(false);
  }
  else
  {
    if (m_leftDock && m_leftDock->userWantsVisible())
      m_leftDock->setProgrammaticVisible(true);
    // W2：任务驱动露出的底栏（m_bottomDockAutoShown）不随切页收回。
    if (m_bottomDock && (m_bottomDock->userWantsVisible() || m_bottomDockAutoShown))
      m_bottomDock->setProgrammaticVisible(true);
  }

  // 图层平台：页面档案——不同页面激活不同图层组（QgsMapThemeCollection，
  // data 页 no-op）；档案工具条同步当前页指示。
  if (m_profileSvc)
  {
    m_profileSvc->applyPageProfile(pageId);
    if (m_profileBar)
      m_profileBar->setCurrentPage(pageId);
  }


  // 页作用域工具面：编辑命令组只在编图链三页的 ribbon 里。落到非编辑页
  // 时停用活动画布工具——各工具 deactivate() 统一发 abort 信号，约束捕获/
  // 编辑会话经 owner 的 abort 路径拆台（等价 §42.15 的 Esc）。
  if (!kEditingToolPages.contains(pageId) && m_canvasCtl)
    m_canvasCtl->deactivateTool();
}

// ---------------------------------------------------------------------------
// T22 文件夹导入确认表：类型下拉从分类器词表构建（label↔type 稳定映射，
// type 存 Qt::UserRole——不靠显示文本反推）；HZ28-6-1 行锁定为参考；
// 「参考资料」目录内井类/未判内容默认显示「参考」（可改，成 override 送达
// 后端）；覆盖只收「合法且不同于分类器原类型」的行；Failed 行给「重试」。
// ---------------------------------------------------------------------------

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

FolderImportWorkflow *PaleoMainWindow::folderImportWorkflow()
{
  // W2：文件夹导入编排在 workflow/folderimport；壳只留目录拾取 +
  // 对话框 exec + 视图出口回调。首次用到时按当前服务实例懒建。
  if (!m_folderImportWf && m_importSvc)
  {
    m_folderImportWf = new FolderImportWorkflow(m_importSvc, m_taskSvc, this);
    connect(m_folderImportWf, &FolderImportWorkflow::importActiveChanged, this,
            [this](bool active) { m_folderImportActive = active; });
  }
  return m_folderImportWf;
}

void PaleoMainWindow::runFolderImportAt(DataImportService *svc,
                                        const QString &dir)
{
  if (!svc)
    return;
  auto *wf = folderImportWorkflow();
  QString err;
  const auto preview = wf ? wf->previewFolder(dir, &err)
                          : QVector<FolderPreviewRow>();
  if (preview.isEmpty())
  {
    QMessageBox::warning(this, tr("导入工区文件夹"),
                         err.isEmpty() ? tr("目录里没有可导入的文件") : err);
    return;
  }

  QDialog dlg(this);
  PaleoFolderConfirm::Hooks hooks;
  hooks.importRow = [wf](const QString &path, const QString &force) {
    return wf->importFolderRow(path, force, nullptr);
  };
  hooks.importAll = [wf, dir](const QMap<QString, QString> &overrides,
                              FolderImportWorkflow::ImportDone done) {
    wf->importFolder(dir, overrides, std::move(done));
  };
  hooks.importedWellHead = [wf](const QString &rowPath) {
    return wf->importedWellHeadAsset(rowPath);
  };
  hooks.showUnresolved = [this] {
    showPage(QStringLiteral("data"));
    if (auto *page = findChild<DataPage *>())
      page->setUnresolvedFilter(true);
  };
  // 保持旧语义：井口入库的预览标签排队到对话框信号处理完成后开。
  hooks.previewAsset = [this](const QString &assetId) {
    QMetaObject::invokeMethod(
        this,
        [this, assetId] {
          if (m_previewTabs)
            m_previewTabs->openAsset(assetId);
        },
        Qt::QueuedConnection);
  };
  hooks.stampSourceArea = [this, dir](const QVariantMap &stats) {
    stampSourceArea(dir, stats);
  };
  PaleoFolderConfirm::buildFolderConfirmDialog(&dlg, dir, preview, hooks);
  dlg.exec();
}

void PaleoMainWindow::stampSourceArea(const QString &dir,
                                      const QVariantMap &stats)
{
  if (auto *wf = projectOpenWorkflow())
    wf->stampSourceArea(dir, stats);
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

ProjectOpenWorkflow *PaleoMainWindow::projectOpenWorkflow()
{
  // W2：openPath 编排在 workflow/projectopen（路径判别/新建/sourceArea
  // 回写）；壳只剩错误弹窗与「文件夹导入」意图的接线。
  if (!m_projectOpenWf && m_projectSvc)
  {
    m_projectOpenWf = new ProjectOpenWorkflow(m_projectSvc, this);
    connect(m_projectOpenWf, &ProjectOpenWorkflow::openFailed, this,
            [this](const QString &title, const QString &detail, bool fatal) {
              if (isOffscreen())
                return;
              if (fatal)
                QMessageBox::critical(this, title, detail);
              else
                QMessageBox::warning(this, title, detail);
            });
    connect(m_projectOpenWf, &ProjectOpenWorkflow::folderImportRequested, this,
            [this](const QString &dir) {
              if (m_importSvc)
                runFolderImportAt(m_importSvc, dir);
              else if (!isOffscreen())
                QMessageBox::information(
                    this, tr("从工区文件夹新建"),
                    tr("工程已创建于 %1；导入服务未就绪，请在数据页手动导入该文件夹。")
                        .arg(dir));
            });
  }
  return m_projectOpenWf;
}

bool PaleoMainWindow::openPath(const QString &path)
{
  if (path.isEmpty())
    return false;
  auto *wf = projectOpenWorkflow();
  return wf ? wf->openPath(path) : false;
}

void PaleoMainWindow::showStartup()
{
  setProjectReadOnly(false);
  m_currentPage = QStringLiteral("startup");
  if (m_dataListDock)
    m_dataListDock->setProgrammaticVisible(false);
  if (m_centerStack)
    m_centerStack->setCurrentIndex(0);
}

void PaleoMainWindow::onProjectOpened()
{
  if (m_centerStack)
    m_centerStack->setCurrentIndex(1); // startup -> workspace canvas

  updateWindowTitle(); // 标题跟随工程名（「<工程名> — Paleo Workbench [*]」）

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
  showPage(paleo::pagesinternal::kPageIds.contains(last) ? last : paleo::pagesinternal::kPageIds.first());
}

void PaleoMainWindow::closeEvent(QCloseEvent *event)
{
  // C1 关窗数据保护：编辑中且有未提交改动的矢量图层 → 保存/放弃/取消
  // 三选一。走编辑服务（busy 挂账随 commit/rollback 清），无服务时直连
  // commitChanges/rollBack。offscreen（无头测试/渲染环境）不弹模态框——
  // 弹了没人点会挂死事件循环，保持旧行为直接放行。
  if (m_projectSvc && m_projectSvc->project() && !isOffscreen())
  {
    QList<QgsVectorLayer *> dirty;
    QStringList dirtyNames;
    const auto layers = m_projectSvc->project()->mapLayers();
    for (QgsMapLayer *l : layers)
      if (auto *vl = qobject_cast<QgsVectorLayer *>(l))
        if (vl->isEditable() && vl->isModified())
        {
          dirty << vl;
          dirtyNames << (vl->name().isEmpty() ? vl->id() : vl->name());
        }
    if (!dirty.isEmpty())
    {
      // 显式中文按钮文案（标准按钮的翻译依赖 Qt 自带 qtbase 翻译目录，
      // 未装载时会漏英文——i18n 决策 2026-09-29 用户可见串必须中文）。
      QMessageBox box(QMessageBox::Warning, tr("未保存的编辑"),
                      tr("以下图层有未保存的编辑：\n%1\n\n关闭前如何处理？")
                          .arg(dirtyNames.join(QLatin1Char('\n'))),
                      QMessageBox::NoButton, this);
      QPushButton *saveBtn = box.addButton(tr("保存"), QMessageBox::AcceptRole);
      QPushButton *discardBtn = box.addButton(tr("放弃"), QMessageBox::DestructiveRole);
      box.addButton(tr("取消"), QMessageBox::RejectRole);
      box.setDefaultButton(saveBtn);
      box.exec();
      if (box.clickedButton() != saveBtn && box.clickedButton() != discardBtn)
      {
        event->ignore(); // 取消（含 Esc/窗口 ✕）
        return;
      }
      if (box.clickedButton() == saveBtn)
      {
        for (QgsVectorLayer *vl : dirty)
        {
          QString err;
          const bool ok = m_editSvc ? m_editSvc->commitEdit(vl, &err)
                                    : vl->commitChanges();
          if (!ok)
          {
            QMessageBox::critical(
                this, tr("保存编辑失败"),
                tr("图层「%1」的编辑未能提交，窗口不会关闭。")
                    .arg(vl->name().isEmpty() ? vl->id() : vl->name()));
            event->ignore();
            return;
          }
        }
      }
      else // 放弃
        for (QgsVectorLayer *vl : dirty)
        {
          if (m_editSvc)
            m_editSvc->rollbackEdit(vl);
          else
            vl->rollBack();
        }
    }
  }
  saveWindowState();
  SARibbonMainWindow::closeEvent(event);
}

void PaleoMainWindow::updateWindowTitle()
{
  QString name;
  if (m_projectSvc && !m_projectSvc->projectPath().isEmpty())
    name = QFileInfo(m_projectSvc->projectPath()).completeBaseName();
  if (m_isProjectReadOnly)
  {
    setWindowTitle(name.isEmpty() ? tr("Paleo Workbench [只读]")
                                  : tr("%1 — Paleo Workbench [只读]").arg(name));
  }
  else
  {
    setWindowTitle(name.isEmpty() ? tr("Paleo Workbench [*]")
                                  : tr("%1 — Paleo Workbench [*]").arg(name));
  }
}

void PaleoMainWindow::setProjectReadOnly(bool readOnly)
{
  m_isProjectReadOnly = readOnly;
  updateWindowTitle();

  if (auto *saveAct = findChild<QAction *>(QStringLiteral("saveProjectAction")))
  {
    saveAct->setEnabled(!readOnly);
    saveAct->setToolTip(readOnly ? tr("工程处于只读模式（另一个实例持有写锁）") : tr("保存工程（Ctrl+S）"));
  }

  if (auto *editTb = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
  {
    editTb->setEnabled(!readOnly);
  }

  if (statusBar() && readOnly)
  {
    statusBar()->showMessage(tr("工程以只读模式运行（写操作已禁用）"), 10000);
  }
}

void PaleoMainWindow::syncBottomDockForTasks()
{
  if (!m_bottomDock)
    return;
  bool anyRunning = false;
  if (m_taskSvc)
    for (const PaleoTask *t : m_taskSvc->tasks())
      // quiet 交互任务（三维切片/剖面解码/预取）不拉起任务中心——交互控件
      // 自带进度语义，弹出反而打断操作。
      anyRunning |= t->running() && !t->quiet();
  if (anyRunning)
  {
    // 有活动任务且底栏藏着 → 程序化露出并切到任务页（不动
    // userWantsVisible）。用户中途手动关掉即尊重其选择，不再反复拉起。
    if (!m_bottomDockAutoShown && !m_bottomDock->isVisible())
    {
      m_bottomDockAutoShown = true;
      m_bottomDock->setProgrammaticVisible(true);
      if (auto *tabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
        if (auto *panel = findChild<TaskPanel *>(QStringLiteral("taskPanel")))
          tabs->setCurrentWidget(panel);
      m_bottomDock->raise();
    }
  }
  else if (m_bottomDockAutoShown)
  {
    // 任务清空 → 恢复用户原可见态（数据页的纯三栏布局照常隐藏底栏）。
    m_bottomDockAutoShown = false;
    const bool want = m_currentPage != QLatin1String("data") &&
                      m_bottomDock->userWantsVisible();
    m_bottomDock->setProgrammaticVisible(want);
  }
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
  if (!m_canvasCtl || !m_projectSvc || !m_projectSvc->project() || m_projectSvc->projectPath().isEmpty())
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


// ---------------------------------------------------------------------------
// D11 临时配准（手工仿射 → DERIVED + 水印图层）
// ---------------------------------------------------------------------------
void PaleoMainWindow::applyProvisionalRegistration(DataImportService *svc,
                                                   const QString &assetId,
                                                   const QVariantMap &params)
{
  // W3：编排在 workflow/registration（catalog 写 + 图层实例化 + 计数）；
  // 壳只接信号做状态栏文案/画布水印/zoomToLayer。
  if (!svc || !m_layerSvc)
    return;
  if (!m_registrationWf)
  {
    m_registrationWf = new RegistrationWorkflow(svc, m_layerSvc, this);
    connect(m_registrationWf, &RegistrationWorkflow::registrationFailed, this,
            [this](const QString &text) {
              if (statusBar())
                statusBar()->showMessage(text, 8000);
              QgsMessageLog::logMessage(text, QStringLiteral("Paleo"),
                                      Qgis::MessageLevel::Warning);
            });
    connect(m_registrationWf, &RegistrationWorkflow::provisionalRegistered, this,
            [this](const QString &layerId, const QString &title,
                   int featureCount) {
              if (m_canvasCtl)
                m_canvasCtl->zoomToLayer(layerId);
              if (statusBar())
                statusBar()->showMessage(
                    tr("临时配准已上图：%1 · %2 个要素（手工仿射，非权威坐标）")
                        .arg(title)
                        .arg(featureCount),
                    8000);
            });
    connect(m_registrationWf,
            &RegistrationWorkflow::provisionalLayerCountChanged, this,
            [this](int n) {
              m_provisionalLayers = n; // 单向锁存语义原样（计数只增）
              if (m_decorMgr)
                m_decorMgr->setWatermarkEnabled(n > 0);
            });
  }
  m_registrationWf->applyProvisionalRegistration(assetId, params);
}

void PaleoMainWindow::syncSeismicVolumeToDocks()
{
  DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
  if (!cat)
    return;
  for (const CatalogAsset &a : cat->assets())
  {
    if (a.type == QLatin1String("seismic"))
    {
      const CatalogVersion tv = cat->currentVersion(a.id);
      const QString abs = tv.id.isEmpty() ? QString() : m_previewDoc->absolutePathForVersion(tv);
      if (!abs.isEmpty() && QFile::exists(abs))
      {
        if (m_seismic3dPanel && (m_seismic3dPanel->volume() == nullptr ||
                                 QString::fromStdString(m_seismic3dPanel->volume()->Path().string()) != abs))
        {
          if (m_seismicTaskSvc)
            m_seismic3dPanel->setTaskService(m_seismicTaskSvc.get());
          auto vol = std::make_shared<seismic::SgyVolume>();
          std::string volErr;
          if (vol->Load(abs.toStdString(), volErr))
          {
            m_seismic3dPanel->setVolume(vol);
            if (m_seismic3dPanel->viewport())
            {
              m_seismic3dPanel->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
              m_seismic3dPanel->viewport()->fitToBounds();
            }
          }
        }
        if (m_seismicSectionDock && (m_seismicSectionDock->volume() == nullptr ||
                                     QString::fromStdString(m_seismicSectionDock->volume()->Path().string()) != abs))
        {
          auto vol = std::make_shared<seismic::SgyVolume>();
          std::string volErr;
          if (vol->Load(abs.toStdString(), volErr))
          {
            double origin = 0;
            for (const auto &link : cat->linksForAsset(a.id))
              if (!link.unresolved && link.entityType == "seismic_survey") {
                origin = cat->entityById(link.entityId).startTimeMs;
                break;
              }
            m_seismicSectionDock->setTimeOriginMs(origin);
            if (m_sectionLink)
              m_sectionLink->setActiveVolume(vol);
            else
              m_seismicSectionDock->setVolume(vol);
          }
        }
        break;
      }
    }
  }
}

