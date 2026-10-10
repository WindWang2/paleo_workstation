// 层：视图
// token 例外：DESIGN 数据符号例外：QGIS 测区空间覆盖的蓝色描边/透明填充。（tools/ui-token-exceptions.json 精确计数）。
// paleomainwindow 本体（方向 83 拆分后）：壳生命周期域——构造/析构/关窗、
// buildShell 六段编排（各段实现见 paleomainwindow_docks/_status/_errorhub/
// _ribbon/_shortcuts.cpp）、主题切换、窗口状态与画布范围持久化、工程边界
// 清理（resetProjectScopedState/onProjectOpened）、C1/#282 保存流、T29 闪烁
// 定位、D11 临时配准、地震体后台装载。页切换在 _pages.cpp、工程打开/导入
// 编排在 _project.cpp。
#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"
#include "uienv_internal.h"

#include "paleodockmanager.h"

#include "paleotheme.h" // T32：焦点环/mono 数字面 token 出口；暗色翻案 token 全集
#include "paleoribbon.h" // SARibbon 壳公用件：主题/命令镜像
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "horizonchipbar.h"
#include "layout/mapbookcontroller.h" // #148：工程关闭时 resetProject
#include "layout/mapbookpanel.h"
#include "notifications/paleonotify.h"
#include "pages/pageshared.h" // kPageIds（W4 跨 TU 页序表）
#include "shortcuts/shortcutcatalog.h" // 方向63：注册表健康度进启动日志
#include "help/whatsthiscatalog.h"     // 方向63：壳控件 whatsThis 回填
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "ui/seismic3d/seismic3dviewportwidget.h" // 3D 视口 presetView/fitToBounds
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../domain/mappinghorizons.h" // baseHorizonFor（工程打开后的层位文案）
#include "../linkage/seismicmaplink.h"
#include "../linkage/selectioncontext.h" // ctor 的 activeHorizonChanged 接线
#include "../workflow/sectionworkbench.h" // m_sectionWorkbench->cancelPreviewData（resetProjectScopedState）
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgiseditingservice.h" // resolveDirtyLayerEdits 走编辑服务提交/回滚
#include "../qgis/seismicsectiontool.h" // 剖面捕获工具壳持有（R4 信号化）：析构需完整类型
#include "../services/previewdoc.h"
#include "../services/paleotaskservice.h"
#include "../services/fspathutils.h" // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
#include "../workflow/registration.h"
#include "../workflow/sectionworkbench.h" // cancelPreviewData 解引用需完整类型（#156；resetProjectScopedState 同款）
#include "domain/seismic/sgyvolume.h"
#include "qgis/projectmapreference.h" // restoreCanvasExtent 的跨 CRS 范围换算

#include <qgis.h> // Qgis::MapToolUnit / MessageLevel
#include <qgscoordinatereferencesystem.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsrectangle.h>
#include <qgssnappingutils.h>
#include <qgsproject.h>
#include <qgsmessagelog.h>
#include <qgsrubberband.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>

#include <memory>

// §42 shell: six-page workflow chain as ribbon tabs on top, layer tree left,
// page panel right, log/tasks bottom, startup page stacked under the
// workspace. The header fixes the member set, so the page table and
// cross-widget lookups live at file scope / via objectName (same discipline
// as qgiscanvascontroller.cpp).
namespace
{
using paleo::ui_detail::isOffscreen;
using paleo::mainwindow_internal::readRecentProjects;
using paleo::mainwindow_internal::writeRecentProjects;

  // SARibbonMainWindow 构造参数：顺带在基类构造前备好库（qrc + 关掉跟随
  // 系统暗色）。原生边框：Linux X11/Wayland 与 offscreen 测试同一路径，
  // SARibbon 此时自动用紧凑三行布局（页签与右侧按钮同一行）。
  SARibbonMainWindowStyles ribbonWindowStyle()
  {
    PaleoRibbon::prepareLibrary();
    return SARibbonMainWindowStyles(SARibbonMainWindowStyleFlag::UseRibbonMenuBar) |
           SARibbonMainWindowStyleFlag::UseNativeFrame;
  }
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
  // 密度同口径：构造按 QSettings 钉一次（缺省 comfort；表行高兜底扫在
  // reapplyThemeChrome，覆盖 buildShell 后建的全部表）。
  PaleoTheme::applyDensity(PaleoTheme::densityFromSettings());
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
    connect(m_projectSvc, &QgisProjectService::projectAboutToClose, this,
            [this] { resetProjectScopedState(); });
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this](const QString &) { onProjectOpened(); });
    // #282：writeProject 落盘后 setFileName 重靶权威路径，QGIS 把「文件名
    // 变更」计为未保存改动——内容刚整本写入，这里把状态拉回干净。否则标题
    // [*] 保存后常驻，且切工程/关窗会拿刚保存过的工程反复询问。
    connect(m_projectSvc, &QgisProjectService::projectWritten, this, [this] {
      if (QgsProject *p = m_projectSvc ? m_projectSvc->project() : nullptr)
        p->setDirty(false);
    });
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

  // 方向63：快捷键注册表健康度进启动日志（冲突 warning / 遮蔽 info），
  // 外壳控件按清单回填 whatsThis（页面板在 attachWorkflows 末尾再补一轮）。
  paleo::shortcuts::logConflictsOnce();
  paleo::help::applyWhatsThis(this);
}

PaleoMainWindow::~PaleoMainWindow()
{
  if (m_horizonFlashTimer)
  {
    m_horizonFlashTimer->stop();
    delete m_horizonFlashTimer.data();
  }
  if (m_horizonFlashBand)
  {
    delete m_horizonFlashBand.data();
  }
  // R4 信号化（方向 49）：剖面捕获工具壳持有，随壳析构。画布存活时先摘
  // 当前工具再拆（直接 delete 活动工具会把画布 mTool 留成悬空指针）；画布
  // 若先死，destroyed 接线已把指针置空，这里自然跳过。
  if (m_sectionCaptureTool)
  {
    if (m_canvasCtl && m_canvasCtl->canvas())
      m_canvasCtl->canvas()->unsetMapTool(m_sectionCaptureTool);
    delete m_sectionCaptureTool;
    m_sectionCaptureTool = nullptr;
  }
  // 剖面编辑/断层两库为裸指针（非 QObject 无父子回收），析构补口——
  // 换工程重绑路径有 delete，进程退出前不回收会拖住 project.sqlite 句柄。
  delete m_wellSectionStore;
  delete m_wellSectionFaultStore;
}

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

// §42 壳构建编排：六段按原文行序逐段调用（方向 83 拆 TU 前 buildShell 为
// 单函数巨石；拆分只加函数边界，语句序/连接序零变化）。
void PaleoMainWindow::buildShell()
{
  m_dockManager = new PaleoDockManager(this, QStringLiteral("ui/layout/workbench"));
  updateWindowTitle(); // 「<工程名> — Paleo Workbench [*]」（无工程时只有产品名）
  setMinimumSize(1280, 800); // §42.11 a11y floor

  // ---- center: startup page stacked under the workspace ----
  buildCenterArea();

  // ---- left dock: 图层平台（档案工具条 + 图层树面板） + 空态同步 ----
  buildLayerTreeDock();

  // ---- right placeholder / bottom log+tasks / seismic / well section /
  //      error history / 3D / web dock 族 ----
  buildPageDocks();

  // ---- ribbon 骨架（页签 / 文件菜单 / 右侧按钮组；右侧组要挂 Web dock
  //      的 toggleViewAction，所以放在 dock 建好之后）----
  buildRibbon();

  // ---- status bar 族（层位/坐标/比例尺/CRS/来源 + 错误胶囊 + 打开进度）----
  buildStatusBar();

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
  // 表行高不随 QSS 走——补扫一遍（新建的表也在此统一落档）。
  PaleoTheme::applyDensityToViewTree(this);
}

void PaleoMainWindow::setCompactDensityEnabled(bool compact)
{
  // 唯一密度写者：只有用户显式切换（面板菜单「紧凑密度」勾选）才写盘。
  PaleoTheme::writeDensityToSettings(
      compact ? PaleoTheme::Density::Compact : PaleoTheme::Density::Comfort);
  PaleoTheme::applyDensity(
      compact ? PaleoTheme::Density::Compact : PaleoTheme::Density::Comfort);
  reapplyThemeChrome();
  if (statusBar())
    statusBar()->showMessage(compact ? tr("已切换到紧凑密度") : tr("已切换到宽松密度"),
                             4000);
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

void PaleoMainWindow::flashHorizonLayer(QgsMapLayer *layer)
{
  if (!m_canvasCtl || !layer)
    return;
  QgsMapCanvas *cv = m_canvasCtl->canvas();
  if (!cv)
    return;

  // Clean up previous in-flight flash timer and rubber band if active
  if (m_horizonFlashTimer)
  {
    m_horizonFlashTimer->stop();
    delete m_horizonFlashTimer.data();
  }
  else if (auto *oldTimer = findChild<QTimer *>(QStringLiteral("horizonFlashTimer")))
  {
    oldTimer->stop();
    delete oldTimer;
  }
  if (m_horizonFlashBand)
  {
    delete m_horizonFlashBand.data();
  }
  else if (auto *oldBand = cv->findChild<QgsRubberBand *>(QStringLiteral("horizonFlashRubberBand")))
  {
    delete oldBand;
  }

  // 闪烁定位（T29 spec ~300–500ms）：#1B73D0 半透明多边形橡皮带盖住图层
  // 范围，100ms 一闪 ×4 后自毁。交互蓝只做交互反馈，不做常驻装饰
  //（DESIGN.md：交互色不兼装饰）。
  auto *band = new QgsRubberBand(cv, Qgis::GeometryType::Polygon);
  band->setParent(cv);
  band->setObjectName(QStringLiteral("horizonFlashRubberBand"));
  m_horizonFlashBand = band;
  band->setToGeometry(QgsGeometry::fromRect(layer->extent()),
                      layer->crs());
  band->setColor(QColor(27, 115, 208, 60)); // #1B73D0 @ ~24% 填充透明度
  band->setStrokeColor(QColor(QStringLiteral("#1B73D0")));
  band->setWidth(2);
  setProperty("horizonFlashActive", true);
  auto *timer = new QTimer(this);
  timer->setObjectName(QStringLiteral("horizonFlashTimer"));
  m_horizonFlashTimer = timer;
  int blinks = 4;
  QPointer<QgsRubberBand> safeBand(band);
  QPointer<QTimer> safeTimer(timer);
  connect(timer, &QTimer::timeout, this, [this, safeTimer, safeBand, blinks]() mutable {
    if (!safeBand)
    {
      if (safeTimer)
      {
        safeTimer->stop();
        safeTimer->deleteLater();
      }
      setProperty("horizonFlashActive", false);
      return;
    }
    safeBand->setVisible(!safeBand->isVisible());
    if (--blinks <= 0)
    {
      if (safeTimer)
      {
        safeTimer->stop();
        safeTimer->deleteLater();
      }
      delete safeBand.data(); // 画布条目直接删——不在信号发送者栈上
      setProperty("horizonFlashActive", false);
    }
  });
  timer->start(100);
}

void PaleoMainWindow::showStartup()
{
  if (m_currentPage == QLatin1String("correlation"))
    restoreCorrelationDocks();
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
  // setProject 必须补：nativeSnappingConfig 是无 project 的静态构造，缺它
  // 则重开工程时 QgsSnappingConfig::readProject 在空 project 上调 mapLayer
  // 崩溃（QGIS 4.2 setSnappingConfig 不代挂 project 指针）。
  if (m_projectSvc && m_projectSvc->project())
  {
    auto config = m_projectSvc->project()->snappingConfig();
    if (!config.enabled() || config.units() != Qgis::MapToolUnit::Pixels)
      config = QgisCanvasController::nativeSnappingConfig();
    config.setProject(m_projectSvc->project());
    m_projectSvc->project()->setSnappingConfig(config);
    if (m_canvasCtl)
      m_canvasCtl->canvas()->snappingUtils()->setConfig(config);
    if (auto *toolbar = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
      toolbar->snapToleranceSpin()->setValue(qRound(config.tolerance()));
  }

  restoreCanvasExtent(); // per-project display state from the .qgz

  // #156：测井对比井集来自 catalog（不再只由本会话的导入事件填充）。
  refreshCorrelationWells(QString(), /*loadAllLas=*/true);

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
  // C1 关窗数据保护 + #282（UIS-09）工程级脏状态：maybeSaveProject 先查
  // 编辑缓冲（resolveDirtyLayerEdits）再查 QgsProject::isDirty（版面/样式/
  // 图层树/主题只存 .qgz）——保存/放弃/取消三选一，取消（含 Esc/窗口 ✕）
  // 不关窗。offscreen 且未注入 seam 时直接放行（无头环境不弹模态）。
  if (!maybeSaveProject())
  {
    event->ignore();
    return;
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
    const QString saveKey = paleo::shortcuts::displayKey(
        paleo::shortcuts::keyFor(QStringLiteral("main.project.save")));
    const QString enabledTip =
        saveKey.isEmpty() ? tr("保存工程") : tr("保存工程（%1）").arg(saveKey);
    saveAct->setToolTip(readOnly ? tr("工程处于只读模式（另一个实例持有写锁）") : enabledTip);
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

void PaleoMainWindow::saveWindowState()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("windowGeometry"), saveGeometry());
  s.setValue(QStringLiteral("windowState"),
             m_currentPage == QLatin1String("correlation") && !m_beforeCorrelationWindowState.isEmpty()
                 ? m_beforeCorrelationWindowState : saveState());
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
                              .arg(e.xMinimum(), 0, 'g', 17).arg(e.yMinimum(), 0, 'g', 17)
                              .arg(e.xMaximum(), 0, 'g', 17).arg(e.yMaximum(), 0, 'g', 17);
  m_projectSvc->project()->writeEntry(QStringLiteral("paleo"),
                                      QStringLiteral("canvasExtent"), encoded);
  m_projectSvc->project()->writeEntry("paleo", "canvasExtentCrs", cv->mapSettings().destinationCrs().toWkt());
}

void PaleoMainWindow::restoreCanvasExtent()
{
  if (!m_canvasCtl || !m_projectSvc || !m_projectSvc->project())
    return;
  bool ok = false;
  const QString raw = m_projectSvc->project()->readEntry(
      QStringLiteral("paleo"), QStringLiteral("canvasExtent"), QString(), &ok);
  if (!ok) {
    QTimer::singleShot(0, this, [this] { m_canvasCtl->zoomToFullExtent(); });
    return;
  }
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
  QgsRectangle e(xmin, ymin, xmax, ymax);
  if (e.isEmpty())
    return;
  const auto savedCrs = m_projectSvc->project()->readEntry("paleo", "canvasExtentCrs");
  const auto source = QgsCoordinateReferenceSystem::fromWkt(savedCrs.isEmpty() ? DataCatalog::localGridCrsWkt() : savedCrs);
  e = paleo::mapreference::mapExtent(m_canvasCtl->canvas(), e, source);
  if (e.isEmpty()) return;
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

void PaleoMainWindow::resetProjectScopedState()
{
  // 属性建模：作业取消（任务已随任务会话取消，这里同步收 UI 忙态的入口；
  // jobCompleted 到达时 finishPropertyModelRun 按「已取消」收尾）。
  m_propModelCancel = true;
  m_propModelRunner.requestCancel();
  if (m_factorTask && m_factorTask->running())
    m_factorTask->requestCancel();

  // #154：预览标签全部关闭 + PreviewDoc 按 assetId 的会话缓存清空。
  if (m_previewTabs)
    m_previewTabs->closeAllTabs();
  if (m_previewDoc)
    m_previewDoc->resetProjectState();

  // #156：测井对比井集/曲线/在途 LAS 清空；新工程打开后从 catalog 重灌。
  if (m_corrPanel)
    m_corrPanel->resetProject();

  ++m_seismicOpenGeneration;
  if (m_sectionWorkbench) m_sectionWorkbench->cancelPreviewData();
  if (m_seismicOpenTask)
    m_seismicOpenTask->requestCancel();
  m_seismicOpenPath.clear();
  if (auto *status = findChild<QLabel *>(QStringLiteral("projectSeismicLoadStatus")))
    status->hide();
  if (auto *progress = findChild<QProgressBar *>(QStringLiteral("projectSeismicLoadProgress")))
    progress->hide();
  // #158：3D 视图与剖面 dock 的地震体清空；新工程若有地震，打开后由
  // syncSeismicVolumeToDocks 重新装载。
  if (m_seismic3dPanel)
    m_seismic3dPanel->setVolume(nullptr);
  if (m_sectionLink)
    m_sectionLink->setActiveVolume(nullptr);
  else if (m_seismicSectionDock)
    m_seismicSectionDock->setVolume(nullptr);

  // #236：剖面解释态跨工程残留清理——会话/可登记属性结果/在途 SATV+预览/
  // 书签下拉清空（setVolume 只换体不清解释会话，链路在场时 dock 甚至收不到
  // setVolume(nullptr)）；不确定性图签（集合均值 · N 成员口径签）同理——
  // 只随集合面在画布期间存在，工程边界即关。
  if (m_seismicSectionDock)
    m_seismicSectionDock->resetInterpretationState();
  if (m_decorMgr)
    m_decorMgr->clearUncertaintyBadge();
  // #226：登记上下文随工程边界清空（新工程由 syncSeismicVolumeToDocks 重注；
  // 旧工程 catalog 指针下不得残留「可登记」上下文）。
  if (m_seismicSectionDock)
    m_seismicSectionDock->setInterpretationCatalog(nullptr, QString(), QString(),
                                                  QString());

  // #148：地图册在途一册取消（下一版边界停，不再碰旧工程图层），迟到结果
  // 作废；范围清空，新工程下次打开面板时按画布范围重新预填。
  if (m_mapBookCtl)
    m_mapBookCtl->resetProject();
  if (m_mapBookPanel)
  {
    m_mapBookPanel->setArea(PaleoMapBook::Area());
    m_mapBookPanel->setOutputDir(QString());
  }

  // 临时配准水印跨工程残留：旧工程的临时层不随新工程存在——计数清零、
  // 水印关闭（RegistrationWorkflow 的计数只增不减且不接工程边界信号，
  // 窗口侧收口；否则新工程被永久误标「非权威坐标」）。
  m_provisionalLayers = 0;
  if (m_decorMgr)
    m_decorMgr->setWatermarkEnabled(false);
}

// C1/#282：编辑中且有未提交改动的矢量图层 → 保存/放弃/取消三选一。走编辑
// 服务（busy 挂账随 commit/rollback 清），无服务时直连 commitChanges/
// rollBack。offscreen（无头测试/渲染环境）且未注入询问 seam 时不弹模态框
// ——弹了没人点会挂死事件循环，保持旧行为直接放行。返回 false = 取消。
bool PaleoMainWindow::resolveDirtyLayerEdits()
{
  if (!m_projectSvc || !m_projectSvc->project())
    return true;
  if (isOffscreen() && !m_projectSaveAsk)
    return true;
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
  if (dirty.isEmpty())
    return true;
  const QString text = tr("以下图层有未保存的编辑：\n%1\n\n继续之前如何处理？")
                           .arg(dirtyNames.join(QLatin1Char('\n')));
  const auto choice =
      m_projectSaveAsk
          ? m_projectSaveAsk(tr("未保存的编辑"), text)
          : PaleoNotify::askSaveDiscard(this, PaleoNotify::AskIcon::Warning,
                                        tr("未保存的编辑"), text, tr("保存"),
                                        tr("放弃"), tr("取消"));
  if (choice == PaleoNotify::SaveChoice::Cancel)
    return false;
  if (choice == PaleoNotify::SaveChoice::Save)
  {
    for (QgsVectorLayer *vl : dirty)
    {
      QString err;
      const bool ok =
          m_editSvc ? m_editSvc->commitEdit(vl, &err) : vl->commitChanges();
      if (!ok)
      {
        PaleoNotify::critical(
            this, tr("保存编辑失败"),
            tr("图层「%1」的编辑未能提交，操作中止。")
                .arg(vl->name().isEmpty() ? vl->id() : vl->name()));
        return false;
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
  return true;
}

// #282（UIS-09）：关窗/切换工程前的工程级脏状态检查。版面、图层样式、
// 图层树顺序、地图主题只活在 .qgz 里——此前 closeEvent 只查编辑缓冲、切换
// 路径（开新工程/新建/最近列表/openPath）根本不问，改动随 QgsProject::
// clear() 静默丢失（标题栏 [*] 曾是唯一线索）。收口成三选一：保存
//（writeProject 原子落盘）/ 放弃（照常关闭或切换）/ 取消（原地不动）。
// 返回 false = 取消。offscreen 且无 seam 时直接放行（无头自动化旧行为）。
bool PaleoMainWindow::maybeSaveProject()
{
  // 没有打开工程文件时，QgsProject 仍可能被标脏（空工程、标题 [*]）。
  // 关窗和换工程直接放行，不弹保存/放弃。
  if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
    return true;
  if (!resolveDirtyLayerEdits())
    return false;
  QgsProject *proj = m_projectSvc->project();
  if (!proj || !proj->isDirty())
    return true; // 不脏：直接放行
  if (isOffscreen() && !m_projectSaveAsk)
    return true;
  const QString text =
      tr("当前工程有未保存的改动（版面、图层样式、图层树顺序、地图主题等"
         "保存在工程文件里）。\n\n继续之前是否保存？");
  const auto choice =
      m_projectSaveAsk
          ? m_projectSaveAsk(tr("保存工程改动？"), text)
          : PaleoNotify::askSaveDiscard(this, PaleoNotify::AskIcon::Warning,
                                        tr("保存工程改动？"), text,
                                        tr("保存"), tr("放弃"), tr("取消"));
  if (choice == PaleoNotify::SaveChoice::Cancel)
    return false;
  if (choice == PaleoNotify::SaveChoice::Save && !m_projectSvc->writeProject())
  {
    const QString why = m_projectSvc->lastErrors().join(QLatin1Char('\n'));
    PaleoNotify::critical(this, tr("保存工程失败"),
                          tr("工程文件未能写入：%1\n当前工程保持不变。").arg(why));
    return false;
  }
  // 放弃：不写盘——随后的关闭/切换按既有语义丢弃（QgsProject::clear()）。
  return true;
}

void PaleoMainWindow::refreshCorrelationWells(const QString &loadLasForAssetId,
                                              bool loadAllLas)
{
  if (!m_corrPanel || !m_previewDoc)
    return;
  QList<QPair<QString, QString>> wells;
  const QStringList ids = m_previewDoc->assetIds(QStringLiteral("well_log"));
  for (const QString &id : ids)
    wells.append({id, m_previewDoc->assetSource(id)});
  m_corrPanel->setWells(wells);
  const QString root = m_projectSvc && !m_projectSvc->projectPath().isEmpty()
                           ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                           : QString();
  // LAS 资产把 GR 曲线拉进井列（任务服务在场时是 quiet 异步解析）。
  for (const QString &id : ids)
  {
    if (!loadAllLas && id != loadLasForAssetId)
      continue;
    const QString src = m_previewDoc->assetSource(id);
    if (src.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
      m_corrPanel->loadWellLas(id, QDir(root).absoluteFilePath(src), QStringLiteral("GR"));
  }
}

void PaleoMainWindow::syncSeismicVolumeToDocks()
{
  if (m_projectSvc && m_projectSvc->isOpening()) return;
  DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
  if (!cat || !cat->isOpen() || !m_seismicTaskSvc)
    return;
  for (const CatalogAsset &asset : cat->assets())
  {
    if (asset.type != QLatin1String("seismic"))
      continue;
    const auto version = cat->currentVersion(asset.id);
    // #226：解释登记链生产接线——dock 注入 catalog + 源体资产/版本 +
    // 解释产物目录（工程受管 artifacts/derived/interpretation：拾取 CSV/
    // 断层/属性扫描产物全部落此并登记 DERIVED，对 catalog/治理/版本溯源
    // 可见）。调用时现取工程目录（attach 期没有工程——同 #275 口径）。
    if (m_seismicSectionDock && !version.id.isEmpty())
    {
      const QString projDir =
          m_projectSvc && !m_projectSvc->projectPath().isEmpty()
              ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
              : QString();
      m_seismicSectionDock->setInterpretationCatalog(
          cat, asset.id, version.id,
          projDir.isEmpty()
              ? QString()
              : QDir(projDir).filePath(
                    QStringLiteral("artifacts/derived/interpretation")));
    }
    const QString path = m_previewDoc->absolutePathForVersion(version);
    if (path.isEmpty() || !QFile::exists(path))
      continue;
    const auto matches = [&path](const auto &volume) {
      return volume && paleo::fromFsPath(volume->Path()) == path;
    };
    const bool need3d = m_seismic3dPanel && !matches(m_seismic3dPanel->volume());
    const bool needSection = m_seismicSectionDock && !matches(m_seismicSectionDock->volume());
    if ((!need3d && !needSection) || (m_seismicOpenPath == path && m_seismicOpenTask && m_seismicOpenTask->running()))
      return;
    if (m_seismicOpenTask)
      m_seismicOpenTask->requestCancel();
    const quint64 generation = ++m_seismicOpenGeneration;
    m_seismicOpenPath = path;
    const quint64 session = m_projectSvc ? m_projectSvc->sessionId() : 0;
    double origin = 0;
    for (const auto &link : cat->linksForAsset(asset.id))
      if (!link.unresolved && link.entityType == QLatin1String("seismic_survey")) {
        origin = cat->entityById(link.entityId).startTimeMs;
        break;
      }
    auto *status = findChild<QLabel *>(QStringLiteral("projectSeismicLoadStatus"));
    auto *progress = findChild<QProgressBar *>(QStringLiteral("projectSeismicLoadProgress"));
    if (!status) {
      status = new QLabel(this);
      status->setObjectName(QStringLiteral("projectSeismicLoadStatus"));
      progress = new QProgressBar(this);
      progress->setObjectName(QStringLiteral("projectSeismicLoadProgress"));
      progress->setMaximumWidth(160);
      statusBar()->addPermanentWidget(status);
      statusBar()->addPermanentWidget(progress);
    }
    status->setText(tr("后台加载地震体：%1").arg(QFileInfo(path).fileName()));
    progress->setRange(0, 0);
    status->show(); progress->show();
    QPointer<PaleoMainWindow> guard(this);
    m_seismicOpenTask = m_seismicTaskSvc->startVolumeLoad(path,
        [guard, generation, session, origin, status, progress](bool ok,
              std::shared_ptr<seismic::SgyVolume> volume, const QString &error) {
      if (!guard || generation != guard->m_seismicOpenGeneration ||
          (guard->m_projectSvc && session != guard->m_projectSvc->sessionId()))
        return;
      status->hide(); progress->hide();
      guard->m_seismicOpenPath.clear();
      if (!ok || !volume) {
        guard->statusBar()->showMessage(error, 10000);
        return;
      }
      if (guard->m_seismic3dPanel) {
        guard->m_seismic3dPanel->setTaskService(guard->m_seismicTaskSvc.get());
        guard->m_seismic3dPanel->setVolume(volume);
        if (guard->m_seismic3dPanel->viewport()) {
          guard->m_seismic3dPanel->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
          guard->m_seismic3dPanel->viewport()->fitToBounds();
        }
      }
      if (guard->m_seismicSectionDock) {
        guard->m_seismicSectionDock->setTimeOriginMs(origin);
        if (guard->m_sectionLink)
          guard->m_sectionLink->setActiveVolume(volume);
        else
          guard->m_seismicSectionDock->setVolume(volume);
      }
      guard->statusBar()->showMessage(QObject::tr("地震体加载完成"), 5000);
    }, /*quiet=*/true); // 状态栏已有进度；自动加载不反复展开/收起底栏。
    if (m_seismicOpenTask) {
      QPointer<PaleoTask> task = m_seismicOpenTask;
      connect(task, &PaleoTask::changed, this, [task, progress, generation, this] {
        if (!task || generation != m_seismicOpenGeneration) return;
        const int percent = task->percent();
        progress->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) progress->setValue(percent);
      });
    }
    return;
  }
}
