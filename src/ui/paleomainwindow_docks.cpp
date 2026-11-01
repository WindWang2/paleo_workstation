// 层：视图
// paleomainwindow_docks — 壳构建域（方向 83 拆 TU）：中央面（启动页/画布面/
// 数据面/地层对比页）、左 dock 图层平台、右栏占位与底栏/地震/连井/3D/Web
// dock 族。原 buildShell 六段中的三段，按原文行序自本体移入（初始化序/
// 连接序逐行保持；编排见 paleomainwindow.cpp 的 buildShell）。
#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"
#include "uienv_internal.h"
#include "../workflow/projectsetup.h"

#include "paleodockmanager.h"
#include "paleoviewport.h" // PaleoViewportStack + PaleoPanelHost
#include "paleoemptystate.h" // T31 空态卡片共享组件
#include "paleotheme.h"
#include "windowsizing_internal.h" // charColumns（R7 #885 启动页块宽）
#include "paleoicons.h"
#include "notifications/paleonotify.h" // 启动页「新建工程失败」弹窗
#include "decorations/paleodecorations.h" // PaleoDecorationManager（D11 水印宿主）

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../metadata/layermanifest.h" // LayerDeclaration（剖面/时切片声明 declare 装配）
#include "../qgis/qgislabelzorder.h"
#include "../qgis/qgislayerprofile.h"
#include "datapreview/datapreviewtabs.h"
#include "horizonchipbar.h"
#include "subintervalselectorbar.h"
#include "../domain/mappinghorizons.h"
#include "layers/layertreepanel.h"
#include "../workflow/mappingworkbench.h"
#include "layers/layerpropertiesdialog.h"
#include "pages/pageshared.h"
#include "pages/stratigraphypage.h"
#include "../workflow/stratigraphyworkflow.h"
#include "../linkage/selectioncontext.h"
#include <QFrame>
#include <QTabBar>
#include <QToolButton>
#include <QSignalBlocker>
#include "webviewpanel.h"
#include "wellsection/wellsectionpanel.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/edittools/undogrouprouter.h" // #748
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "services/seismictaskservice.h" // AttributeVolumePreview（attrVolumeReady 接线）

#include <qgis.h> // Qgis::MessageLevel
#include <qgsmapcanvas.h>
#include <qgsmaptool.h> // syncContext 的活动工具面（mapTool()/action()）
#include <qgsproject.h>
#include <qgsmessagelog.h>
#include <qgsmessagelogviewer.h>
#include <qgsmaplayer.h>

#include <QDockWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedLayout>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
using paleo::ui_detail::isOffscreen;
using paleo::mainwindow_internal::pageLabels;
using paleo::mainwindow_internal::pageDockTitles;
using paleo::mainwindow_internal::readRecentProjects;

// §42.1 startup page: recent-projects list + 新建/打开. Named children so the
// shell (and tests) can find them: recentProjectsList, newProjectButton,
// openProjectButton.
QWidget *makeStartupPage()
{
  auto *outer = new QWidget;
  outer->setObjectName(QStringLiteral("startupPage"));
  auto *outerLay = new QVBoxLayout(outer);
  outerLay->setContentsMargins(PaleoTheme::tokens().spacing2xl, PaleoTheme::tokens().spacing2xl, PaleoTheme::tokens().spacing2xl, PaleoTheme::tokens().spacing2xl);
  // R7 #885：内容块整体居中（上下各一份 stretch），宽屏不再左上漂 + 大片空白；块宽上限取
  // 「舒适阅读行宽」≈ 100 个平均字符（随字体/DPI 缩放，非裸 px），窄窗下照常收窄。
  auto *page = new QWidget(outer);
  page->setObjectName(QStringLiteral("startupContent"));
  page->setMaximumWidth(paleo::ui_detail::charColumns(page, 100));
  page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); // 对齐布局下仍可横向伸展到上限
  outerLay->addStretch(1);
  outerLay->addWidget(page, 0, Qt::AlignHCenter);
  outerLay->addStretch(1);
  auto *lay = new QVBoxLayout(page);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *title = new QLabel(QCoreApplication::translate("PaleoMainWindow", "Paleo Workbench"), page);
  title->setObjectName(QStringLiteral("startupHomeTitle"));
  QFont f = title->font();
  f.setPointSize(PaleoTheme::tokens().displayPt); // DESIGN.md 图件标题/页级标题 token
  f.setWeight(QFont::Medium);
  title->setFont(f);
  auto *sub = new QLabel(QCoreApplication::translate("PaleoMainWindow", "古地理编图工作台 — 新建工程或打开最近工程开始"), page);

  sub->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(sub, [] { return PaleoTheme::mutedCaptionStyleSheet(); });

  auto *recentLabel = new QLabel(QCoreApplication::translate("PaleoMainWindow", "最近工程"), page);
  recentLabel->setObjectName(QStringLiteral("recentProjectsLabel"));
  PaleoTheme::applyThemedStyleSheet(recentLabel, [] { return PaleoTheme::sectionTitleStyleSheet(); });
  auto *list = new QListWidget(page);
  list->setObjectName(QStringLiteral("recentProjectsList"));
  list->setAccessibleName(QCoreApplication::translate("PaleoMainWindow", "最近工程列表"));
  list->setIconSize(PaleoIcons::toolbarSize());
  // R7 #885：列表高度贴合条目（空态仅一行提示，不再是占满 2/3 列的大白框）；条目多时
  // 受可用高度约束并出滚动条。AdjustToContents 随 clear()/新增条目自动重算 sizeHint。
  list->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  const QStringList recent = readRecentProjects();
  for (const QString &p : recent)
  {
    const QFileInfo info(p);
    auto *item = new QListWidgetItem(PaleoIcons::qgisTheme(QStringLiteral("mActionFileOpen.svg")),
                                     info.completeBaseName() + QStringLiteral("\n") + info.absolutePath(), list);
    item->setToolTip(p);
    item->setData(Qt::UserRole, p); // activation handler reads the path from here
  }
  if (recent.isEmpty()) // §42.4 empty state — guidance, not a blank panel
  {
    auto *item = new QListWidgetItem(PaleoIcons::qgisTheme(QStringLiteral("mActionHistory.svg")),
                                     QCoreApplication::translate("PaleoMainWindow", "（暂无最近工程）"), list);
    item->setFlags(Qt::NoItemFlags);
  }

  auto *newBtn = new QPushButton(QCoreApplication::translate("PaleoMainWindow", "新建工程"), page);
  newBtn->setObjectName(QStringLiteral("newProjectButton"));
  auto *openBtn = new QPushButton(QCoreApplication::translate("PaleoMainWindow", "打开工程"), page);
  openBtn->setObjectName(QStringLiteral("openProjectButton"));
  auto *fromAreaBtn =
      new QPushButton(QCoreApplication::translate("PaleoMainWindow", "从工区文件夹新建"), page);
  fromAreaBtn->setObjectName(QStringLiteral("importFromFolderButton"));
  paleo::pagesinternal::markPrimaryButton(newBtn);
  newBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionFileNew.svg")));
  openBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionFileOpen.svg")));
  fromAreaBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconFolderOpen.svg")));
  for (auto *button : {newBtn, openBtn, fromAreaBtn})
    button->setIconSize(PaleoIcons::toolbarSize());

  auto *actionColumn = new QVBoxLayout;
  actionColumn->setSpacing(PaleoTheme::tokens().spacingSm);
  actionColumn->addWidget(newBtn);
  actionColumn->addWidget(openBtn);
  actionColumn->addWidget(fromAreaBtn);
  actionColumn->addStretch(1); // 纵向：按钮顶对齐
  auto *recentColumn = new QVBoxLayout;
  recentColumn->setSpacing(PaleoTheme::tokens().spacingSm);
  recentColumn->addWidget(recentLabel);
  recentColumn->addWidget(list);
  recentColumn->addStretch(1);
  auto *contentRow = new QHBoxLayout;
  contentRow->setSpacing(PaleoTheme::tokens().spacingXl);
  // R7 #885：操作列取按钮内容宽（stretch 0），不再被 1:2 横向拉成半屏扁条。
  contentRow->addLayout(actionColumn, 0);
  contentRow->addLayout(recentColumn, 1);

  lay->addWidget(title);
  lay->addWidget(sub);
  lay->addSpacing(PaleoTheme::tokens().spacingMd);
  lay->addLayout(contentRow);
  return outer;
}
// T31 空态标签已收敛为共享组件 ui/paleoemptystate（本文件旧匿名类删除）；
// layertreepanel 的复制版迁移登记在 docs/progress/ux.md seam 表。
} // namespace

// 中央面：startup 页（index 0）叠在工作区（画布面/数据面/地层对比）之下。
// 启动页按钮接线与 setCentralWidget 亦在此段（原行序 425-632）。
namespace
{
// 中央区被停靠面板压窄时 chip 不压到 sizeHint 以下省略成「…」（DESIGN.md），
// 改为 chip 条单独横向滚动；宽度充足时与原先一致。
QScrollArea *wrapHorizonChipsInScroll(QWidget *chips, QWidget *parent)
{
  auto *scroll = new QScrollArea(parent);
  scroll->setObjectName(QStringLiteral("horizonChipScroll"));
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(false);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  // 横向滚动条压成 spacingXs 细条：宽度充足时只预留一条细缝，不再常驻一整条
  // 滚动条高度的空白（层位行默认更紧凑）；窄屏仍可拖动滚动。
  scroll->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral(
      "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"
      "QScrollBar:horizontal { height: {spacing.xs}px; }")));
  scroll->setWidget(chips);
  scroll->setFixedHeight(chips->sizeHint().height() + PaleoTheme::tokens().spacingXs);
  return scroll;
}

// 地震层位是地层「界限」，不是地层单元（用户裁决 2026-10-30）：画布上方不再
// 默认铺开整行层位 chip。chip 条收进「界限」下拉弹层——仍是界限查看/编辑时
// 切换当前界限的入口（与定位器 Ctrl+K 同口径），层段分析一律走下方
// SubIntervalSelectorBar（父单元 + S1–S4）。弹层是按钮的子对象，
// findChild<HorizonChipBar*>() 的重建/放空接线不受影响。
QToolButton *makeBoundaryPicker(HorizonChipBar *chips, SelectionContext *selection, QWidget *parent)
{
  auto *btn = new QToolButton(parent);
  btn->setObjectName(QStringLiteral("horizonBoundaryButton"));
  btn->setAutoRaise(true);
  btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  btn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
  btn->setIconSize(PaleoIcons::toolbarSize());
  btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed); // 窄画布也完整显示当前界限名
  btn->setAccessibleName(PaleoMainWindow::tr("界限切换"));
  btn->setToolTip(PaleoMainWindow::tr("切换当前地层界限（地震层位是地层界限，不是地层单元）。"
                                      "用于界限查看/编辑；层段分析请用下方父单元与 S1–S4。"));
  auto *popup = new QFrame(btn, Qt::Popup);
  popup->setObjectName(QStringLiteral("horizonBoundaryPopup"));
  popup->setFrameShape(QFrame::StyledPanel);
  auto *lay = new QHBoxLayout(popup);
  const int pad = PaleoTheme::tokens().spacingXs;
  lay->setContentsMargins(pad, pad, pad, pad);
  lay->addWidget(wrapHorizonChipsInScroll(chips, popup));
  const auto refresh = [btn, selection] {
    const QString h = selection ? selection->activeHorizon() : QString();
    btn->setText(h.isEmpty() ? PaleoMainWindow::tr("界限") : PaleoMainWindow::tr("界限：%1").arg(h));
  };
  refresh();
  if (selection)
    QObject::connect(selection, &SelectionContext::activeHorizonChanged, btn, [refresh, popup](const QString &) {
      refresh();
      popup->hide();
    });
  // 编辑中拒切界限：收起弹层，原因由壳侧另行落状态栏（见 buildCenterArea）。
  QObject::connect(chips, &HorizonChipBar::horizonSwitchRefused, popup, [popup](const QString &) {
    popup->hide();
  });
  QObject::connect(btn, &QToolButton::clicked, btn, [btn, popup] {
    popup->adjustSize();
    popup->move(btn->mapToGlobal(QPoint(0, btn->height())));
    popup->show();
  });
  return btn;
}

void installSubIntervalSelectorBar(PaleoMainWindow *mw, SelectionContext *selection,
                                   const QStringList &boundaries, QWidget *canvasPane,
                                   QVBoxLayout *canvasLay, QWidget *boundaryPicker)
{
  auto *unitBar = new SubIntervalSelectorBar(selection, canvasPane);
  unitBar->setObjectName(QStringLiteral("subIntervalSelectorBar"));
  // 初始无工区路径时先草稿；工程打开后再 reloadFromProject。
  unitBar->reloadFromBoundaries(boundaries);
  QObject::connect(unitBar, &SubIntervalSelectorBar::readinessMessage, mw,
                   [mw](const QString &msg) {
                     if (mw->statusBar())
                       mw->statusBar()->showMessage(msg, 8000);
                   });
  QObject::connect(unitBar, &SubIntervalSelectorBar::unitSelectionCommitted, mw,
                   [mw](const paleo::strat::UnitSelection &u) {
                     if (mw->statusBar() && u.isValid())
                       mw->statusBar()->showMessage(
                           PaleoMainWindow::tr("分析层段：%1").arg(u.displayName), 4000);
                   });
  PaleoIcons::applyCommandIcons(unitBar);
  unitBar->insertLeadingWidget(boundaryPicker); // 「界限」入口并入本条，顶部只剩一行
  canvasLay->addWidget(unitBar);
}
} // namespace

void PaleoMainWindow::buildCenterArea()
{
  // ---- center: startup page stacked under the workspace ----
  m_centerStack = new PaleoViewportStack(this);
  m_centerStack->setObjectName(QStringLiteral("centerStack"));

  QWidget *startup = makeStartupPage();
  m_centerStack->addWidget(startup); // index 0

  // 工作区两面（用户裁决：数据管理是列表面，另外四页以 QGIS 画布为主）：
  //   0 画布面 = 层位 chip 条 + QgsMapCanvas（智能预测/单因素图/智能编图/验证）
  //   1 数据面 = 可视化预览；数据列表独立停靠在主窗口左侧（数据管理）
  m_workspaceStack = new PaleoViewportStack(m_centerStack);
  m_workspaceStack->setObjectName(QStringLiteral("workspaceStack"));

  auto *canvasPane = new QWidget(m_workspaceStack);
  canvasPane->setObjectName(QStringLiteral("canvasPane"));
  auto *canvasLay = new QVBoxLayout(canvasPane);
  canvasLay->setContentsMargins(0, 0, 0, 0);
  canvasLay->setSpacing(0);
  // 界限入口：ribbon 之下、画布之上，并入层段分析条最左端（不再单独一行）。
  // 界限 chip 收进「界限」下拉（见 makeBoundaryPicker）；切换 = activeHorizon + 懒加载。
  auto *chips = new HorizonChipBar(m_selection, m_layerSvc, canvasPane);
  chips->setObjectName(QStringLiteral("horizonChips"));
  chips->setAccessibleName(tr("界限切换"));
  // C2：编辑中拒切界限的原因此前无人接（信号发出去就丢了）——落状态栏。
  connect(chips, &HorizonChipBar::horizonSwitchRefused, this,
          [this](const QString &reason) {
            if (statusBar())
              statusBar()->showMessage(reason, 8000);
          });
  auto *boundaryPicker = makeBoundaryPicker(chips, m_selection, canvasPane);
  installSubIntervalSelectorBar(this, m_selection, mappingHorizons(), canvasPane, canvasLay,
                                boundaryPicker);
  if (m_canvasCtl)
  {
    QgsMapCanvas *canvas = m_canvasCtl->canvas();
    // Context belongs to the map surface, so it disappears on the data page.
    auto *contextRow = new QWidget(canvasPane);
    contextRow->setObjectName(QStringLiteral("mapInteractionContext"));
    auto *contextLayout = new QHBoxLayout(contextRow);
    contextLayout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
    contextLayout->setSpacing(PaleoTheme::tokens().spacingSm);
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
  buildVisualizationWorkspace();

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
      m_rightDock->setProgrammaticVisible(false);
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
  if (m_canvasCtl)
  {
    auto *mapEmpty = new PaleoEmptyStateLabel(
        QCoreApplication::translate("PaleoMainWindow",
                                    "地图上还没有图层 — 先在「数据管理」导入工区文件夹，或在「智能预测」运行预测"),
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
          tr("Paleo 工程 (*.paleo);;QGIS 工程 (*.qgz *.qgs)"));
      if (p.isEmpty())
        return;
      // #282：切换工程前先问当前工程脏状态（取消则原地不动）。
      if (!maybeSaveProject())
        return;
      // §38 blocking-error contract: a failed open surfaces as a dialog, not
      // a silent no-op on the startup page.
      openPath(p);
    });
  if (auto *newBtn = startup->findChild<QPushButton *>(QStringLiteral("newProjectButton")))
    connect(newBtn, &QPushButton::clicked, this, [this] {
      if (isOffscreen() || !m_projectSvc)
        return;
      runNewProject();
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
      if (!dir.isEmpty()) {
        if (ProjectSetupWorkflow::containsProject(dir)) openPath(dir);
        else runNewProject(dir);
      } // 脏状态检查在 openPath 内（#282）
    });
  if (auto *list = startup->findChild<QListWidget *>(QStringLiteral("recentProjectsList")))
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
      const QString p = item ? item->data(Qt::UserRole).toString() : QString();
      if (p.isEmpty() || !m_projectSvc)
        return;
      openPath(p); // 脏状态检查在 openPath 内（#282）
    });

  setCentralWidget(m_centerStack);
}

// 左 dock 图层平台（档案服务/属性对话框/图层树/标注 z 序）+ 地图空态随工程
// 图层集显隐（原行序 634-716；mapEmpty 经 objectName 取回——本文件建件规约）。
void PaleoMainWindow::buildLayerTreeDock()
{
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
    // 标注随图层 z 序：井位等标注默认会被 PAL 引擎画在所有层之上；
    // 该维护器给每层打 rendering/labelsWithLayer（vendored QGIS 补丁），
    // 让标注跟本层一起出图、被上层盖住。未打补丁的 QGIS 上属性为空值，无碍。
    m_labelZOrder = new QgisLabelZOrder(m_projectSvc->project(), this);
    // #138 降级方案：未打补丁的 QGIS（apt / OSGeo4W 二进制路）标注不随图层
    // z 序——启动后在状态栏如实提示一次，而不是只留 qInfo 日志。
    if (!QgisLabelZOrder::labelsWithLayerSupported()) {
      QTimer::singleShot(0, this, [this]() {
        if (statusBar())
          statusBar()->showMessage(
              tr("提示：当前 QGIS 未含「标注随图层」补丁，地图标注将始终置顶显示"),
              15000);
      });
    }
    connect(m_layerPanel, &LayerTreePanel::propertiesRequested, m_layerProps,
            &LayerPropertiesDialog::openLayerProperties);
    connect(m_layerPanel, &LayerTreePanel::attributeTableRequested, this,
            &PaleoMainWindow::showAttributeTable);
    connect(m_layerPanel, &LayerTreePanel::mappingPageRequested, this,
            &PaleoMainWindow::showPage);
    connect(m_layerPanel, &LayerTreePanel::graphRequested, this,
            [this](const QString &layer) {
      if (!m_workbench) return;
      const auto version = m_workbench->versionForLayer(layer);
      if (!version.id.isEmpty()) openDataGraph(version.assetId, version.id);
      else statusBar()->showMessage(tr("该图层尚未登记资料版本，请先导入或保存图件"), 8000);
    });
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
    layerLayout->addWidget(m_layerPanel, 1);
    m_leftDock->setWidget(layerHost);
  }
  else
  {
    m_leftDock->setWidget(new QLabel(tr("未打开工程"), m_leftDock));
  }
  addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);

  // 地图空态随工程图层集显隐（T31）；图层树空态由 LayerTreePanel 自持。
  // mapEmpty 在 buildCenterArea 建件（objectName "mapEmptyState" 唯一）。
  if (QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr)
  {
    // PaleoEmptyStateLabel 无 Q_OBJECT（Qt 6.11 findChild static_assert 拒绝）——
    // 经 QLabel 基类查（该块只用 setVisible，无派生面）。
    auto *mapEmpty = findChild<QLabel *>(QStringLiteral("mapEmptyState"));
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
}

// 右栏占位 stack + 底栏日志/任务 + 地震剖面/连井/错误历史/3D/Web dock 族
// （原行序 718-877；错误历史 dock 本体在 paleomainwindow_errorhub.cpp）。
void PaleoMainWindow::buildPageDocks()
{
  // ---- right dock: per-page panel stack (placeholders until attachWorkflows) ----
  // 标题随页（pageDockTitles()）；ribbon 里的「参数」钮就是它的 toggleViewAction。
  m_rightDock = new PaleoDockWidget(pageDockTitles().first(), this);
  m_rightDock->setObjectName(QStringLiteral("pagePanelDock"));
  auto *panelHost = new PaleoPanelHost(m_rightDock);
  panelHost->setObjectName(QStringLiteral("rightPanelHost"));
  auto *panelStack = qobject_cast<QStackedLayout *>(panelHost->layout());
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
  undoRouter()->registerScope(m_seismicSectionDock, m_seismicSectionDock->undoStack()); // #748 剖面/拾取
  addDockWidget(Qt::BottomDockWidgetArea, m_seismicSectionDock);
  tabifyDockWidget(m_bottomDock, m_seismicSectionDock);
  m_seismicSectionDock->hide();
  // goal/horizon-autotrack — 追踪层位资产登记产出 → 层清单/画布上图
  // （同 dataimport layerDeclared → declare 装配先例）
  connect(m_seismicSectionDock, &seismic::SeismicSectionDockWidget::horizonLayerDeclared,
          this, [this](const LayerDeclaration &decl) {
            QString err;
            if (!m_layerSvc->declare(decl, &err))
              qWarning() << "horizon layer declare failed:" << decl.layerId << err;
          });
  // goal/attr-volume — 时间切片属性层树条目（诚实栅格 URI）→ 上图；
  // 属性体扫描完成 → 3D 体视喂入（三槽切片 + 堆叠层体渲染）。
  connect(m_seismicSectionDock,
          &seismic::SeismicSectionDockWidget::timeSliceAttrLayerReady, this,
          [this](const LayerDeclaration &decl) {
            QString err;
            if (!m_layerSvc->declare(decl, &err))
              qWarning() << "time-slice attr layer declare failed:" << decl.layerId
                         << err;
            else if (statusBar())
              statusBar()->showMessage(
                  tr("时间切片属性已上图：%1").arg(decl.title), 8000);
          });
  connect(m_seismicSectionDock,
          &seismic::SeismicSectionDockWidget::attrVolumeReady, this,
          [this](const seismic::SeismicTaskService::AttributeVolumePreview &preview,
                 bool ok, const QString &message) {
            showAttributeVolumeIn3D(preview, ok, message);
          });

  // ---- 连井剖面 dock（地层对比图件；默认隐藏，显隐随页规则同底栏）----
  m_wellSectionDock = new PaleoDockWidget(tr("连井剖面"), this);
  m_wellSectionDock->setObjectName(QStringLiteral("wellSectionDock"));
  m_wellSectionPanel = new WellSectionPanel(m_selection, m_wellSectionDock);
  m_wellSectionPanel->setObjectName(QStringLiteral("wellSectionPanel"));
  m_wellSectionDock->setWidget(m_wellSectionPanel);
  addDockWidget(Qt::BottomDockWidgetArea, m_wellSectionDock);
  m_wellSectionDock->setUserWantsVisible(false);
  m_wellSectionDock->setProgrammaticVisible(false);

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
  webLay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // spacing.sm panel padding
  webLay->setSpacing(PaleoTheme::tokens().spacingXs);                  // spacing.xs between bar and view
  auto *addrRow = new QHBoxLayout;
  addrRow->setSpacing(PaleoTheme::tokens().spacingXs);
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
}

void PaleoMainWindow::buildVisualizationWorkspace() {
  auto *visualizations = new QWidget(m_centerStack);
  auto *visualLayout = new QVBoxLayout(visualizations);
  visualLayout->setContentsMargins(0, 0, 0, 0); visualLayout->setSpacing(0);
  m_visualizationTabs = new QTabBar(visualizations);
  m_visualizationTabs->setObjectName(QStringLiteral("visualizationTabs"));
  m_visualizationTabs->setAccessibleName(tr("可视化视图"));
  m_visualizationTabs->setExpanding(false);
  m_visualizationTabs->setTabsClosable(true);
  connect(m_visualizationTabs, &QTabBar::tabCloseRequested, this, [this](int index) {
    if (m_visualizationTabs->tabData(index).toMap().value(QStringLiteral("surface")) == QStringLiteral("compose")) return;
    m_visualizationTabs->removeTab(index);
    if (m_visualizationTabs->count() == 0) showStartup();
  });
  visualLayout->addWidget(m_visualizationTabs); visualLayout->addWidget(m_workspaceStack);
  connect(m_visualizationTabs, &QTabBar::currentChanged, this, [this](int index) {
    if (index >= 0) showPage(m_visualizationTabs->tabData(index).toMap().value(QStringLiteral("page")).toString());
  });
  m_centerStack->addWidget(visualizations);

  m_stratigraphy = new StratigraphyWorkflow(m_taskSvc, this);
  m_stratigraphyPage = new StratigraphyPage(m_stratigraphy, m_workspaceStack);
  connect(m_stratigraphy, &StratigraphyWorkflow::classificationChanged, this, [this](const QStringList &horizons) {
    if (auto *chips = findChild<HorizonChipBar *>()) chips->reloadHorizons();
    if (auto *unit = findChild<SubIntervalSelectorBar *>()) {
      const QString dir = m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath() : QString();
      unit->reloadFromProject(dir, horizons);
    }
    // horizonCombo：历史兼容刷新名单；分析身份走 SubIntervalSelectorBar → UnitSelection。
    for (auto *combo : findChildren<QComboBox *>(QStringLiteral("horizonCombo"))) {
      const auto selected = combo->currentText(); combo->clear(); combo->addItems(horizons);
      if (horizons.contains(selected)) combo->setCurrentText(selected);
    }
    if (m_selection && !horizons.isEmpty() && !horizons.contains(m_selection->activeHorizon())) {
      m_selection->setActiveHorizon(horizons.first());
      if (m_layerSvc) m_layerSvc->setActiveHorizon(horizons.first());
    }
  });
  m_workspaceStack->addWidget(m_stratigraphyPage);
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::projectOpened, this, [this](const QString &path) {
      const QString dir = QFileInfo(path).absolutePath();
      m_stratigraphy->setProject(dir, m_projectSvc->isReadOnly());
      if (auto *unit = findChild<SubIntervalSelectorBar *>())
        unit->reloadFromProject(dir, {});
    });
    connect(m_projectSvc, &QgisProjectService::projectAboutToClose, this, [this] {
      m_stratigraphy->setProject({}, false);
      if (auto *unit = findChild<SubIntervalSelectorBar *>())
        unit->reloadFromBoundaries({});
      QSignalBlocker blocker(m_visualizationTabs);
      while (m_visualizationTabs->count()) m_visualizationTabs->removeTab(0);
    });
  }

}
