#include <QtTest>
#include <QTemporaryDir>
#include <QLabel>
#include <QStatusBar>
#include <QScrollArea>
#include <QComboBox>
#include <QLineEdit>
#include <QTabBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStackedLayout>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QSignalSpy>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include "../src/ui/pages/constraintpage.h"
#include "../src/io/constraintstore.h"
#include "../src/workflow/workflows.h"

#include "../src/app/appcontext.h"
#include "../src/domain/faciescatalog.h"
#include "../src/io/dataimportservice.h" // 测试可直触 io（断言 DataImportService 信号）
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/correlationpanel.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/catalog/datacatalog.h"
#include "../src/ui/dialogs/folderconfirm.h"
#include "../src/ui/faults/faultmanagerpanel.h"
#include "../src/ui/layoutdesignershell.h"
#include "../src/ui/seismicsection/sectionsetupdialog.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"
#include "../src/ui/propertymodel/propertymodelpanel.h"
#include "../src/ui/wellcomposite/curveconfigdialog.h"
#include "../src/ui/decorations/paleodecorations.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/pages/datapage.h"
#include "../src/ui/pages/wellpredictionpanel.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/paleoribbon.h"
#include "../src/ui/paleodockmanager.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/mappingworkbench.h"
#include <QDialog>
#include <QMenu>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <qgsmapmouseevent.h>

#include <qgsproject.h>
#include <qgsprintlayout.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
#include <qgssnappingutils.h>
#include <cmath>
#include <QGraphicsItem>

#include <qgslayertreeview.h>
#include <qgslayertree.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgslayertreemodel.h>
#include <qgsmaptoolpan.h>

// P3 D9 分栏契约测试的前置上下文（类外声明——moc 不解析槽区内的嵌套结构体）。
struct DataOpsWidthCtx
{
    QDockWidget *dock = nullptr;
    int width = 0;
};

// App-shell acceptance (§42): the five-page workflow chrome over the P0 spine.
// AppContext owns the QgsApplication — main() builds the context (which brings
// the QApplication up), never a QApplication of its own.
class TestUiShell : public QObject
{
  Q_OBJECT
  public:
    explicit TestUiShell(AppContext *ctx, QObject *parent = nullptr)
      : QObject(parent), m_ctx(ctx) {}

  private:
    AppContext *m_ctx;
    PaleoMainWindow *m_win = nullptr;

  private slots:
    void initTestCase()
    {
      QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
      // QSettings persists across runs in its temp path — start every run
      // with a clean slate (lastPage must not leak into startupThenCanvas).
      QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
      QVERIFY(m_ctx->canvasCtl() && m_ctx->projectSvc() && m_ctx->layerSvc() &&
              m_ctx->toolSvc() && m_ctx->selection() && m_ctx->store() &&
              m_ctx->manifest() && m_ctx->processingSvc() && m_ctx->editingSvc() &&
              m_ctx->styleSvc());
      m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(),
                                  m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
    }

    void cleanupTestCase()
    {
      delete m_win;
      m_win = nullptr;
    }

    // §42.11 a11y floor
    void windowMeetsMinSize()
    {
      QVERIFY(m_win->minimumWidth() >= 1280);
      QVERIFY(m_win->minimumHeight() >= 800);
    }

    // goal/ui-experience-polish：W5 快捷键族——Ctrl+1..5 直切、Ctrl+Tab/
    // Ctrl+Shift+Tab 循环（含末→首环绕）、密度切换菜单动作在位。
    void pageShortcutsTabCycleAndDensityToggle()
    {
      m_win->show();
      QTest::qWait(10);
      // 起点不限（首启是 startup 页）；Ctrl+3 → 单因素图。
      QTest::keyClick(m_win, Qt::Key_3, Qt::ControlModifier);
      QCOMPARE(m_win->currentPage(), QStringLiteral("constraint"));
      // Ctrl+Tab → 智能编图
      QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier);
      QCOMPARE(m_win->currentPage(), QStringLiteral("compose"));
      // Ctrl+Tab → 验证；再进一格环绕回数据管理
      QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier);
      QCOMPARE(m_win->currentPage(), QStringLiteral("validate"));
      QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier);
      QCOMPARE(m_win->currentPage(), QStringLiteral("data"));
      // Ctrl+Shift+Tab 从首页环绕到末页
      QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier);
      QCOMPARE(m_win->currentPage(), QStringLiteral("validate"));
      // 密度切换（菜单 action objectName 契约 + 行为等价的公共面）：
      // applyDensity 切档 → QSS padding 档位即时跟随；settings 往返守恒。
      PaleoTheme::applyDensity(PaleoTheme::Density::Compact);
      QCOMPARE(PaleoTheme::currentDensity(), PaleoTheme::Density::Compact);
      QVERIFY(PaleoTheme::itemViewStyleSheet().contains(QStringLiteral("padding: 1px")));
      PaleoTheme::writeDensityToSettings(PaleoTheme::Density::Compact);
      QCOMPARE(PaleoTheme::densityFromSettings(), PaleoTheme::Density::Compact);
      PaleoTheme::applyDensity(PaleoTheme::Density::Comfort);
      PaleoTheme::writeDensityToSettings(PaleoTheme::Density::Comfort);
      QCOMPARE(PaleoTheme::densityFromSettings(), PaleoTheme::Density::Comfort);
      QVERIFY(PaleoTheme::itemViewStyleSheet().contains(QStringLiteral("padding: 3px")));
      // Ctrl+K 归定位器独占（数据页命令面板改键的断言在 tst_panels
      // uipolish_commandPaletteShortcutMoved——裸壳不构建 DataPage）。
    }

    // T32：全局焦点环进主窗样式表（2px #1B73D0）；工作流标签溢出走滚动
    // 按钮；状态栏坐标/比例尺是 JetBrains Mono 9pt 数字面。
    void focusRingScrollButtonsAndStatusMono()
    {
      QVERIFY2(m_win->styleSheet().contains(QStringLiteral("2px solid #1B73D0")),
               "window stylesheet must carry the DESIGN.md focus ring");
      QVERIFY(m_win->styleSheet().contains(QStringLiteral("QLineEdit:focus")));
      QVERIFY(m_win->styleSheet().contains(QStringLiteral("QTableView:focus")));

      auto *tabs = m_win->findChild<QTabBar *>(QStringLiteral("workflowTabs"));
      QVERIFY(tabs && tabs->usesScrollButtons());

      auto *coords = m_win->findChild<QLabel *>(QStringLiteral("statusCoords"));
      auto *scale = m_win->findChild<QLabel *>(QStringLiteral("statusScale"));
      QVERIFY(coords && scale);
      QVERIFY(coords->font().families().contains(QStringLiteral("JetBrains Mono")));
      QVERIFY(scale->font().families().contains(QStringLiteral("JetBrains Mono")));
      QCOMPARE(coords->font().pointSize(), 9);
    }

    void ribbonThemePreservesBaseStyles()
    {
      QCoreApplication::processEvents(); // Finish SARibbon's queued startup theme.
      const auto apply = [this] {
        PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() +
                                      PaleoTheme::focusRingStyleSheet());
      };
      PaleoTheme::applyLightTheme();
      apply();
      const QString light = m_win->styleSheet();
      QVERIFY(light.contains(QStringLiteral("SARibbonSeparatorWidget")));
      apply();
      QCOMPARE(m_win->styleSheet(), light);
      PaleoTheme::applyDarkTheme();
      apply();
      QVERIFY(m_win->styleSheet() != light);
      PaleoTheme::applyLightTheme();
      apply();
      QCOMPARE(m_win->styleSheet(), light);
    }

    // §42 workflow chain: 数据管理/预测编图/单因素图/智能编图/验证
    void workflowTabBarHasFiveTabs()
    {
      auto *tabs = m_win->findChild<QTabBar *>(QStringLiteral("workflowTabs"));
      QVERIFY(tabs);
      QCOMPARE(tabs->count(), 5);
      QCOMPARE(tabs->tabText(0), QStringLiteral("数据管理"));
      QCOMPARE(tabs->tabText(1), QStringLiteral("预测编图"));
      QCOMPARE(tabs->tabText(2), QStringLiteral("单因素图"));
      QCOMPARE(tabs->tabText(3), QStringLiteral("智能编图"));
      QCOMPARE(tabs->tabText(4), QStringLiteral("验证"));
      QVERIFY(m_win->categoryForPage(QStringLiteral("data")));
      QVERIFY(m_win->categoryForPage(QStringLiteral("predict")));
      QVERIFY(m_win->categoryForPage(QStringLiteral("constraint")));
      QVERIFY(m_win->categoryForPage(QStringLiteral("compose")));
      QVERIFY(m_win->categoryForPage(QStringLiteral("validate")));

      // shell anatomy: left tree / right panel / bottom tabs all exist
      QVERIFY(m_win->findChild<QWidget *>(QStringLiteral("rightPanelHost")));
      QVERIFY(m_win->findChild<QTabWidget *>(QStringLiteral("bottomTabs")));
      QVERIFY(m_win->findChild<QPushButton *>(QStringLiteral("newProjectButton")));
      QVERIFY(m_win->findChild<QPushButton *>(QStringLiteral("openProjectButton")));
    }

    void showPageTracksCurrentPage()
    {
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(m_win->currentPage(), QStringLiteral("data"));

      m_win->showPage(QStringLiteral("validate"));
      QCOMPARE(m_win->currentPage(), QStringLiteral("validate"));

      auto *tabs = m_win->findChild<QTabBar *>(QStringLiteral("workflowTabs"));
      QCOMPARE(tabs->currentIndex(), 4);

      // right panel stack follows the tab
      auto *host = m_win->findChild<QWidget *>(QStringLiteral("rightPanelHost"));
      auto *panelStack = static_cast<QStackedLayout *>(host->layout());
      QCOMPARE(panelStack->currentIndex(), 4);

      // unknown ids are rejected without disturbing current state
      m_win->showPage(QStringLiteral("bogus"));
      QCOMPARE(m_win->currentPage(), QStringLiteral("validate"));
      QCOMPARE(tabs->currentIndex(), 4);
    }

    // §42.1: startup page shows until a project is opened, then the canvas
    void startupThenCanvas()
    {
      m_win->showStartup();
      QCOMPARE(m_win->currentPage(), QStringLiteral("startup"));
      auto *stack = m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack"));
      QVERIFY(stack);
      QCOMPARE(stack->currentIndex(), 0); // startup page

      m_win->onProjectOpened();
      QCOMPARE(stack->currentIndex(), 1); // canvas page
      QCOMPARE(m_win->currentPage(), QStringLiteral("data")); // lands on first step
    }

    void projectOpenWiresLayerTree()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString path = dir.filePath(QStringLiteral("proj.qgz"));

      QVERIFY2(m_ctx->projectSvc()->createProject(path),
               qPrintable(m_ctx->projectSvc()->lastErrors().join(';')));
      m_win->onProjectOpened(); // idempotent — signal already fired it once

      auto *view = m_win->findChild<QgsLayerTreeView *>(QStringLiteral("layerTreeView"));
      QVERIFY(view);
      QVERIFY(view->model() != nullptr);          // the view-visible proxy model
      QVERIFY(view->layerTreeModel() != nullptr); // the source QgsLayerTreeModel
      QCOMPARE(m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack"))->currentIndex(), 1);

      // AppContext manifest convention: "<qgz>.project.sqlite" opened on open
      QVERIFY(QFile::exists(path + QStringLiteral(".project.sqlite")));

      // recent-projects list picked the opened project up
      auto *recent = m_win->findChild<QListWidget *>(QStringLiteral("recentProjectsList"));
      QVERIFY(recent);
      QVERIFY(recent->count() >= 1);
      QCOMPARE(recent->item(0)->data(Qt::UserRole).toString(), path);
    }

    void readOnlyProjectTogglesUiAndTitle()
    {
      m_win->setProjectReadOnly(true);
      QVERIFY(m_win->windowTitle().contains(QStringLiteral("[只读]")));
      auto *saveAct = m_win->findChild<QAction *>(QStringLiteral("saveProjectAction"));
      if (saveAct)
        QVERIFY(!saveAct->isEnabled());
      auto *editTb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
      if (editTb)
        QVERIFY(!editTb->isEnabled());

      m_win->setProjectReadOnly(false);
      QVERIFY(!m_win->windowTitle().contains(QStringLiteral("[只读]")));
      if (saveAct)
        QVERIFY(saveAct->isEnabled());
      if (editTb)
        QVERIFY(editTb->isEnabled());
    }

    // Processing entry point: attachWorkflows with the processing service adds
    // a top-bar "处理算法" button whose menu surfaces the paleo:* algorithms
    // (full-registry ids live in per-provider submenus).
    void processingButtonSurfacesAlgorithms()
    {
      // 套件内第一次 attach：attachWorkflows 幂等（二次调用早退），这里必须
      // 带齐 editing/layout/task 服务，否则依赖 taskSvc 的地图册 dock/入口
      // （#148）永远装不上，后续用例的 mapBookButton/mapBookDock 断言必红。
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());

      auto *btn = m_win->findChild<QToolButton *>(QStringLiteral("processingButton"));
      QVERIFY(btn);
      QVERIFY(btn->menu());

      // Top-bar companion controls wired by the same call.
      QVERIFY(m_win->findChild<QWidget *>(QStringLiteral("paleoLocator")));
      QVERIFY(m_win->findChild<QToolButton *>(QStringLiteral("saveButton")));
      QVERIFY(m_win->findChild<QWidget *>(QStringLiteral("releasePanel")));
      // T32 a11y：发布面板与发布列表报名。
      auto *release = m_win->findChild<QWidget *>(QStringLiteral("releasePanel"));
      QVERIFY(!release->accessibleName().isEmpty());
      auto *releaseList = m_win->findChild<QWidget *>(QStringLiteral("releaseList"));
      QVERIFY(releaseList && !releaseList->accessibleName().isEmpty());

      QStringList flat, tips;
      const auto walk = [&flat, &tips](QMenu *menu, auto &&self) -> void {
        for (QAction *a : menu->actions())
        {
          if (a->menu())
            self(a->menu(), self);
          else
          {
            flat << a->text();
            tips << a->toolTip();
          }
        }
      };
      walk(btn->menu(), walk);

      // W6：菜单项显示 displayName（人读名），机器 id 退到 tooltip。
      QVERIFY(flat.contains(QStringLiteral("Paleo: Constraint IDW")));
      QVERIFY(flat.contains(QStringLiteral("Paleo: Facies Fusion")));
      QVERIFY(flat.contains(QStringLiteral("Paleo: Geological Smoothing")));
      QVERIFY(!flat.contains(QStringLiteral("paleo:paleo_constraint_idw")));
      QVERIFY(tips.contains(QStringLiteral("paleo:paleo_constraint_idw")));

      // Provider submenus exist for non-paleo algorithms (registry-dependent:
      // only assert when the registry actually exposes others).
      const QStringList all = m_ctx->processingSvc()->algorithmIds();
      QStringList nonPaleo;
      for (const QString &id : all)
        if (!id.startsWith(QStringLiteral("paleo:")))
          nonPaleo << id;
      if (!nonPaleo.isEmpty())
      {
        bool hasSubmenu = false;
        for (QAction *a : btn->menu()->actions())
          hasSubmenu |= (a->menu() != nullptr);
        QVERIFY(hasSubmenu);
      }
    }

    // ribbon 图标整理：编辑条全部动作挂 vendor QGIS 主题图标且
    // icon-over-text（DESIGN.md ribbon-button）；顶部动作钮 icon-beside-
    // text；chips 独占 ribbon 之下一行（horizonChips 的父链落在
    // ribbonActionRow，不再与工作流标签挤同一行）。
    // 注：attachWorkflows 非幂等——重复调用重复建 dock/按钮，且二次调用后
    // 后续用例会段错误（实测验证）。因此本用例沿用「attachWorkflows 已在
    // 前面的用例跑过」惯例：editingToolbar 缺席才补调（单跑路径），在套件
    // 内复用既有接线。designerButton 需要 layoutSvc 参数，套件内前面的
    // attachWorkflows 没传它 → 存在则断言、缺席不硬要。
    void ribbonButtonsCarryIcons()
    {
      if (!m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                               m_ctx->compositionWf(), m_ctx->validationWf(),
                               m_ctx->importSvc(), m_ctx->seismicLink(),
                               m_ctx->processingSvc(), m_ctx->store(),
                               m_ctx->editingSvc(), m_ctx->layoutSvc(),
                               m_ctx->taskSvc());

      auto *editTb = m_win->findChild<PaleoEditingToolbar *>(
          QStringLiteral("editingToolbar"));
      QVERIFY(editTb);
      QCOMPARE(editTb->toolBar()->toolButtonStyle(), Qt::ToolButtonTextUnderIcon);
      const QList<QAction *> editActions = {
          editTb->actionSelect(),   editTb->actionAddFeature(),
          editTb->actionAddPoint(), editTb->actionAddLine(),
          editTb->actionAddPolygon(), editTb->actionReshape(),
          editTb->actionMove(),     editTb->actionDeleteFeatures(),
          editTb->actionVertexEdit(), editTb->actionSave(),
          editTb->actionCancel(),   editTb->actionUndo(),
          editTb->actionRedo()};
      for (QAction *a : editActions)
        QVERIFY2(a && !a->icon().isNull(),
                 qPrintable(QStringLiteral("edit action lacks icon: %1").arg(a->text())));

      for (const char *name :
           {"saveButton", "processingButton", "webServiceButton"})
      {
        auto *btn = m_win->findChild<QToolButton *>(QLatin1String(name));
        QVERIFY2(btn, name);
        QVERIFY2(!btn->icon().isNull(), name);
      }
      // #148：地图册入口在「智能编图 › 图件输出」（任务服务在场即接线）。
      {
        auto *mb = m_win->findChild<QToolButton *>(QStringLiteral("mapBookButton"));
        QVERIFY2(mb, "mapBookButton");
        QVERIFY(!mb->icon().isNull());
        auto *dock = m_win->findChild<QDockWidget *>(QStringLiteral("mapBookDock"));
        QVERIFY(dock);
        mb->click();
        QVERIFY(!dock->isHidden());
        dock->hide();
      }
      // designerButton 需要带 layoutSvc 的 attachWorkflows——套件内缺席则跳过。
      if (auto *d = m_win->findChild<QToolButton *>(QStringLiteral("designerButton")))
        QVERIFY(!d->icon().isNull());

      auto *chipsRow =
          m_win->findChild<QWidget *>(QStringLiteral("horizonChipRow"));
      QVERIFY(chipsRow);
      auto *chips =
          chipsRow->findChild<QWidget *>(QStringLiteral("horizonChips"));
      QVERIFY2(chips, "horizon chips must live on the dedicated action row");
    }

    // T29「在地图上显示」shell 接线：意图 → 实例化 + zoomToLayer + ~400ms
    // 闪烁（horizonFlashActive 起止）+ visibilityChanged 双向同步不崩。
    void showOnMapZoomsAndFlashes()
    {
      // 自建一个活着的工程：manifest 随 projectOpened 绑到本测试的临时目录
      // （早前测试的临时工程目录已销毁，manifest 会变只读）。
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(
                   dir.filePath(QStringLiteral("proj.qgz"))),
               "need a live project for manifest writes");

      // 声明一个可实例化的 horizon 图层（gpkg 面要素），把画布拉到远处。
      LayerDeclaration d;
      d.layerId = QStringLiteral("horizon.D61");
      d.horizon = QStringLiteral("D61");
      d.type = QStringLiteral("vector"); // instantiate 按类型建层：gpkg 是矢量
      d.source = QStringLiteral(FIXTURE_GPKG) + QStringLiteral("|layername=basin");
      d.group = QStringLiteral("03_Composite");
      QString declErr;
      QVERIFY2(m_ctx->layerSvc()->declare(d, &declErr), qPrintable(declErr));

      m_ctx->canvasCtl()->canvas()->setExtent(QgsRectangle(0, 0, 1, 1));
      const QgsRectangle before = m_ctx->canvasCtl()->canvas()->extent();

      auto *preview = m_win->findChild<QWidget *>(QStringLiteral("dataPreview"));
      QVERIFY(preview);
      // 经元系统发数据页预览的意图信号（attachWorkflows 已接线）。
      QVERIFY(QMetaObject::invokeMethod(
          preview, "showHorizonOnMapRequested",
          Q_ARG(QString, QStringLiteral("horizon.D61"))));
      QTest::qWait(50); // zoom + flash 启动

      // 缩放生效：画布范围离开了 1×1（向图层范围移动）。
      const QgsRectangle after = m_ctx->canvasCtl()->canvas()->extent();
      QVERIFY2(after != before, "zoomToLayer must move the canvas extent");

      // 闪烁窗口内 active；~600ms 后结束。
      QVERIFY(m_win->property("horizonFlashActive").toBool());
      auto *timer = m_win->findChild<QTimer *>(QStringLiteral("horizonFlashTimer"));
      QVERIFY(timer);
      QTest::qWait(600);
      QVERIFY(!m_win->property("horizonFlashActive").toBool());

      // 双向同步通路：图层树勾选/取消（QGIS 4 语义）不崩——按钮态由预览
      // 侧测试覆盖（setHorizonOnMap 断言）。
      QgsMapLayer *layer = m_ctx->layerSvc()->layer(QStringLiteral("horizon.D61"));
      QVERIFY(layer);
      QgsLayerTreeLayer *node = m_ctx->projectSvc()->project()
                                    ->layerTreeRoot()
                                    ->findLayer(layer->id());
      QVERIFY(node);
      node->setItemVisibilityChecked(false);
      node->setItemVisibilityChecked(true);
      QTest::qWait(10);

      // 不把图层泄漏给后续用例（空态测试断言 mapLayers().isEmpty()）。
      m_ctx->projectSvc()->project()->removeMapLayer(layer->id());
    }

    // (i) P1-06 / MEM-03: Rapid re-entrant calls to flashHorizonLayer must cleanly
    // clean up prior rubber bands without leaking orphaned items on the canvas or scene.
    void showOnMapRapidReentrantFlashDoesNotLeakRubberBand()
    {
      if (!m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                               m_ctx->compositionWf(), m_ctx->validationWf(),
                               m_ctx->importSvc(), m_ctx->seismicLink(),
                               m_ctx->processingSvc(), m_ctx->store(),
                               m_ctx->editingSvc(), m_ctx->layoutSvc(),
                               m_ctx->taskSvc());

      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(
                   dir.filePath(QStringLiteral("proj_reentrant.qgz"))),
               "need a live project for manifest writes");

      LayerDeclaration d;
      d.layerId = QStringLiteral("horizon.reentrant");
      d.horizon = QStringLiteral("D61");
      d.type = QStringLiteral("vector");
      d.source = QStringLiteral(FIXTURE_GPKG) + QStringLiteral("|layername=basin");
      d.group = QStringLiteral("03_Composite");
      QString declErr;
      QVERIFY2(m_ctx->layerSvc()->declare(d, &declErr), qPrintable(declErr));

      auto *preview = m_win->findChild<QWidget *>(QStringLiteral("dataPreview"));
      QVERIFY(preview);

      auto *cv = m_ctx->canvasCtl()->canvas();
      QVERIFY(cv);

      auto countSceneRubberBands = [cv]() -> int {
        int count = 0;
        if (cv && cv->scene())
        {
          for (auto *item : cv->scene()->items())
          {
            if (dynamic_cast<QgsRubberBand *>(item))
              ++count;
          }
        }
        return count;
      };

      auto countFlashRubberBands = [cv]() -> int {
        int count = 0;
        if (cv && cv->scene())
        {
          for (auto *item : cv->scene()->items())
          {
            if (auto *rb = dynamic_cast<QgsRubberBand *>(item))
            {
              if (rb->objectName() == QStringLiteral("horizonFlashRubberBand"))
                ++count;
            }
          }
        }
        return count;
      };

      const int baseSceneBands = countSceneRubberBands();

      // Initial state: 0 horizon flash rubber bands on scene or canvas
      QCOMPARE(countFlashRubberBands(), 0);
      QCOMPARE(cv->findChildren<QgsRubberBand *>(QStringLiteral("horizonFlashRubberBand")).count(), 0);

      // Re-entrancy stress: trigger rapid successive horizon flash requests (4 times, 20ms apart)
      for (int i = 0; i < 4; ++i)
      {
        QVERIFY(QMetaObject::invokeMethod(
            preview, "showHorizonOnMapRequested",
            Q_ARG(QString, QStringLiteral("horizon.reentrant"))));
        QTest::qWait(20);

        // Invariant: prior in-flight rubber band must be cleaned up; exactly 1 flash band on scene and canvas
        QCOMPARE(countFlashRubberBands(), 1);
        QCOMPARE(countSceneRubberBands(), baseSceneBands + 1);
        auto *activeBand = cv->findChild<QgsRubberBand *>(QStringLiteral("horizonFlashRubberBand"));
        QVERIFY(activeBand != nullptr);
      }

      QVERIFY(m_win->property("horizonFlashActive").toBool());

      // Allow flash animation to complete (~400ms duration)
      QTest::qWait(600);
      QVERIFY(!m_win->property("horizonFlashActive").toBool());

      // Invariant: strictly 0 horizon flash rubber bands remain on canvas or scene after completion
      QCOMPARE(cv->findChild<QgsRubberBand *>(QStringLiteral("horizonFlashRubberBand")), nullptr);
      QCOMPARE(cv->findChildren<QgsRubberBand *>(QStringLiteral("horizonFlashRubberBand")).count(), 0);
      QCOMPARE(countFlashRubberBands(), 0);
      QCOMPARE(countSceneRubberBands(), baseSceneBands);

      QgsMapLayer *layer = m_ctx->layerSvc()->layer(QStringLiteral("horizon.reentrant"));
      if (layer)
        m_ctx->projectSvc()->project()->removeMapLayer(layer->id());
    }

    // T20 恢复链路：坏 catalog → 告警 + 导入禁用；重开好工程（AppContext
    // 在 projectOpened 里同步重设 projectDir）→ 告警收起、导入放开。
    void catalogErrorRecoversOnReopen()
    {
      QTemporaryDir bad;
      QVERIFY(bad.isValid());
      const QString metaDir =
          QDir(bad.path()).filePath(QStringLiteral("artifacts/metadata"));
      QVERIFY(QDir().mkpath(metaDir));
      {
        QFile f(QDir(metaDir).filePath(QStringLiteral("catalog.json")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ not json");
      }
      DataImportService *svc = m_ctx->importSvc();
      QSignalSpy spy(svc, &DataImportService::catalogOpenFailed);
      svc->setProjectDir(bad.path());
      QCOMPARE(spy.count(), 1);

      auto *label = m_win->findChild<QLabel *>(QStringLiteral("statusCatalogError"));
      QVERIFY(label && label->isVisibleTo(m_win));
      QVERIFY(!m_win->findChild<QPushButton *>(QStringLiteral("importWells"))
                   ->isEnabled());

      QTemporaryDir good;
      QVERIFY(good.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(
                   good.filePath(QStringLiteral("ok.qgz"))),
               "reopen a healthy project");
      QVERIFY(!label->isVisibleTo(m_win));
      QVERIFY(m_win->findChild<QPushButton *>(QStringLiteral("importWells"))
                  ->isEnabled());
    }

    // ---- T31 空态：地图/图层树没有图层时给居中指引 ----
    void mapAndLayerTreeEmptyStates()
    {
      // offscreen 未 show：isVisible 受祖先链影响，用显式隐藏标记断言。
      auto *mapEmpty = m_win->findChild<QLabel *>(QStringLiteral("mapEmptyState"));
      auto *treeEmpty = m_win->findChild<QLabel *>(QStringLiteral("layerTreeEmptyState"));
      QVERIFY(mapEmpty && treeEmpty);
      QVERIFY2(mapEmpty->text().contains(QString::fromUtf8("还没有图层")),
               "guidance must name the next step");
      QVERIFY(treeEmpty->text().contains(QString::fromUtf8("图层树是空的")));

      // 当前工程（projectOpenWiresLayerTree 建的 temp 工程）没有图层 → 露出。
      QgsProject *proj = m_ctx->projectSvc()->project();
      QVERIFY(proj);
      QVERIFY(proj->mapLayers().isEmpty());
      QVERIFY(!mapEmpty->isHidden());
      QVERIFY(!treeEmpty->isHidden());

      // 加一层 → 两个空态都收起；删掉 → 回来。
      auto *vl = new QgsVectorLayer(QStringLiteral("Point"), QStringLiteral("临时井"),
                                    QStringLiteral("memory"));
      QVERIFY(vl->isValid());
      proj->addMapLayer(vl);
      QVERIFY(mapEmpty->isHidden());
      QVERIFY(treeEmpty->isHidden());
      proj->removeMapLayer(vl->id());
      QVERIFY(!mapEmpty->isHidden());
      QVERIFY(!treeEmpty->isHidden());
    }

    // Window state roundtrip: geometry + last page persist via QSettings;
    // canvas extent persists inside the .qgz custom properties.
    void windowStateAndExtentPersist()
    {
      // Geometry/state bytes: offscreen has no real window manager — Qt clamps
      // and repositions freely, so literal pos/size asserts are platform-bound.
      // Our contract: non-empty distinct blobs persisted; restore accepted;
      // dock visibility (a saveState payload) actually round-trips.
      m_win->show();
      auto *leftDock = m_win->findChild<QDockWidget *>(QStringLiteral("layerTreeDock"));
      QVERIFY(leftDock);
      leftDock->hide();
      QTest::qWait(10);
      m_win->showPage(QStringLiteral("constraint"));
      m_win->saveWindowState();
      leftDock->show(); // don't leak hidden-dock state into later tests

      QSettings s2(QStringLiteral("paleo"), QStringLiteral("paleo"));
      const QByteArray geom = s2.value(QStringLiteral("windowGeometry")).toByteArray();
      const QByteArray state = s2.value(QStringLiteral("windowState")).toByteArray();
      QVERIFY2(!geom.isEmpty(), "saveWindowState must persist geometry bytes");
      QVERIFY2(!state.isEmpty(), "saveWindowState must persist dock-state bytes");

      // A second window must own its own canvas, not reparent the first one.
      QgisCanvasController secondCanvas(m_ctx);
      PaleoMainWindow win2(&secondCanvas, m_ctx->projectSvc(),
                           m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
      QVERIFY2(win2.restoreGeometry(geom), "restoreGeometry rejected stored bytes");
      QVERIFY2(win2.restoreState(state), "restoreState rejected stored bytes");
      win2.show();
      QTest::qWait(10);
      auto *leftDock2 = win2.findChild<QDockWidget *>(QStringLiteral("layerTreeDock"));
      QVERIFY(leftDock2);
      QVERIFY(!leftDock2->isVisible()); // saveState round-trip: dock stays hidden

      QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
      QCOMPARE(s.value(QStringLiteral("lastPage")).toString(),
               QStringLiteral("constraint"));

      // Canvas extent → .qgz custom property. setExtent adjusts to the canvas
      // aspect ratio, so compare against the *actual* extent, not the request.
      m_ctx->canvasCtl()->canvas()->setExtent(QgsRectangle(10.5, 20.5, 110.5, 90.5));
      const QgsRectangle saved = m_ctx->canvasCtl()->canvas()->extent();
      m_win->saveCanvasExtent();
      bool ok = false;
      const QString raw = m_ctx->projectSvc()->project()->readEntry(
          QStringLiteral("paleo"), QStringLiteral("canvasExtent"), QString(), &ok);
      QVERIFY(ok);
      const QStringList parts = raw.split(QLatin1Char(','));
      QCOMPARE(parts.size(), 4);
      QVERIFY2(qAbs(parts.at(0).toDouble() - saved.xMinimum()) < 0.01 &&
               qAbs(parts.at(3).toDouble() - saved.yMaximum()) < 0.01,
               qPrintable(raw));

      // Reopen path: dirty extent → restore pulls the stored one back
      // (aspect-adjusted again, so verify the center is preserved).
      m_ctx->canvasCtl()->canvas()->setExtent(QgsRectangle(0, 0, 1, 1));
      m_win->restoreCanvasExtent();
      const QgsRectangle got = m_ctx->canvasCtl()->canvas()->extent();
      QVERIFY2(qAbs(got.center().x() - saved.center().x()) < 0.01 &&
               qAbs(got.center().y() - saved.center().y()) < 0.01,
               qPrintable(got.toString()));

      // Malformed/absent payload → no crash, extent untouched.
      m_ctx->projectSvc()->project()->writeEntry(
          QStringLiteral("paleo"), QStringLiteral("canvasExtent"),
          QStringLiteral("bogus"));
      const QgsRectangle before = m_ctx->canvasCtl()->canvas()->extent();
      m_win->restoreCanvasExtent();
      QCOMPARE(m_ctx->canvasCtl()->canvas()->extent(), before);
    }

    void dataNavigationIsManagedDock()
    {
      auto *dock = m_win->findChild<QDockWidget *>("dataListDock");
      auto *preview = m_win->findChild<DataPreviewTabs *>("dataPreview");
      QVERIFY(dock && preview);
      QVERIFY(dock->features().testFlag(QDockWidget::DockWidgetFloatable));
      QVERIFY(dock->findChild<DataListPanel *>());
      QVERIFY(!m_win->findChild<QSplitter *>("dataListPreviewSplit"));
      QScopedPointer<QMenu> menu(m_win->findChild<PaleoDockManager *>()->createMenu());
      QVERIFY(menu->actions().contains(dock->toggleViewAction()));
      m_win->showPage("data");
      QVERIFY(!dock->isHidden());
      for (const QString &page : {"predict", "constraint", "compose", "validate"}) {
        m_win->showPage(page);
        QVERIFY(dock->isHidden());
        QVERIFY(preview->isHidden());
      }
      m_win->showPage("data");
      QVERIFY(!dock->isHidden());
      auto *options = dock->findChild<QWidget *>("dataListAdvancedOptions");
      auto *button = dock->findChild<QToolButton *>("dataListOptionsButton");
      QVERIFY(options && button);
      QVERIFY(options->isHidden());
      button->click();
      QVERIFY(!options->isHidden());
      button->click();
      QVERIFY(options->isHidden());
    }

    void previewMaximizeRestoresDockLayout()
    {
      m_win->resize(1600, 1000);
      m_win->show();
      m_win->findChild<QStackedWidget *>("centerStack")->setCurrentIndex(1);
      m_win->showPage("data");
      auto *dock = m_win->findChild<QDockWidget *>("dataListDock");
      auto *right = m_win->findChild<QDockWidget *>("pagePanelDock");
      auto *preview = m_win->findChild<DataPreviewTabs *>("dataPreview");
      auto *inner = preview->findChild<QTabWidget *>("dataPreviewTabs");
      auto *button = preview->findChild<QToolButton *>("previewMaxButton");
      QVERIFY(dock && right && inner && button);
      right->show();
      inner->addTab(new QLabel("preview"), "test");
      QTest::qWait(30);
      m_win->resizeDocks({dock}, {340}, Qt::Horizontal);
      QTest::qWait(30);
      const int width = dock->width();
      const int before = preview->width();
      button->setChecked(true);
      QTest::qWait(30);
      QVERIFY(dock->isHidden());
      QVERIFY(right->isHidden());
      QVERIFY(preview->width() > before);
      button->setChecked(false);
      QTest::qWait(30);
      QCOMPARE(dock->width(), width);
      QVERIFY(!right->isHidden());
      inner->removeTab(0);
    }

    // #154/#156：换工程清工程作用域视图——预览标签全关、测井对比井集清空；
    // 重开工程时测井对比从 catalog 重灌（不依赖本会话的导入事件）。
    void projectSwitchResetsProjectScopedViews()
    {
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(),
                             m_ctx->taskSvc()); // 幂等：套件内已 attach 时为空操作
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      auto *corr = m_win->findChild<WellCorrelationPanel *>(QStringLiteral("correlationPanel"));
      QVERIFY(preview && corr);

      QTemporaryDir dirA, dirB;
      const QString qgzA = dirA.filePath(QStringLiteral("a.qgz"));
      QVERIFY(m_ctx->projectSvc()->createProject(qgzA));
      QString error;
      const QString asset = m_ctx->importSvc()->importProjectFile(
          QFINDTESTDATA("../testdata/project_area/A1.Las"), &error);
      QVERIFY2(!asset.isEmpty(), qPrintable(error));
      preview->openAsset(asset);
      QVERIFY(preview->tabCount() >= 1);
      int wellLogs = 0;
      for (const CatalogAsset &a : m_ctx->importSvc()->catalog()->assets())
        if (a.type == QLatin1String("well_log"))
          ++wellLogs;
      QVERIFY(wellLogs >= 1);
      corr->setWells({{asset, QStringLiteral("A1")}});
      QCOMPARE(corr->wellCount(), 1);

      QVERIFY(m_ctx->projectSvc()->createProject(dirB.filePath(QStringLiteral("b.qgz"))));
      QCOMPARE(preview->tabCount(), 0);   // #154：旧工程标签不残留
      QCOMPARE(corr->wellCount(), 0);     // #156：旧工程井集不残留

      QVERIFY(m_ctx->projectSvc()->openProject(qgzA));
      QTRY_COMPARE_WITH_TIMEOUT(corr->wellCount(), wellLogs, 3000); // #156：从 catalog 重灌
    }

    void canvasYieldsSpaceToDocks()
    {
      m_win->resize(1800, 1000);
      m_win->show();
      m_win->findChild<QStackedWidget *>("centerStack")->setCurrentIndex(1);
      auto *right = m_win->findChild<QDockWidget *>("pagePanelDock");
      auto *bottom = m_win->findChild<QDockWidget *>("bottomDock");
      auto *preview = m_win->findChild<DataPreviewTabs *>("dataPreview");
      QTemporaryDir project;
      QVERIFY(m_ctx->projectSvc()->createProject(project.filePath("layout.qgz")));
      QString error;
      const QString asset = m_ctx->importSvc()->importProjectFile(
          QFINDTESTDATA("../testdata/project_area/A1.Las"), &error);
      QVERIFY2(!asset.isEmpty(), qPrintable(error));
      preview->openAsset(asset);
      m_win->findChild<DataPage *>()->selectAsset(asset);
      auto *well = preview->findChild<WellComposite::WellCompositePanel *>();
      QVERIFY(well);
      for (const QString &page : {"data", "compose"}) {
        m_win->showPage(page);
        auto *left = m_win->findChild<QDockWidget *>(page == "data" ? "dataListDock" : "layerTreeDock");
        left->show();
        right->show();
        bottom->hide();
        m_win->resizeDocks({left, right}, {280, 280}, Qt::Horizontal);
        QTest::qWait(50);
        const QSize windowSize = m_win->size();
        QWidget *canvas = page == "data" ? well->findChild<QWidget *>("wellCompositeCanvas")
                                         : m_ctx->canvasCtl()->canvas();
        QVERIFY(canvas);
        const QSize before = canvas->size();
        m_win->resizeDocks({left, right}, {500, 550}, Qt::Horizontal);
        QTest::qWait(50);
        QCOMPARE(m_win->size(), windowSize);
        QVERIFY2(left->width() >= 490, qPrintable(QString::number(left->width())));
        QVERIFY2(right->width() >= 540, qPrintable(QString::number(right->width())));
        QVERIFY2(canvas->width() < before.width() - 300, qPrintable(QString("%1: %2 -> %3").arg(page).arg(before.width()).arg(canvas->width())));
        bottom->show();
        m_win->resizeDocks({bottom}, {240}, Qt::Vertical);
        QTest::qWait(50);
        QVERIFY(canvas->height() < before.height() - 100);
        bottom->hide();
        const QString capture = qEnvironmentVariable("PALEO_DATA_CAPTURE_PATH");
        if (page == "data" && !capture.isEmpty()) {
          QTest::qWait(50);
          QVERIFY(m_win->grab().save(capture));
        }
      }
      preview->closeAssetTab(asset);
      m_win->showPage("data");
    }

    // 数据列表的宽度绝不应双击数据项而改变，只能由用户调整。
    void dataListWidthUnchangedOnItemActivationAndFollowsUser()
    {
      m_win->resize(1280, 1100);
      m_win->show();
      if (auto *centerStack =
              m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack")))
        centerStack->setCurrentIndex(1);
      m_win->showPage(QStringLiteral("data"));
      QTest::qWait(30);

      auto *dock = m_win->findChild<QDockWidget *>(QStringLiteral("dataListDock"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      QVERIFY(dock && preview);
      auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
      QVERIFY(inner);

      while (inner->count() > 0)
        inner->removeTab(0);
      QTest::qWait(20);

      // 1. 用户拖拽分栏调整列表宽度为 275px
      const int customWidth = 275;
      m_win->resizeDocks({dock}, {customWidth}, Qt::Horizontal);
      QTest::qWait(20);
      QCOMPARE(dock->width(), customWidth);

      // 2. 双击/激活数据项，打开首个预览标签：数据列表宽度绝不得改变
      inner->addTab(new QLabel(QStringLiteral("item1")), QStringLiteral("Tab1"));
      QTest::qWait(20);
      QCOMPARE(dock->width(), customWidth);

      // 3. 打开第二个预览标签 / 切换标签：数据列表宽度绝不得改变
      inner->addTab(new QLabel(QStringLiteral("item2")), QStringLiteral("Tab2"));
      inner->setCurrentIndex(1);
      QTest::qWait(20);
      QCOMPARE(dock->width(), customWidth);

      // 4. 关闭所有标签回到空态：数据列表宽度绝不得改变
      inner->removeTab(1);
      inner->removeTab(0);
      QTest::qWait(20);
      QCOMPARE(dock->width(), customWidth);

      // 5. 再次打开标签：数据列表宽度依然保持用户设定值
      inner->addTab(new QLabel(QStringLiteral("item3")), QStringLiteral("Tab3"));
      QTest::qWait(20);
      QCOMPARE(dock->width(), customWidth);

      // 清理
      inner->removeTab(0);
    }

    // ---- P3 D9 分栏/布局契约回归（wave/data-page-operations）----
  private:
    DataOpsWidthCtx dataOpsWidthSetup()
    {
      m_win->resize(1280, 1100);
      m_win->show();
      if (auto *centerStack = m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack")))
        centerStack->setCurrentIndex(1);
      m_win->showPage(QStringLiteral("data"));
      QTest::qWait(30);
      DataOpsWidthCtx ctx;
      ctx.dock = m_win->findChild<QDockWidget *>(QStringLiteral("dataListDock"));
      ctx.width = 275;
      m_win->resizeDocks({ctx.dock}, {ctx.width}, Qt::Horizontal);
      QTest::qWait(20);
      return ctx;
    }

  private slots:
    // D9.3：过滤切换/未决快捷过滤/清除过滤下宽度不变。
    void dataListWidthStableUnderFilterOperations()
    {
      const auto ctx = dataOpsWidthSetup();
      QVERIFY(ctx.dock);
      auto *lp = m_win->findChild<DataListPanel *>();
      QVERIFY(lp);
      auto *search = lp->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
      search->setText(QStringLiteral("xyz-无命中"));
      QTest::qWait(20);
      QCOMPARE(ctx.dock->width(), ctx.width);
      auto *quick = lp->findChild<QWidget *>(QStringLiteral("pendingQuickBar"));
      if (auto *btn = quick->findChild<QPushButton *>(QStringLiteral("quickWarned")))
      {
        btn->click();
        QTest::qWait(20);
        QCOMPARE(ctx.dock->width(), ctx.width);
        btn->click();
      }
      // D2.10 状态串应用（过滤器整组替换）。
      lp->setFilterFromStateString(
          QStringLiteral("paleo://dataops-filter?op=and&q=%E6%B5%8B%E8%AF%95"));
      QTest::qWait(20);
      QCOMPARE(ctx.dock->width(), ctx.width);
      search->clear();
      QTest::qWait(20);
      QCOMPARE(ctx.dock->width(), ctx.width);
    }

    // D9.2/D9.3：视图五态切换（树/表/图标/高速/分组）+ 多选/全选/反选下宽度不变。
    void dataListWidthStableUnderViewModesAndSelection()
    {
      const auto ctx = dataOpsWidthSetup();
      QVERIFY(ctx.dock);
      auto *lp = m_win->findChild<DataListPanel *>();
      QVERIFY(lp);
      for (int mode = 0; mode <= 4; ++mode)
      {
        lp->setViewMode(mode);
        QTest::qWait(15);
        QCOMPARE(ctx.dock->width(), ctx.width);
      }
      // 选择操作（D1.9 全选/反选——空目录下是空操作，但不得触发重排宽度）。
      lp->selectAllVisibleAssets();
      lp->invertAssetSelection();
      QTest::qWait(15);
      QCOMPARE(ctx.dock->width(), ctx.width);
      // 撤销/重做空栈调用（D5 面按钮态刷新）。
      lp->undoOp();
      lp->redoOp();
      QTest::qWait(15);
      QCOMPARE(ctx.dock->width(), ctx.width);
    }

    // D9.4：窗口 resize 是允许的被动分配——总宽随窗口走，列表宽不越界涨。
    void dataListWidthFollowsWindowResize()
    {
      const auto ctx = dataOpsWidthSetup();
      QVERIFY(ctx.dock);
      const int totalBefore = ctx.dock->width() + m_win->centralWidget()->width();
      m_win->resize(1560, 1100); // +280
      QTest::qWait(30);
      const int totalAfter = ctx.dock->width() + m_win->centralWidget()->width();
      QVERIFY2(totalAfter > totalBefore, "resize must redistribute more total width");
      // 列表侧不得借机自涨超过用户设定 + 增量的一半（被动分配以预览侧为主）。
      QVERIFY2(ctx.dock->width() <= ctx.width + 140,
               qPrintable(QStringLiteral("list grew to %1").arg(ctx.dock->width())));
      m_win->resize(1280, 1100);
      QTest::qWait(30);
    }

    // wave/data-integrity：attachWorkflows 幂等——同一窗口二次调用不得重复
    // 建 dock/连接/崩溃（旧行为：重复建 correlationPanel/editingToolbar/
    // processingButton 等 + 叠加信号连接，二次调用后后续用例段错误）。
    // 直接证据：连续两次调用后 dock 总数与逐对象名单不翻倍、右栏页数不变，
    // 且页切换照常不崩。单跑本用例（首次 attach）与套件内（第二次触达）
    // 都必须过——这正是原 bug 的表现面。
    void attachWorkflowsIsIdempotent()
    {
      const auto attachAll = [this] {
        m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                               m_ctx->compositionWf(), m_ctx->validationWf(),
                               m_ctx->importSvc(), m_ctx->seismicLink(),
                               m_ctx->processingSvc(), m_ctx->store(),
                               m_ctx->editingSvc(), m_ctx->layoutSvc(),
                               m_ctx->taskSvc());
      };
      attachAll(); // 套件路径里前面用例已 attach 过——幂等语义下这次是空操作

      const int docksAfterFirst = m_win->findChildren<QDockWidget *>().size();
      const auto namedCount = [this](const char *name) {
        return m_win->findChildren<QWidget *>(QLatin1String(name)).size();
      };
      for (const char *name : {"correlationPanel", "editingToolbar",
                               "releasePanel", "attributeTablePanel", "processingButton",
                               "statusCatalogError", "wellSectionDock",
                               "wellSectionPanel", "mapBookDock", "mapBookPanel"})
        QCOMPARE(namedCount(name), 1);
      auto *host = m_win->findChild<QWidget *>(QStringLiteral("rightPanelHost"));
      QVERIFY(host);
      QCOMPARE(static_cast<QStackedLayout *>(host->layout())->count(), 5);

      attachAll(); // 二次调用：必须早退——不翻倍、不叠加连接、不崩。

      QCOMPARE(m_win->findChildren<QDockWidget *>().size(), docksAfterFirst);
      for (const char *name : {"correlationPanel", "editingToolbar",
                               "releasePanel", "attributeTablePanel", "processingButton",
                               "statusCatalogError", "wellSectionDock",
                               "wellSectionPanel", "mapBookDock", "mapBookPanel"})
        QCOMPARE(namedCount(name), 1);
      QCOMPARE(static_cast<QStackedLayout *>(host->layout())->count(), 5);

      // 不崩的直接证据：二次调用后窗口照常响应页切换。
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(m_win->currentPage(), QStringLiteral("data"));
    }

    // goal/wellsection 装配：ribbon 动作 `ribbonWellSectionAction` 触发 →
    // wellSectionDock 现；数据页收；编图页按用户意愿恢复。
    void wellSectionDockRibbonAndPageVisibility()
    {
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(),
                             m_ctx->taskSvc());
      auto *dock =
          m_win->findChild<QDockWidget *>(QStringLiteral("wellSectionDock"));
      auto *panel =
          m_win->findChild<QWidget *>(QStringLiteral("wellSectionPanel"));
      QVERIFY(dock);
      QVERIFY(panel);
      QVERIFY(dock->isAncestorOf(panel)); // dock 内层有 scrollHost 包装
      m_win->show();
      QTest::qWait(10);
      auto *act =
          m_win->findChild<QAction *>(QStringLiteral("ribbonWellSectionAction"));
      QVERIFY(act);
      // 三页共享的「连井分析」ribbon 面板在位。
      for (const char *name : {"ribbonPanel.constraint.correlation",
                               "ribbonPanel.predict.correlation",
                               "ribbonPanel.compose.correlation"})
        QVERIFY2(m_win->findChild<QWidget *>(QLatin1String(name)), name);

      m_win->showPage(QStringLiteral("constraint"));
      dock->setVisible(false);
      act->trigger();
      QVERIFY(dock->isVisible());

      m_win->showPage(QStringLiteral("data"));
      QVERIFY(!dock->isVisible());
      m_win->showPage(QStringLiteral("constraint"));
      QVERIFY(dock->isVisible()); // 用户意愿保留 → 编图页恢复
    }

    // 页作用域工具面（用户裁决）：数字化/编辑工具只属于编图链三页
    // （predict/constraint/compose）——数据管理页与验证页不得出现 QGIS
    // 编图工具。进入非编辑页时活动画布工具被停用（画布只读展示）。
    void editingToolsScopedToMappingPages()
    {
      // attachWorkflows 幂等：套件内已 attach 则空操作；单跑则首次建 dock。
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(),
                             m_ctx->taskSvc());
      auto *tb = m_win->findChild<QWidget *>(QStringLiteral("editingToolbar"));
      QVERIFY(tb);
      // 归属 ribbon 体：不再有 dock 包装——编辑条动作挂在编图链三页的 ribbon 面板中。
      QVERIFY(!m_win->findChild<QDockWidget *>(QStringLiteral("editToolbarDock")));
      QVERIFY(tb->parentWidget() == m_win);

      // 编辑命令组只在编图链三页（predict / constraint / compose）的 ribbon 页签中；
      // 数据管理页与验证页无此面板。
      for (const QString &p : {QStringLiteral("data"), QStringLiteral("validate")})
      {
        SARibbonCategory *cat = m_win->categoryForPage(p);
        QVERIFY(cat);
        QVERIFY(!cat->findChild<SARibbonPanel *>(QStringLiteral("ribbonEditPanel")));
      }
      for (const QString &p : {QStringLiteral("predict"), QStringLiteral("constraint"),
                               QStringLiteral("compose")})
      {
        SARibbonCategory *cat = m_win->categoryForPage(p);
        QVERIFY(cat);
        QVERIFY(cat->findChild<SARibbonPanel *>(QStringLiteral("ribbonEditPanel")));
      }

      // 活动工具随页停用：借原生 QgsMapToolPan 当活动工具，落数据页后
      // 画布不再持有任何工具（非编辑页 deactivate 等价 Esc）。
      auto *pan = new QgsMapToolPan(m_ctx->canvasCtl()->canvas());
      m_ctx->canvasCtl()->setMapTool(pan);
      QCOMPARE(m_ctx->canvasCtl()->activeTool(), pan);
      m_win->showPage(QStringLiteral("data"));
      QVERIFY(m_ctx->canvasCtl()->activeTool() == nullptr);
      delete pan;
    }

    void mapContextAndLayerSelectionStayInSync()
    {
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
      auto *editor = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
      auto *tree = m_win->findChild<QgsLayerTreeView *>(QStringLiteral("layerTreeView"));
      auto *hint = m_win->findChild<QLabel *>(QStringLiteral("mapInteractionHint"));
      auto *stop = m_win->findChild<QToolButton *>(QStringLiteral("stopMapToolButton"));
      auto *pan = m_win->findChild<QAction *>(QStringLiteral("ribbonPanAction"));
      QVERIFY(editor && tree && hint && stop && pan);
      auto *project = m_ctx->projectSvc()->project();
      auto *first = new QgsVectorLayer(QStringLiteral("Point"), QStringLiteral("井位测试"), QStringLiteral("memory"));
      auto *second = new QgsVectorLayer(QStringLiteral("LineString"), QStringLiteral("约束线测试"), QStringLiteral("memory"));
      project->addMapLayer(first);
      project->addMapLayer(second);
      m_win->showPage(QStringLiteral("compose"));
      tree->setCurrentLayer(first);
      QCOMPARE(editor->currentLayer(), first);
      QCOMPARE(m_ctx->canvasCtl()->canvas()->currentLayer(), first);
      editor->setCurrentLayer(second);
      QCOMPARE(tree->currentLayer(), second);
      for (auto *combo : m_win->findChildren<QComboBox *>(QStringLiteral("ribbonEditLayerCombo")))
        QCOMPARE(qvariant_cast<QgsVectorLayer *>(combo->currentData()), second);
      auto *targetNode = project->layerTreeRoot()->findLayer(second->id());
      QVERIFY(targetNode);
      targetNode->setItemVisibilityChecked(false);
      editor->actionSelect()->trigger();
      QVERIFY(targetNode->isVisible());
      QVERIFY(!second->isEditable());
      QVERIFY(hint->text().contains(QStringLiteral("选择")));
      QVERIFY(hint->text().contains(second->name()));
      editor->actionAddLine()->trigger();
      QVERIFY(second->isEditable());
      tree->setCurrentLayer(first); // live session refuses target switches everywhere
      QCOMPARE(tree->currentLayer(), second);
      QCOMPARE(editor->currentLayer(), second);
      pan->trigger();
      QVERIFY(pan->isChecked());
      QVERIFY(!editor->actionAddLine()->isChecked());
      QVERIFY(hint->text().contains(QStringLiteral("平移")));
      editor->actionAddLine()->trigger();
      QVERIFY(!pan->isChecked());
      QVERIFY(editor->actionAddLine()->isChecked());
      // Optional visual evidence from the real widget tree, using the same flow.
      const QString capturePath = qEnvironmentVariable("PALEO_UI_CAPTURE");
      if (!capturePath.isEmpty())
      {
        m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack"))->setCurrentIndex(1);
        m_win->show();
        QTest::qWait(100);
        QVERIFY(m_win->grab().save(capturePath));
        PaleoTheme::applyDarkTheme();
        PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet());
        QTest::qWait(50);
        QVERIFY(m_win->grab().save(capturePath + QStringLiteral(".dark.png")));
        PaleoTheme::applyLightTheme();
        PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet());
      }
      stop->click();
      QVERIFY(!m_ctx->canvasCtl()->canvas()->mapTool());
      QVERIFY(!editor->actionAddLine()->isChecked());
      QVERIFY(second->isEditable()); // ending a tool doesn't discard edits
      QVERIFY(!stop->isEnabled());
      QVERIFY(editor->cancelEditing());
      editor->actionAddLine()->trigger();
      m_win->showPage(QStringLiteral("data"));
      QVERIFY(!m_ctx->canvasCtl()->canvas()->mapTool());
      QVERIFY(!editor->actionAddLine()->isChecked());
      QVERIFY(editor->cancelEditing());
      project->removeMapLayer(first);
      project->removeMapLayer(second);
    }

    // 数据页是列表面（用户裁决）：中央工作区切到数据面（数据列表 + 预览），
    // 编图链四页切到画布面（层位 chips + 画布）。预览分栏显隐沿用旧约（预览只在数据页）。
    void dataPageHidesCanvasForLists()
    {
      auto *dock = m_win->findChild<QDockWidget *>(QStringLiteral("dataListDock"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      auto *workspaceStack = m_win->findChild<QStackedWidget *>(QStringLiteral("workspaceStack"));
      QVERIFY(dock && preview && workspaceStack);

      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(workspaceStack->currentIndex(), 1);
      QVERIFY(!preview->isHidden());

      for (const QString &p : {QStringLiteral("predict"), QStringLiteral("constraint"),
                               QStringLiteral("compose"), QStringLiteral("validate")})
      {
        m_win->showPage(p);
        QCOMPARE(workspaceStack->currentIndex(), 0);
        QVERIFY2(preview->isHidden(), qPrintable(p));
      }
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(workspaceStack->currentIndex(), 1);
      QVERIFY(!preview->isHidden());
    }

    // 面板管理（右键 dock 标题栏 = 顶栏「面板」钮）：createPopupMenu
    // 列出全部 dock 的 toggleViewAction；编辑条是 ribbon 行内控件，
    // 页作用域归 showPage——不在可关清单里。
    void panelMenuListsDocks()
    {
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(),
                             m_ctx->taskSvc());
      auto *btn = m_win->findChild<QToolButton *>(QStringLiteral("panelsMenuButton"));
      QVERIFY(btn);
      QCOMPARE(btn->text(), QStringLiteral("布局"));

      QMenu *menu = m_win->createPopupMenu();
      QVERIFY(menu);
      QStringList texts;
      for (QAction *a : menu->actions())
        texts << a->text();
      auto *rightDock = m_win->findChild<QDockWidget *>(QStringLiteral("pagePanelDock"));
      const QString rightDockTitle = rightDock ? rightDock->windowTitle() : QStringLiteral("页面面板");
      for (const QString &t : {QStringLiteral("图层"), rightDockTitle,
                               QStringLiteral("日志 / 任务"), QStringLiteral("Web 服务")})
        QVERIFY2(texts.contains(t), qPrintable(t + " / got: " + texts.join(",")));
      // 每个非分隔符条目是 dock 的 toggleViewAction——可勾选
      const QList<QAction *> acts = menu->actions(); // 迭代器必须同一容器
      QVERIFY(std::all_of(acts.begin(), acts.end(),
                          [](QAction *a) { return a->isSeparator() || a->isCheckable(); }));
      delete menu;
    }

    // Ribbon 工作流五页与工作区视图联动：
    // 1. 五个页签全部就位且文案对齐设计（数据管理 / 预测编图 / 单因素图 / 智能编图 / 验证）；
    // 2. 数据管理页处于数据面（index 1：数据列表 + 数据预览）；
    // 3. 预测/单因素/智能编图/验证页处于画布面（index 0：层位 chips + QgsMapCanvas）。
    void ribbonWorkflowCategoriesAndWorkspaceSwitching()
    {
      const QStringList pages = {
        QStringLiteral("data"), QStringLiteral("predict"), QStringLiteral("constraint"),
        QStringLiteral("compose"), QStringLiteral("validate")
      };
      const QStringList expectedTitles = {
        QStringLiteral("数据管理"), QStringLiteral("预测编图"), QStringLiteral("单因素图"),
        QStringLiteral("智能编图"), QStringLiteral("验证")
      };

      for (int i = 0; i < pages.size(); ++i)
      {
        SARibbonCategory *cat = m_win->categoryForPage(pages.at(i));
        QVERIFY2(cat != nullptr, qPrintable("Missing category for page: " + pages.at(i)));
        QCOMPARE(cat->categoryName(), expectedTitles.at(i));
      }
      QVERIFY(m_win->categoryForPage(QStringLiteral("nonexistent")) == nullptr);

      auto *workspaceStack = m_win->findChild<QStackedWidget *>(QStringLiteral("workspaceStack"));
      QVERIFY(workspaceStack);

      // 数据管理页：工作区展示列表与预览分栏（index 1）
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(workspaceStack->currentIndex(), 1);

      // 预测编图、单因素图、智能编图、验证页：工作区展示 QGIS 画布与层位栏（index 0）
      for (const QString &p : {QStringLiteral("predict"), QStringLiteral("constraint"),
                               QStringLiteral("compose"), QStringLiteral("validate")})
      {
        m_win->showPage(p);
        QCOMPARE(workspaceStack->currentIndex(), 0);
      }

      // 切回数据管理页再次验证幂等性
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(workspaceStack->currentIndex(), 1);
    }
    // Reproducible audit of the production shell, using native widgets and
    // isolated synthetic project data. No user settings or files are touched.
    void workflowAuditSnapshots()
    {
      const QString dir = qEnvironmentVariable("PALEO_WORKFLOW_CAPTURE");
      if (dir.isEmpty())
        QSKIP("Set PALEO_WORKFLOW_CAPTURE to capture the five workflow pages");
      QVERIFY(QDir().mkpath(dir));
      QTemporaryDir project;
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                            m_ctx->compositionWf(), m_ctx->validationWf(),
                            m_ctx->importSvc(), m_ctx->seismicLink(),
                            m_ctx->processingSvc(), m_ctx->store(),
                            m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
      m_win->attachWorkbench(m_ctx->mappingWorkbench());
      m_win->resize(1440, 900);
      m_win->show();
      QTest::qWait(100);
      QVERIFY(m_win->grab().save(dir + "/00-startup.png"));
      QVERIFY(m_ctx->projectSvc()->createProject(project.filePath("audit.qgz")));
      auto *chip = m_win->findChild<QToolButton *>("chip_D61");
      QVERIFY(chip && chip->isEnabled());
      chip->click();
      const QStringList pages{"data", "predict", "constraint", "compose", "validate"};
      for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark})
      {
        if (theme == PaleoTheme::Theme::Light)
          PaleoTheme::applyLightTheme();
        else
          PaleoTheme::applyDarkTheme();
        PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() +
                                       PaleoTheme::focusRingStyleSheet());
        for (int i = 0; i < pages.size(); ++i)
        {
          m_win->showPage(pages[i]);
          QTest::qWait(100);
          QCOMPARE(m_win->currentPage(), pages[i]);
          QVERIFY(m_win->grab().save(dir + QString("/%1-%2-%3.png")
              .arg(i + 1, 2, 10, QLatin1Char('0')).arg(pages[i],
                  theme == PaleoTheme::Theme::Light ? "light" : "dark")));
        }
      }
      PaleoTheme::applyLightTheme();
      PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() +
                                     PaleoTheme::focusRingStyleSheet());
    }

    void secondarySurfaceAuditSnapshots()
    {
      const QString dir = qEnvironmentVariable("PALEO_SECONDARY_CAPTURE");
      if (dir.isEmpty())
        QSKIP("Set PALEO_SECONDARY_CAPTURE to capture native secondary surfaces");
      QVERIFY(QDir().mkpath(dir));
      QgsPrintLayout layout(m_ctx->projectSvc()->project());
      layout.initializeDefaults();
      PaleoLayoutDesignerShell designer(&layout);
      QDialog folder;
      FolderPreviewRow log;
      log.path = QStringLiteral("/audit/测井/示例井.las");
      log.classifiedType = QStringLiteral("well_log");
      log.sizeBytes = 4096;
      PaleoFolderConfirm::buildFolderConfirmDialog(&folder, QStringLiteral("/audit"), {log}, {});
      SectionSetupDialog setup;
      seismic::SeismicSectionDockWidget section;
      paleo::fault::FaultManagerPanel faults(nullptr, nullptr);
      WellComposite::CurveData curve;
      curve.name = QStringLiteral("GR");
      curve.unit = QStringLiteral("API");
      curve.depths = {1000, 1010, 1020, 1030, 1040};
      curve.values = {30, 45, 80, 50, 20};
      WellComposite::WellCompositePanel well;
      well.setProjectName(QStringLiteral("visual-audit"));
      QVERIFY(well.loadLasCurves(QStringLiteral("示例井（合成资料）"), {curve}));
      well.resize(1100, 700);
      WellComposite::CurveConfigDialog curves(well.canvas());
      PropertyModelPanel properties;
      properties.resize(420, 500);
      section.resize(1440, 700);
      faults.resize(800, 450);
      const QList<QPair<QString, QWidget *>> surfaces{
          {QStringLiteral("06-import"), &folder},
          {QStringLiteral("07-section-setup"), &setup},
          {QStringLiteral("08-section"), &section},
          {QStringLiteral("09-faults"), &faults},
          {QStringLiteral("10-designer"), &designer},
          {QStringLiteral("11-well"), &well},
          {QStringLiteral("12-curves"), &curves},
          {QStringLiteral("13-property-model"), &properties}};
      for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark})
      {
        PaleoTheme::applyTheme(theme);
        for (const auto &surface : surfaces)
        {
          surface.second->show();
          QTest::qWait(100);
          QVERIFY(surface.second->grab().save(dir + QLatin1Char('/') + surface.first +
              (theme == PaleoTheme::Theme::Light ? "-light.png" : "-dark.png")));
          surface.second->hide();
        }
      }
      PaleoTheme::applyLightTheme();
    }

    void interactiveConstraintBatchUsesEditingSession()
    {
      QTemporaryDir dir;
      QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath("editing.qgz")));
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
          m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(), m_ctx->processingSvc(),
          m_ctx->store(), m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
      m_ctx->selection()->setActiveHorizon("D61");
      auto *workflow = m_ctx->constraintWf();
      QString error;
      for (int i=0; i<2; ++i)
        QVERIFY2(workflow->addConstraint("D61", QString("LINESTRING(0 %1, 10 %1)").arg(i*5),
            "direction_line", 7, &error, nullptr, {{"semantic", "direction_guide"}, {"opaque", 42}, {"enabled", false}}), qPrintable(error));
      auto *page = m_win->findChild<ConstraintPage *>();
      auto *toolbar = m_win->findChild<PaleoEditingToolbar *>("editingToolbar");
      QVERIFY(page && toolbar);
      auto *horizons = page->findChild<QComboBox *>("horizonCombo");
      if (horizons->findText("D61") < 0)
        horizons->addItem(QStringLiteral("D61"));
      QVERIFY(horizons->findText("D61") >= 0);
      horizons->setCurrentIndex(horizons->findText("D61"));
      page->refreshConstraintList();
      auto *list = page->findChild<QListWidget *>("constraintList");
      QCOMPARE(list->count(), 2);
      list->setCurrentRow(0);
      list->item(0)->setSelected(true);
      list->item(1)->setSelected(true);
      const auto original = workflow->constraintStore()->load("D61");
      auto *semantic = page->findChild<QComboBox *>("constraintSemanticCombo");
      semantic->setCurrentIndex(semantic->findData("interpretive_boundary"));
      page->findChild<QPushButton *>("constraintBatchTypeButton")->click();
      QVERIFY2(toolbar->isEditing(), qPrintable(page->findChild<QLabel *>("statusLabel")->text() + " / " + m_win->statusBar()->currentMessage()));
      const auto changed = workflow->constraintStore()->load("D61");
      for (const auto &row : changed)
      {
        QCOMPARE(row.value("type").toString(), QString("interpretive_boundary"));
        const auto params = QJsonDocument::fromJson(row.value("params_json").toString().toUtf8()).object();
        QCOMPARE(params.value("opaque").toInt(), 42);
        QCOMPARE(params.value("enabled").toBool(), false);
      }
      toolbar->undoStack()->undo();
      QCOMPARE(workflow->constraintStore()->load("D61"), original);
      QCOMPARE(list->item(0)->data(Qt::UserRole+1).toString(), original.first().value("params_json").toString());
      toolbar->undoStack()->redo();
      QCOMPARE(workflow->constraintStore()->load("D61"), changed);
      list->setCurrentRow(0);
      page->findChild<QDoubleSpinBox *>("constraintAngleSpin")->setValue(0);
      page->findChild<QPushButton *>("constraintParamSaveButton")->click();
      auto *layer = toolbar->currentLayer();
      const auto feature = layer->getFeature(changed.first().value("fid").toLongLong());
      const auto line = feature.geometry().asPolyline();
      QVERIFY(std::abs(line.first().x()-line.last().x()) < 1e-8);
      const QString shots = qEnvironmentVariable("PALEO_EDITING_SCREENSHOT_DIR");
      if (!shots.isEmpty())
      {
        QVERIFY(QDir().mkpath(shots));
        m_win->showPage("constraint");
        m_win->resize(1600,1000);
        m_win->show();
        auto *scroll = page->findChild<QScrollArea *>("constraintPageScroll");
        QVERIFY(scroll);
        scroll->ensureWidgetVisible(page->findChild<QPushButton *>("constraintParamSaveButton"));
        for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark})
        {
          PaleoTheme::applyTheme(theme);
          PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet());
          QCoreApplication::processEvents();
          QVERIFY(m_win->grab().save(shots + (theme == PaleoTheme::Theme::Light ? "/editing-light.png" : "/editing-dark.png")));
        }
        PaleoTheme::applyLightTheme();
        PaleoRibbon::applyTheme(m_win, PaleoTheme::shellStyleSheet() + PaleoTheme::focusRingStyleSheet());
      }
      QVERIFY(toolbar->saveEditing());
      const auto saved = workflow->constraintStore()->load("D61");
      QCOMPARE(saved.first().value("horizon").toString(), QString("D61"));
      for (const auto &row : saved)
        QVERIFY(!QJsonDocument::fromJson(row.value("params_json").toString().toUtf8()).object().value("enabled").toBool());
      QVERIFY(!toolbar->isEditing());
      auto *snap = m_win->findChild<QSpinBox *>("ribbonEditingSnapToleranceSpin");
      QVERIFY(snap);
      snap->setValue(13);
      QCOMPARE(m_ctx->canvasCtl()->canvas()->snappingUtils()->config().tolerance(), 13.0);
    }

    void mappingWorkbenchCanvasRibbonAndReferences()
    {
      QTemporaryDir dir;
      QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath("mapping.qgz")));
      m_win->attachWorkflows(m_ctx->predictionWf(),m_ctx->constraintWf(),m_ctx->compositionWf(),m_ctx->validationWf(),m_ctx->importSvc(),m_ctx->seismicLink(),m_ctx->processingSvc(),m_ctx->store(),m_ctx->editingSvc(),m_ctx->layoutSvc(),m_ctx->taskSvc());
      m_win->attachWorkbench(m_ctx->mappingWorkbench());
      QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
      auto *d61=m_win->findChild<QToolButton *>("chip_D61");auto *d62=m_win->findChild<QToolButton *>("chip_D62");QVERIFY(d61 && d61->isEnabled());QVERIFY(d62 && d62->isEnabled());d61->click();m_win->showPage("predict");
      auto *catalog=m_ctx->importSvc()->catalog();QVERIFY(catalog && catalog->isOpen());
      DerivedAssetRegistrar registrar(catalog,dir.path());auto st=registrar.stage("seismic","示例地震体","volume.bin");QVERIFY(st.isValid());QFile source(st.absolutePath);QVERIFY(source.open(QIODevice::WriteOnly));source.write("mock volume");source.close();QVERIFY(registrar.commit(st,{},"test",{}));
      CatalogEntity e;e.id="workbench-survey";e.name="示例工区";e.entityType="seismic_survey";e.corners={{0,0},{640,0},{640,640},{0,640}};QVERIFY(catalog->addEntity(e));EntityAssetLink link;link.entityType=e.entityType;link.entityId=e.id;link.assetId=st.assetId;link.role="seismic_volume";QVERIFY(catalog->addLink(link));
      auto *page=m_win->findChild<MappingWorkbenchPage *>("mappingWorkbench.predict");QVERIFY(page);auto *inputs=page->findChild<QListWidget *>("workbenchInputs");QVERIFY(inputs && inputs->count()==1);inputs->item(0)->setCheckState(Qt::Checked);
      auto *run=m_win->findChild<QAction *>("ribbonRunPrediction");QVERIFY(run && run->isEnabled());run->trigger();QVERIFY(m_ctx->mappingWorkbench()->busy());QVERIFY(!run->isEnabled());QTRY_VERIFY_WITH_TIMEOUT(!m_ctx->mappingWorkbench()->busy(),5000);
      const auto id=page->selectedLayer();QVERIFY(!id.isEmpty());auto *layer=m_ctx->layerSvc()->layer(id);QVERIFY(layer);QTRY_VERIFY(m_ctx->canvasCtl()->canvas()->layers().contains(layer));QCOMPARE(m_ctx->canvasCtl()->canvas()->currentLayer(),layer);
      auto *decor=m_win->findChild<PaleoDecorationManager *>();QVERIFY(decor);QVERIFY(decor->isNorthArrowEnabled());QVERIFY(decor->isScaleBarEnabled());QVERIFY(decor->legendTitle().contains("D61"));QVERIFY(decor->legendTitle().contains("Mock"));
      page->commandButton("compare")->click();auto *ref=m_win->findChild<QDialog *>("mappingReferenceWindow");QVERIFY(ref);auto *referenceCanvas=ref->findChild<QgsMapCanvas *>("referenceCanvas");QVERIFY(referenceCanvas);QCOMPARE(referenceCanvas->layers().size(),1);QPointer<QgsMapLayer> reference=referenceCanvas->layers().first();QVERIFY(reference!=layer);
      QPointer<QgsMapLayer> mainLayer=layer;d62->click();QVERIFY(mainLayer.isNull());QVERIFY(reference && reference->isValid());QCOMPARE(referenceCanvas->layers().first(),reference.data());QVERIFY(decor->legendTitle().contains("D62"));
      d61->click();QTRY_VERIFY(m_ctx->canvasCtl()->canvas()->layers().contains(m_ctx->layerSvc()->layer(id)));QTRY_COMPARE(page->selectedLayer(),id);page->commandButton("show")->click();ref->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
      PaleoTheme::applyLightTheme();m_win->resize(1600,1000);m_win->show();QTest::qWait(350);
      QTRY_VERIFY(m_ctx->canvasCtl()->canvas()->layers().contains(m_ctx->layerSvc()->layer(id)));
      QVERIFY(m_ctx->projectSvc()->project()->layerTreeRoot()->findLayer(m_ctx->layerSvc()->layer(id)->id())->isVisible());
      auto visibleFaciesPixels = [this] {
        const auto image = m_ctx->canvasCtl()->canvas()->grab().toImage();
        int count = 0;
        for (int y = 100; y < image.height() - 60; y += 4)
          for (int x = 100; x < image.width() - 260; x += 4) {
            const auto c = image.pixelColor(x, y);
            for (const auto &f : FaciesCatalog::defaults())
              if (c == QColor(f.toMap().value("color").toString())) {
                ++count;
                break;
              }
          }
        return count;
      };
      QTRY_VERIFY_WITH_TIMEOUT(visibleFaciesPixels()>100,5000);
      if(const auto capture=qEnvironmentVariable("PALEO_MAPPING_CAPTURE");!capture.isEmpty())QVERIFY(m_win->grab().save(capture));
      page->commandButton("polygonize")->click();QVERIFY(page->selectedLayer()!=id);QVERIFY(page->commandButton("copy")->isEnabled());
      page->commandButton("copy")->click();const auto draft=page->selectedLayer();QVERIFY2(draft.startsWith("draft."),qPrintable(page->findChild<QLabel *>("workbenchMessage")->text()));auto *editing=m_win->findChild<PaleoEditingToolbar *>("editingToolbar");QVERIFY(editing && editing->isEditing());auto previous=m_ctx->mappingWorkbench()->versionForLayer(draft);auto *vector=editing->currentLayer();QVERIFY(vector);QgsFeature feature;auto fi=vector->getFeatures();QVERIFY(fi.nextFeature(feature));const int field=vector->fields().indexOf("facies_code");QVERIFY(field>=0);QVERIFY(vector->changeAttributeValue(feature.id(),field,feature.attribute(field).toInt()==1?2:1));editing->actionSave()->trigger();QVERIFY(!editing->isEditing());QCOMPARE(m_ctx->mappingWorkbench()->versionForLayer(draft).versionNumber,previous.versionNumber+1);
      m_win->showPage("compose");auto *compose=m_win->findChild<MappingWorkbenchPage *>("mappingWorkbench.compose");compose->selectLayer(draft);auto *save=m_win->findChild<QAction *>("ribbonSaveVersion");QVERIFY(save && save->isEnabled());QCOMPARE(save->text(),compose->commandButton("save")->text());
      auto wellStage = registrar.stage("well_log", "井道测试", "review.las");
      QVERIFY(wellStage.isValid());
      QFile las(wellStage.absolutePath);
      QVERIFY(las.open(QIODevice::WriteOnly));
      las.write("~V\nVERS. 2.0 : version\nWRAP. NO : wrap\n~W\nNULL. -999.25 : "
                "null\n~C\nDEPT.M : depth\nGR.API : gamma\n~A\n1000 30\n1030 "
                "50\n1060 90\n1090 45\n1120 20\n");
      las.close();
      QVERIFY(registrar.commit(wellStage, {}, "test", {}));
      CatalogEntity well;
      well.id = "review-well";
      well.entityType = "well";
      well.name = "修订测试井";
      well.hasSurface = true;
      well.surfaceX = 200;
      well.surfaceY = 200;
      QVERIFY(catalog->addEntity(well));
      EntityAssetLink wl;
      wl.entityType = "well";
      wl.entityId = well.id;
      wl.assetId = wellStage.assetId;
      wl.role = "well_log";
      QVERIFY(catalog->addLink(wl));
      m_win->showPage("predict");
      auto *kind = page->findChild<QComboBox *>("predictionKind");
      kind->setCurrentIndex(kind->findData("wells"));
      QCOMPARE(inputs->count(), 1);
      inputs->item(0)->setCheckState(Qt::Checked);
      run->trigger();
      QTRY_VERIFY(!m_ctx->mappingWorkbench()->busy());
      auto *wellPanel = m_win->findChild<WellPredictionPanel *>();
      QVERIFY(wellPanel);
      QTRY_VERIFY(wellPanel->isVisible());
      QCOMPARE(wellPanel->findChild<QTableWidget *>("predictionIntervals")
                   ->rowCount(),
               12);
      QVERIFY(!wellPanel->layerId().startsWith("draft."));
      // 直接在预测相上修订：首次应用自动保留预测原件并切换到修订副本。
      auto *choice = wellPanel->findChild<QComboBox *>("wellFaciesChoice");
      choice->setCurrentIndex((choice->currentIndex() + 1) % choice->count());
      wellPanel->findChild<QPushButton *>("applyWellFacies")->click();
      QVERIFY(wellPanel->layerId().startsWith("draft."));
      const auto wellDraft = wellPanel->layerId();
      auto beforeRevision =
          m_ctx->mappingWorkbench()->versionForLayer(wellDraft);
      QVERIFY(editing->isEditing());
      wellPanel->findChild<QPushButton *>("saveWellPrediction")->click();
      QVERIFY(!editing->isEditing());
      QCOMPARE(
          m_ctx->mappingWorkbench()->versionForLayer(wellDraft).versionNumber,
          beforeRevision.versionNumber + 1);
      // Right-click menu routes the hit feature to the same facies workflow.
      auto *map = m_ctx->canvasCtl()->canvas();
      auto *wellVector = qobject_cast<QgsVectorLayer *>(map->currentLayer());
      QVERIFY(wellVector);
      auto wellFeatures = wellVector->getFeatures();
      QgsFeature wf;
      QVERIFY(wellFeatures.nextFeature(wf));
      const auto pixel =
          map->mapSettings().mapToPixel().transform(wf.geometry().asPoint());
      const QPoint pos(qRound(pixel.x()), qRound(pixel.y()));
      QMouseEvent mouse(QEvent::MouseButtonPress, QPointF(pos),
                        QPointF(map->mapToGlobal(pos)), Qt::RightButton,
                        Qt::RightButton, Qt::NoModifier);
      QgsMapMouseEvent event(map, &mouse);
      QMenu menu;
      emit map->contextMenuAboutToShow(&menu, &event);
      auto *change = menu.findChild<QMenu *>("changeFeatureFacies");
      QVERIFY(change);
      QCOMPARE(change->actions().size(), 3);
      change->actions().first()->trigger();
      QVERIFY(editing->isEditing());
      QVERIFY(map->currentLayer()
                  ->customProperty("paleoLayerId")
                  .toString()
                  .startsWith("draft."));
      editing->actionSave()->trigger();
      QVERIFY(!editing->isEditing());
      QTest::qWait(150);
      if (const auto capture =
              qEnvironmentVariable("PALEO_PREDICTION_WINDOW_CAPTURE");
          !capture.isEmpty())
        QVERIFY(m_win->grab().save(capture));
    }

};

int main(int argc, char *argv[])
{
  // Widgets need a platform; force offscreen before AppContext constructs the
  // QgsApplication inside its ctor (env must be set before QApplication).
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");

  // 每运行一次的临时目录（对齐 tst_seismic_sectionui 惯例）：既隔离直跑时
  // 的真实用户配置，也消除固定 /tmp 路径跨运行/跨用户的陈旧状态向量
  // （ctest 路径另有 add_paleo_test 的 XDG/HOME 沙箱兜底）。
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());

  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");

  TestUiShell tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_ui.moc"
