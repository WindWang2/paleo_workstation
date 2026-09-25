#include <QtTest>
#include <QTemporaryDir>
#include <QTabBar>
#include <QStackedWidget>
#include <QStackedLayout>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>

#include "../src/app/appcontext.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisprocessingservice.h"

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
