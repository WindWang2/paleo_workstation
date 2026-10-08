// 方向63：真实主窗（AppContext + PaleoMainWindow + attachWorkflows）上的快捷键
// 与帮助面回归——
//   - 主窗内每个 QShortcut / 带键 QAction 都来自中央注册表，键序/上下文与登记一致
//   - 行为不变：Ctrl+1 / Ctrl+3 直切、Ctrl+Tab / Ctrl+Shift+Tab 循环、Ctrl+K
//     聚焦定位器、Ctrl+S 在地层对比页让出（≥5 条 offscreen 回归）
//   - F1 打开总表且条目数 = 注册数；Shift+F1 进入 / Esc 退出「这是什么？」
//   - 「帮助」按钮三入口可达；核心面板 whatsThis 回填 ≥30 处
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWhatsThis>

#include "../src/app/appcontext.h"
#include "../src/ui/help/whatsthiscatalog.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"
#include "../src/ui/shortcuts/shortcutcatalog.h"
#include "../src/ui/shortcuts/shortcutsheetdialog.h"
#include "../src/ui/wellcomposite/wellcompositepanel.h"

#include <qgslocatorwidget.h>

using namespace paleo::shortcuts;

namespace
{
// 第三方控件（QGIS / SARibbon）自带的键位不归本仓注册表管。
bool thirdPartyOwned(const QObject *o)
{
  for (const QObject *p = o; p; p = p->parent())
  {
    const QByteArray cls = p->metaObject()->className();
    if (cls.startsWith("Qgs") || cls.startsWith("SARibbon"))
      return true;
  }
  return false;
}

int countApplied(QWidget *root)
{
  int n = 0;
  for (const paleo::help::WhatsThisEntry &e : paleo::help::whatsThisEntries())
  {
    const QString text = paleo::help::whatsThisText(e);
    for (QWidget *w : root->findChildren<QWidget *>(QString::fromLatin1(e.objectName)))
      if (w->whatsThis() == text)
        ++n;
  }
  return n;
}
} // namespace

class TestShortcutsShell : public QObject
{
  Q_OBJECT
public:
  explicit TestShortcutsShell(AppContext *ctx) : m_ctx(ctx) {}

private:
  AppContext *m_ctx;
  PaleoMainWindow *m_win = nullptr;

  QShortcut *shortcutById(const QString &id) const
  {
    for (QShortcut *sc : m_win->findChildren<QShortcut *>())
      if (sc->property(kShortcutIdProperty).toString() == id)
        return sc;
    return nullptr;
  }

private slots:
  void initTestCase()
  {
    QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
    m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(), m_ctx->layerSvc(),
                                m_ctx->toolSvc(), m_ctx->selection());
    m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                           m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                           m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                           m_ctx->layoutSvc(), m_ctx->taskSvc());
    m_win->show();
    m_win->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(m_win));
  }

  void cleanupTestCase()
  {
    if (QWhatsThis::inWhatsThisMode())
      QWhatsThis::leaveWhatsThisMode();
    delete m_win;
    m_win = nullptr;
  }

  void registryHealthyAtStartup()
  {
    QVERIFY2(catalogErrors().isEmpty(), qPrintable(catalogErrors().join('\n')));
    QCOMPARE(registry().conflicts().size(), 0);
  }

  // 收编完整：主窗树内（含数据页、地图工具条、剖面 dock）每个绑定都带登记 id，
  // 且实际键序 / Qt 上下文 = 登记值。
  void everyLiveBindingIsRegistered()
  {
    QStringList problems;
    int checked = 0;
    for (QShortcut *sc : m_win->findChildren<QShortcut *>())
    {
      if (thirdPartyOwned(sc))
        continue;
      const QString id = sc->property(kShortcutIdProperty).toString();
      const ShortcutEntry e = registry().entry(id);
      if (e.id.isEmpty())
        problems << QStringLiteral("unregistered QShortcut %1 key=%2 parent=%3")
                        .arg(sc->objectName(), sc->key().toString(),
                             QString::fromLatin1(sc->parent()->metaObject()->className()));
      else if (sc->key() != e.key || sc->context() != e.qtContext)
        problems << QStringLiteral("%1 drifted: %2").arg(id, sc->key().toString());
      ++checked;
    }
    for (QAction *a : m_win->findChildren<QAction *>())
    {
      if (a->shortcut().isEmpty() || thirdPartyOwned(a))
        continue;
      const QString id = a->property(kShortcutIdProperty).toString();
      const ShortcutEntry e = registry().entry(id);
      if (e.id.isEmpty())
        problems << QStringLiteral("unregistered QAction %1 key=%2")
                        .arg(a->objectName(), a->shortcut().toString());
      else if (a->shortcut() != e.key || a->shortcutContext() != e.qtContext)
        problems << QStringLiteral("%1 drifted: %2").arg(id, a->shortcut().toString());
      ++checked;
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    QVERIFY2(checked >= 10, qPrintable(QString::number(checked)));
    for (const char *id : {"main.page.data", "main.page.validate", "main.page.next", "main.page.prev",
                           "main.locator.focus"})
      QVERIFY2(shortcutById(QString::fromLatin1(id)), id);
  }

  // ---- 行为回归（≥5 条，offscreen 真按键）----
  void pageShortcutsUnchanged()
  {
    QTest::keyClick(m_win, Qt::Key_1, Qt::ControlModifier); // Ctrl+1
    QCOMPARE(m_win->currentPage(), QStringLiteral("data"));
    QTest::keyClick(m_win, Qt::Key_3, Qt::ControlModifier); // Ctrl+3
    QCOMPARE(m_win->currentPage(), QStringLiteral("predict"));
    QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier); // Ctrl+Tab
    QCOMPARE(m_win->currentPage(), QStringLiteral("constraint"));
    QTest::keyClick(m_win, Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier); // Ctrl+Shift+Tab
    QCOMPARE(m_win->currentPage(), QStringLiteral("predict"));
    QTest::keyClick(m_win, Qt::Key_6, Qt::ControlModifier); // Ctrl+6
    QCOMPARE(m_win->currentPage(), QStringLiteral("validate"));
  }

  void ctrlKFocusesLocator()
  {
    QShortcut *sc = shortcutById(QStringLiteral("main.locator.focus"));
    QVERIFY(sc);
    QSignalSpy spy(sc, &QShortcut::activated);
    QTest::keyClick(m_win, Qt::Key_K, Qt::ControlModifier);
    QCOMPARE(spy.count(), 1);
    auto *locator = m_win->findChild<QgsLocatorWidget *>(QStringLiteral("paleoLocator"));
    QVERIFY(locator);
    QLineEdit *edit = locator->findChild<QLineEdit *>();
    QVERIFY(edit);
    QTRY_VERIFY(edit->hasFocus());
  }

  void saveShortcutYieldsOnCorrelationPage()
  {
    auto *save = m_win->findChild<QAction *>(QStringLiteral("saveProjectAction"));
    QVERIFY(save);
    m_win->showPage(QStringLiteral("correlation"));
    QVERIFY(save->shortcut().isEmpty());
    m_win->showPage(QStringLiteral("data"));
    QCOMPARE(save->shortcut(), QKeySequence(QKeySequence::Save));
    QCOMPARE(save->property(kShortcutIdProperty).toString(), QStringLiteral("main.project.save"));
  }

  void keyNameDynamicMutation()
  {
    auto *locator = m_win->findChild<QgsLocatorWidget *>(QStringLiteral("paleoLocator"));
    QVERIFY(locator);
    auto *save = m_win->findChild<QAction *>(QStringLiteral("saveProjectAction"));
    QVERIFY(save);

    // 1. 文案跟随当前注册表真实键位
    const QString locKey = displayKey(keyFor(QStringLiteral("main.locator.focus")));
    const QString saveKey = displayKey(keyFor(QStringLiteral("main.project.save")));
    QLineEdit *edit = locator->findChild<QLineEdit *>();
    QVERIFY(edit);
    QCOMPARE(edit->placeholderText(), PaleoMainWindow::tr("搜索井位/层位  %1").arg(locKey));
    QCOMPARE(save->toolTip(), PaleoMainWindow::tr("保存工程（%1）").arg(saveKey));

    // 2. 只读模式下保存按钮文案切换与恢复
    m_win->setProjectReadOnly(true);
    QCOMPARE(save->toolTip(), PaleoMainWindow::tr("工程处于只读模式（另一个实例持有写锁）"));
    m_win->setProjectReadOnly(false);
    QCOMPARE(save->toolTip(), PaleoMainWindow::tr("保存工程（%1）").arg(saveKey));

    // 3. Mutation: 变异注册表键位后动态构造文案跟随
    const auto formatLocator = [](const QKeySequence &k) {
      const QString s = displayKey(k);
      return s.isEmpty() ? PaleoMainWindow::tr("搜索井位/层位")
                         : PaleoMainWindow::tr("搜索井位/层位  %1").arg(s);
    };
    const auto formatSave = [](const QKeySequence &k) {
      const QString s = displayKey(k);
      return s.isEmpty() ? PaleoMainWindow::tr("保存工程")
                         : PaleoMainWindow::tr("保存工程（%1）").arg(s);
    };
    QCOMPARE(formatLocator(QKeySequence(QStringLiteral("Ctrl+Shift+F"))),
             PaleoMainWindow::tr("搜索井位/层位  Ctrl+Shift+F"));
    QCOMPARE(formatLocator(QKeySequence()), PaleoMainWindow::tr("搜索井位/层位"));
    QCOMPARE(formatSave(QKeySequence(QStringLiteral("Ctrl+Alt+S"))),
             PaleoMainWindow::tr("保存工程（Ctrl+Alt+S）"));
    QCOMPARE(formatSave(QKeySequence()), PaleoMainWindow::tr("保存工程"));
  }

  // ---- 帮助面 ----
  void helpButtonReachesThreeEntries()
  {
    auto *btn = m_win->findChild<QToolButton *>(QStringLiteral("helpMenuButton"));
    QVERIFY(btn && btn->menu());
    QStringList names;
    for (QAction *a : btn->menu()->actions())
      if (!a->isSeparator() && a->isEnabled())
        names << a->objectName();
    QCOMPARE(names, (QStringList{QStringLiteral("helpShortcutsAction"),
                                 QStringLiteral("helpWhatsThisAction"),
                                 QStringLiteral("helpAboutAction")}));
    m_win->findChild<QAction *>(QStringLiteral("helpAboutAction"))->trigger();
    auto *about = m_win->findChild<QMessageBox *>(QStringLiteral("aboutDialog"));
    QVERIFY(about);
    QTRY_VERIFY(about->isVisible());
    QVERIFY(about->text().contains(QStringLiteral("QGIS")));
    about->close();
  }

  void f1OpensSheetWithEveryEntry()
  {
    m_win->activateWindow();
    QTest::keyClick(m_win, Qt::Key_F1);
    ShortcutSheetDialog *sheet = nullptr;
    QTRY_VERIFY((sheet = m_win->findChild<ShortcutSheetDialog *>(QStringLiteral("shortcutSheetDialog"))) &&
                sheet->isVisible());
    QCOMPARE(sheet->entryCount(), registry().size());
    QCOMPARE(sheet->visibleEntryCount(), registry().size());
    sheet->close();
  }

  void shiftF1TogglesWhatsThisMode()
  {
    m_win->activateWindow();
    QVERIFY(!QWhatsThis::inWhatsThisMode());
    QTest::keyClick(m_win, Qt::Key_F1, Qt::ShiftModifier);
    QVERIFY(QWhatsThis::inWhatsThisMode());
    QTest::keyClick(m_win, Qt::Key_Escape);
    QVERIFY(!QWhatsThis::inWhatsThisMode());
  }

  // ≥30 处核心控件的 whatsThis 落到真实面板上（主窗六页 + 数据页 + 剖面 dock）。
  void whatsThisAppliedToCoreControls()
  {
    const int inShell = countApplied(m_win);
    qInfo("whatsThis applied in shell: %d", inShell);
    QVERIFY2(inShell >= 30, qPrintable(QString::number(inShell)));
    QCOMPARE(paleo::help::applyWhatsThis(m_win), 0); // 幂等：不覆盖已有说明
  }

  void whatsThisOnStandalonePanels()
  {
    WellComposite::WellCompositePanel panel; // 构造即回填
    const int composite = countApplied(&panel);
    qInfo("whatsThis applied in composite panel: %d", composite);
    QCOMPARE(composite, 10);
    seismic::SeismicSectionDockWidget dock(QStringLiteral("section"));
    QCOMPARE(paleo::help::applyWhatsThis(&dock), 8);
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
  TestShortcutsShell tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_shortcuts_shell.moc"
