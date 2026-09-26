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
#include <QToolButton>
#include <QDockWidget>

#include "../src/app/appcontext.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgiscanvascontroller.h"

#include <qgsproject.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>

#include <qgslayertreeview.h>
#include <qgslayertreemodel.h>

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

    // §42 workflow chain: 数据管理/①预测/②约束/③编图/④验证
    void workflowTabBarHasFiveTabs()
    {
      auto *tabs = m_win->findChild<QTabBar *>(QStringLiteral("workflowTabs"));
      QVERIFY(tabs);
      QCOMPARE(tabs->count(), 5);
      QCOMPARE(tabs->tabText(0), QStringLiteral("数据管理"));
      QCOMPARE(tabs->tabData(0).toString(), QStringLiteral("data"));
      QCOMPARE(tabs->tabData(4).toString(), QStringLiteral("validate"));

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

    // §4 预览壳重排：中央工作区是「地图在上、预览在下」的竖向分栏；预览
    // 只在数据管理页可见，其余四页隐藏；底栏不再挂地震预览标签（连井
    // 剖面面板保留），状态栏标工程网格坐标系。
    void previewSplitterShell()
    {
      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("mapPreviewSplit"));
      QVERIFY(split);
      QCOMPARE(split->orientation(), Qt::Vertical);
      QVERIFY(split->count() >= 1); // 画布可能被前面用例的 win2 借走——结构断言不依赖它
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

    // D7：预览高度预算——首个标签给 ≥60%；角落「最大化预览」把地图压到
    // ≤64px 壳，「还原」回用户尺寸；空态（关全部标签）复位最大化与预算。
    void previewSplitterBudgetAndMaximize()
    {
      // 自给自足：分栏尺寸断言要求窗口已布局（单跑本用例时前面的用例不会先 show）。
      m_win->resize(1280, 860);
      m_win->show();
      // 无工程时 showPage 不离开启动页——布局断言需要工作区页为当前页。
      if (auto *centerStack =
              m_win->findChild<QStackedWidget *>(QStringLiteral("centerStack")))
        centerStack->setCurrentIndex(1);
      m_win->showPage(QStringLiteral("data"));
      QTest::qWait(30);
      auto *split = m_win->findChild<QSplitter *>(QStringLiteral("mapPreviewSplit"));
      auto *preview = m_win->findChild<DataPreviewTabs *>(QStringLiteral("dataPreview"));
      QVERIFY(split && preview);
      auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
      auto *maxBtn = preview->findChild<QToolButton *>(QStringLiteral("previewMaxButton"));
      QVERIFY(inner && maxBtn);
      QVERIFY(maxBtn->isCheckable());
      QCOMPARE(maxBtn->text(), QStringLiteral("最大化预览"));

      // 画布可能被前面的用例借走——预览不是分栏第一格时尺寸才有意义；
      // 缺上格就补个占位件（splitter 按位置分尺寸，与画布无关）。
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

      // 最大化：预览 ≥80%（地图被压到自身最小高度壳——QgsMapCanvas
      // minimumSizeHint≈70px 会比 64 预算略高），按钮文案翻面。
      const int mapBudget = split->sizes().at(0);
      maxBtn->setChecked(true);
      QTest::qWait(20);
      QVERIFY(split->sizes().at(1) >= total * 4 / 5 - 2);
      QVERIFY(split->sizes().at(0) < mapBudget);
      QVERIFY(split->sizes().at(0) <=
              qMax(70, split->widget(0)->minimumSizeHint().height()));
      QCOMPARE(maxBtn->text(), QStringLiteral("还原预览"));

      // 还原：预览回到 ~60%（还原的是最大化前的预算尺寸）。
      maxBtn->setChecked(false);
      QTest::qWait(20);
      QVERIFY(split->sizes().at(1) >= total * 3 / 5 - 2);
      QVERIFY(split->sizes().at(0) > 64);

      // 关掉最后一个标签 → 空态收成一行，最大化态复位。
      inner->removeTab(0);
      QTest::qWait(20);
      QCOMPARE(inner->count(), 0);
      QVERIFY(!maxBtn->isChecked());
      QVERIFY(split->sizes().at(1) <= qMax(28, preview->sizeHint().height() + 8));
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
