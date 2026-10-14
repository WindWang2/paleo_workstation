// goal7/ui-shell：主窗 chrome（dock 策略 / ribbon / 帮助）真实壳回归。
//   - 页面 dock 策略（数据页藏图层树、编图页按用户意愿恢复）必须在保存/恢复/
//     重置布局之后保持成立：布局命令不得把「页面策略隐藏」当成「用户想藏」。
// 每个用例自建窗口 + 自带画布控制器（同 tst_ui windowStateAndExtentPersist 惯例），
// 彼此不共享 dock/QSettings 状态。
#include <QtTest>

#include <QAction>
#include <QDockWidget>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>

#include "../src/app/appcontext.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/ui/paleodockmanager.h"
#include "../src/ui/paleomainwindow.h"

class TestShellChrome : public QObject
{
  Q_OBJECT
public:
  explicit TestShellChrome(AppContext *ctx) : m_ctx(ctx) {}

private:
  AppContext *m_ctx;

  static void clearSettings()
  {
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
  }

  static QDockWidget *dock(PaleoMainWindow &w, const char *name)
  {
    return w.findChild<QDockWidget *>(QString::fromLatin1(name));
  }

  // 「布局与面板」菜单里按文案取动作（与用户点击同一条路径）。
  static QAction *layoutAction(PaleoMainWindow &w, const QString &text, QScopedPointer<QMenu> &menu)
  {
    auto *manager = w.findChild<PaleoDockManager *>();
    menu.reset(manager ? manager->createMenu() : nullptr);
    if (!menu)
      return nullptr;
    for (QAction *a : menu->actions())
      if (a->text() == text)
        return a;
    return nullptr;
  }

private slots:
  void initTestCase() { QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime"); }
  void init() { clearSettings(); }
  void cleanup() { clearSettings(); }

  // 基线（非回归场景）：纯切页时左 dock 随页策略显隐，且不丢「用户想看」。
  void pagePolicyHidesLayerTreeOnDataOnly()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    QDockWidget *left = dock(win, "layerTreeDock");
    QVERIFY(left);
    win.showPage(QStringLiteral("predict"));
    QVERIFY(!left->isHidden());
    win.showPage(QStringLiteral("data"));
    QVERIFY(left->isHidden());
    win.showPage(QStringLiteral("predict"));
    QVERIFY(!left->isHidden());
  }

  // R-1：在数据页退出（左 dock 被页策略藏起）→ 下次启动恢复窗口状态 → 切到编图页，
  // 图层树必须照常出现。
  void exitOnDataPageDoesNotPinLayerTreeHidden()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      win.showPage(QStringLiteral("data"));
      QVERIFY(dock(win, "layerTreeDock")->isHidden());
      win.saveWindowState();
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree dock stayed hidden on a mapping page after restoring a window state "
             "saved while the data page had hidden it by page policy");
  }

  // R-2：在数据页点「恢复默认布局」——数据页纯三栏策略（图层树藏、数据列表显）不得被破坏。
  void resetLayoutOnDataPageKeepsPagePolicy()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("data"));
    QScopedPointer<QMenu> menu;
    QAction *reset = layoutAction(win, tr("恢复默认布局"), menu);
    QVERIFY(reset);
    reset->trigger();
    QTest::qWait(20);
    QVERIFY2(dock(win, "layerTreeDock")->isHidden(), "layer tree reappeared on the data page");
    QVERIFY2(!dock(win, "dataListDock")->isHidden(), "data list vanished on the data page");
  }

  // R-3：在编图页重置布局后来回切页，图层树仍按页策略出现。
  void resetLayoutThenPageRoundTripKeepsLayerTree()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    QScopedPointer<QMenu> menu;
    QAction *reset = layoutAction(win, tr("恢复默认布局"), menu);
    QVERIFY(reset);
    reset->trigger();
    QTest::qWait(20);
    win.showPage(QStringLiteral("data"));
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree did not come back on a mapping page after reset + page round trip");
  }

  // R-4：数据页「保存当前布局」→ 编图页「恢复保存的布局」→ 来回切页，图层树仍出现。
  void restoreSavedLayoutSavedOnDataPageKeepsLayerTree()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("data"));
    QScopedPointer<QMenu> menu;
    QAction *save = layoutAction(win, tr("保存当前布局"), menu);
    QVERIFY(save);
    save->trigger();
    win.showPage(QStringLiteral("predict"));
    QAction *restore = layoutAction(win, tr("恢复保存的布局"), menu);
    QVERIFY(restore && restore->isEnabled());
    restore->trigger();
    QTest::qWait(20);
    win.showPage(QStringLiteral("data"));
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree stayed hidden on a mapping page after restoring a layout saved on the data page");
  }

  // 用户主动藏图层树（与页策略无关）：意图跨会话保留——恢复后切到编图页也不重显。
  void userHiddenLayerTreeStaysHiddenAcrossSessions()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      win.showPage(QStringLiteral("predict"));
      auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
      QVERIFY(left);
      left->hide(); // 用户手势：非程序化 setVisible
      QVERIFY(!left->userWantsVisible());
      win.saveWindowState();
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
    QVERIFY(left);
    QVERIFY2(left->isHidden(), "user-hidden layer tree reappeared after session restore");
    QVERIFY2(!left->userWantsVisible(), "user-hide intent was lost by the restore");
  }

  // 旧版设置（只有 windowState/windowGeometry、没有 windowDockIntent）：照常恢复不崩，
  // 图层树意愿保持默认 true，编图页照常显示。
  void legacySettingsWithoutIntentStillRestore()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
      s.setValue(QStringLiteral("windowState"), win.saveState());
      s.setValue(QStringLiteral("windowGeometry"), win.saveGeometry());
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
    QVERIFY(left);
    QVERIFY2(left->userWantsVisible(), "legacy restore clobbered the default show intent");
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!left->isHidden(), "layer tree missing on a mapping page after legacy restore");
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestShellChrome tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_shell_chrome.moc"
