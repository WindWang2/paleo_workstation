#include <QtTest>
#include <QTemporaryDir>
#include <QLabel>
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

#include "../src/app/appcontext.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/io/dataimportservice.h" // 测试可直触 io（断言 DataImportService 信号）
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/metadata/layermanifest.h"
#include <QTimer>

#include <qgsproject.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>

#include <qgslayertreeview.h>
#include <qgslayertree.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgslayertreemodel.h>
#include <qgsmaptoolpan.h>

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

    // Processing entry point: attachWorkflows with the processing service adds
    // a top-bar "处理算法" button whose menu surfaces the paleo:* algorithms
    // (full-registry ids live in per-provider submenus).
    void processingButtonSurfacesAlgorithms()
    {
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store());

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

      QStringList flat;
      const auto walk = [&flat](QMenu *menu, auto &&self) -> void {
        for (QAction *a : menu->actions())
        {
          if (a->menu())
            self(a->menu(), self);
          else
            flat << a->text();
        }
      };
      walk(btn->menu(), walk);

      QVERIFY(flat.contains(QStringLiteral("paleo:paleo_constraint_idw")));
      QVERIFY(flat.contains(QStringLiteral("paleo:paleo_facies_fusion")));
      QVERIFY(flat.contains(QStringLiteral("paleo:paleo_geological_smoothing")));

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

      PaleoMainWindow win2(m_ctx->canvasCtl(), m_ctx->projectSvc(),
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

    // §4 预览壳重排：中央工作区在数据页是「数据列表在左、预览在右」的横向分栏；预览
    // 只在数据管理页可见，其余四页隐藏；底栏不再挂地震预览标签（连井
    // 剖面面板保留），状态栏标工程网格坐标系。
    void previewSplitterShell()
    {
      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("dataListPreviewSplit"));
      QVERIFY(split);
      QCOMPARE(split->orientation(), Qt::Horizontal);
      QVERIFY(split->count() >= 1);
      auto *preview = m_win->findChild<QWidget *>(QStringLiteral("dataPreview"));
      QVERIFY(preview);
      QCOMPARE(split->indexOf(preview), split->count() - 1); // 预览总是分栏最后一格
      // 预览部件只有这一个（从右 dock 挪出后没有第二处宿主）。
      QCOMPARE(m_win->findChildren<QWidget *>(QStringLiteral("dataPreview")).size(), 1);

      // attachWorkflows 已在前面的用例跑过：底栏不能再有地震预览。
      QVERIFY(!m_win->findChild<QWidget *>(QStringLiteral("seismicPreviewPanel")));
      QVERIFY(m_win->findChild<QWidget *>(QStringLiteral("correlationPanel")));

      // 预览可见性跟页走：isHidden() 反映显式隐藏标记（offscreen 窗口
      // 可能没 show，isVisible 受祖先链影响不可用）。
      m_win->showPage(QStringLiteral("data"));
      QVERIFY(!preview->isHidden());
      for (const QString &p : {QStringLiteral("predict"), QStringLiteral("constraint"),
                               QStringLiteral("compose"), QStringLiteral("validate")})
      {
        m_win->showPage(p);
        QVERIFY2(preview->isHidden(), qPrintable(p));
      }
      m_win->showPage(QStringLiteral("data"));
      QVERIFY(!preview->isHidden());

      // 状态栏工程坐标系标注（T22：与 PDF 页脚同一句「工程坐标 · 米 · 未投影」）。
      auto *crs = m_win->findChild<QLabel *>(QStringLiteral("statusCrs"));
      QVERIFY(crs);
      QCOMPARE(crs->text(), QStringLiteral("工程坐标 · 米 · 未投影"));
    }

    // D7：预览高度/宽度预算——首个标签给 ≥60%；角落「最大化预览」把数据列表压到
    // ≤64px 壳，「还原」回用户尺寸；空态（关全部标签）复位最大化与预算。
    void previewSplitterBudgetAndMaximize()
    {
      // 自给自足：分栏尺寸断言要求窗口已布局（单跑本用例时前面的用例不会先 show）。
      m_win->resize(1280, 1100);
      m_win->show();
      // 无工程时 showPage 不离开启动页——布局断言需要工作区页为当前页。
      if (auto *centerStack =
              m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack")))
        centerStack->setCurrentIndex(1);
      m_win->showPage(QStringLiteral("data"));
      QTest::qWait(30);
      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("dataListPreviewSplit"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      QVERIFY(split && preview);
      auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
      auto *maxBtn = preview->findChild<QToolButton *>(QStringLiteral("previewMaxButton"));
      QVERIFY(inner && maxBtn);
      QVERIFY(maxBtn->isCheckable());
      QCOMPARE(maxBtn->text(), QStringLiteral("最大化预览"));

      if (split->indexOf(preview) == 0)
        split->insertWidget(0, new QWidget);
      QCOMPARE(split->indexOf(preview), 1);

      // 测试壳未开工程（importSvc==nullptr → openAsset 不建页）；直接给
      // 内层 tabWidget 加页——currentChanged 0→1 同样驱动 applyPreviewSplit。
      inner->addTab(new QLabel(QStringLiteral("x")), QStringLiteral("t"));
      QCOMPARE(inner->count(), 1);
      QTest::qWait(20); // applyPreviewSplit 借 currentChanged 后事件链
      const int total = split->sizes().at(0) + split->sizes().at(1);
      QVERIFY2(total > 0, "split not laid out");
      QVERIFY2(split->sizes().at(1) >= total * 3 / 5 - 2,
               qPrintable(QStringLiteral("preview %1 of %2")
                              .arg(split->sizes().at(1)).arg(total)));

      // 最大化：预览 ≥75%（数据列表被压到自身最小尺寸壳），按钮文案翻面。
      const int mapBudget = split->sizes().at(0);
      maxBtn->setChecked(true);
      QTest::qWait(20);
      QVERIFY(split->sizes().at(0) < mapBudget);
      const int floor = (split->orientation() == Qt::Horizontal)
                            ? split->widget(0)->minimumSizeHint().width()
                            : split->widget(0)->minimumSizeHint().height();
      QVERIFY(split->sizes().at(0) <= qMax(160, floor));
      QVERIFY(split->sizes().at(1) >= total - qMax(160, floor) - 2);
      QCOMPARE(maxBtn->text(), QStringLiteral("还原预览"));

      // 还原：预览回到 ~60%（还原的是最大化前的预算尺寸）。
      maxBtn->setChecked(false);
      QTest::qWait(20);
      QVERIFY(split->sizes().at(1) >= total * 3 / 5 - 2);
      QVERIFY(split->sizes().at(0) > 64);

      // 关掉最后一个标签 → 空态收成一行（竖向）或展示空态底纹（横向），最大化态复位。
      inner->removeTab(0);
      QTest::qWait(20);
      QCOMPARE(inner->count(), 0);
      QVERIFY(!maxBtn->isChecked());
      if (split->orientation() == Qt::Vertical)
        QVERIFY(split->sizes().at(1) <= qMax(28, preview->sizeHint().height() + 8));
      else
        QVERIFY(split->sizes().at(1) > 0);
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

      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("dataListPreviewSplit"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      QVERIFY(split && preview);
      auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
      QVERIFY(inner);

      while (inner->count() > 0)
        inner->removeTab(0);
      QTest::qWait(20);

      // 1. 用户拖拽分栏调整列表宽度为 275px
      const int customWidth = 275;
      emit split->splitterMoved(customWidth, 1);
      split->setSizes({customWidth, split->width() - customWidth});
      QTest::qWait(20);
      QCOMPARE(split->sizes().at(0), customWidth);

      // 2. 双击/激活数据项，打开首个预览标签：数据列表宽度绝不得改变
      inner->addTab(new QLabel(QStringLiteral("item1")), QStringLiteral("Tab1"));
      QTest::qWait(20);
      QCOMPARE(split->sizes().at(0), customWidth);

      // 3. 打开第二个预览标签 / 切换标签：数据列表宽度绝不得改变
      inner->addTab(new QLabel(QStringLiteral("item2")), QStringLiteral("Tab2"));
      inner->setCurrentIndex(1);
      QTest::qWait(20);
      QCOMPARE(split->sizes().at(0), customWidth);

      // 4. 关闭所有标签回到空态：数据列表宽度绝不得改变
      inner->removeTab(1);
      inner->removeTab(0);
      QTest::qWait(20);
      QCOMPARE(split->sizes().at(0), customWidth);

      // 5. 再次打开标签：数据列表宽度依然保持用户设定值
      inner->addTab(new QLabel(QStringLiteral("item3")), QStringLiteral("Tab3"));
      QTest::qWait(20);
      QCOMPARE(split->sizes().at(0), customWidth);

      // 清理
      inner->removeTab(0);
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
                               "statusCatalogError"})
        QCOMPARE(namedCount(name), 1);
      auto *host = m_win->findChild<QWidget *>(QStringLiteral("rightPanelHost"));
      QVERIFY(host);
      QCOMPARE(static_cast<QStackedLayout *>(host->layout())->count(), 5);

      attachAll(); // 二次调用：必须早退——不翻倍、不叠加连接、不崩。

      QCOMPARE(m_win->findChildren<QDockWidget *>().size(), docksAfterFirst);
      for (const char *name : {"correlationPanel", "editingToolbar",
                               "releasePanel", "attributeTablePanel", "processingButton",
                               "statusCatalogError"})
        QCOMPARE(namedCount(name), 1);
      QCOMPARE(static_cast<QStackedLayout *>(host->layout())->count(), 5);

      // 不崩的直接证据：二次调用后窗口照常响应页切换。
      m_win->showPage(QStringLiteral("data"));
      QCOMPARE(m_win->currentPage(), QStringLiteral("data"));
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

    // 数据页是列表面（用户裁决）：中央工作区切到数据面（数据列表 + 预览），
    // 编图链四页切到画布面（层位 chips + 画布）。预览分栏显隐沿用旧约（预览只在数据页）。
    void dataPageHidesCanvasForLists()
    {
      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("dataListPreviewSplit"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      auto *workspaceStack = m_win->findChild<QStackedWidget *>(QStringLiteral("workspaceStack"));
      QVERIFY(split && preview && workspaceStack);

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
      QCOMPARE(btn->text(), QStringLiteral("面板"));

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
};

int main(int argc, char *argv[])
{
  // Widgets need a platform; force offscreen before AppContext constructs the
  // QgsApplication inside its ctor (env must be set before QApplication).
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");

  // Keep QSettings writes out of the real user profile.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QDir::temp().filePath(QStringLiteral("paleo_tst_ui_settings")));

  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");

  TestUiShell tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_ui.moc"
