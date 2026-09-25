#include <QtTest>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QToolButton>

#include "../src/ui/webviewpanel.h"
#include "../src/ui/paleomainwindow.h"

// WebViewPanel contract under offscreen: the engine never initializes on a
// headless platform, setUrl() reports failure honestly, the panel degrades
// to its fallback surface (status text + system-browser escape hatch) and
// the host app is never taken down by the web component.
class TestWebViewPanel : public QObject
{
  Q_OBJECT

private slots:
  void engineStaysAbsentOnOffscreen()
  {
    WebViewPanel panel;
    QVERIFY(!panel.engineAvailable()); // lazy — nothing built before first use

    QSignalSpy failSpy(&panel, &WebViewPanel::loadFailed);
    QVERIFY(!panel.setUrl(QUrl(QStringLiteral("https://svc.internal/map"))));
    QVERIFY(panel.engineAvailable() == false);
    QVERIFY(!panel.lastError().isEmpty());
    QCOMPARE(failSpy.count(), 1);

    // Fallback surface shows reason + external-open escape hatch.
    auto *label = panel.findChild<QLabel *>(QStringLiteral("webStatus"));
    QVERIFY(label);
    QVERIFY(label->text().contains(QStringLiteral("不可用")));
    auto *btn = panel.findChild<QPushButton *>(QStringLiteral("webExternalButton"));
    QVERIFY(btn);
    QVERIFY(btn->isVisibleTo(&panel));
  }

  void invalidUrlFailsWithoutEngine()
  {
    WebViewPanel panel;
    QSignalSpy failSpy(&panel, &WebViewPanel::loadFailed);
    QVERIFY(!panel.setUrl(QUrl()));
    QVERIFY(!panel.engineAvailable());
    QCOMPARE(failSpy.count(), 1);
  }

  void emptyStateBeforeAnyUrl()
  {
    WebViewPanel panel;
    auto *label = panel.findChild<QLabel *>(QStringLiteral("webStatus"));
    QVERIFY(label);
    QVERIFY(!label->text().isEmpty());
    auto *btn = panel.findChild<QPushButton *>(QStringLiteral("webExternalButton"));
    QVERIFY(btn && !btn->isVisibleTo(&panel)); // nothing to open yet
  }

  // ---- host wiring in PaleoMainWindow (goal/webui-host) ----

  void hostDockConstructsOffscreen()
  {
    // No persisted window state leaking in from earlier runs/machines.
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();

    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *dock = win.findChild<QDockWidget *>(QStringLiteral("webServiceDock"));
    QVERIFY(dock);
    QVERIFY(dock->isHidden()); // hidden by default — toggle restores it
    QVERIFY(win.findChild<QLineEdit *>(QStringLiteral("webAddressEdit")));
    QVERIFY(win.findChild<QPushButton *>(QStringLiteral("webOpenButton")));
    auto *toggle = win.findChild<QToolButton *>(QStringLiteral("webServiceButton"));
    QVERIFY(toggle);
    QVERIFY(toggle->isCheckable());
    QVERIFY(!toggle->isChecked());
    // WebViewPanel is lazily constructed — absent before first show/submit.
    QVERIFY(!win.findChild<WebViewPanel *>(QStringLiteral("webViewPanel")));
  }

  void panelBuildsOnFirstShowWithoutEngine()
  {
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();

    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, nullptr);
    win.show(); // offscreen: "shows" without a real compositor
    auto *dock = win.findChild<QDockWidget *>(QStringLiteral("webServiceDock"));
    auto *toggle = win.findChild<QToolButton *>(QStringLiteral("webServiceButton"));
    QVERIFY(dock && toggle);

    QVERIFY(!win.findChild<WebViewPanel *>(QStringLiteral("webViewPanel")));
    toggle->click(); // toggleViewAction → dock visible → lazy panel built
    QVERIFY(!dock->isHidden());
    auto *panel = win.findChild<WebViewPanel *>(QStringLiteral("webViewPanel"));
    QVERIFY(panel);
    QVERIFY(!panel->engineAvailable()); // no engine until a URL, and never offscreen

    // Closing via the dock's title-bar ✕ unchecks the top-bar toggle.
    dock->hide();
    QVERIFY(!toggle->isChecked());
  }

  void urlSubmitValidatesThroughDock()
  {
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();

    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, nullptr);
    win.show();
    auto *dock = win.findChild<QDockWidget *>(QStringLiteral("webServiceDock"));
    auto *edit = win.findChild<QLineEdit *>(QStringLiteral("webAddressEdit"));
    auto *open = win.findChild<QPushButton *>(QStringLiteral("webOpenButton"));
    QVERIFY(dock && edit && open);
    dock->show();
    auto *panel = win.findChild<WebViewPanel *>(QStringLiteral("webViewPanel"));
    QVERIFY(panel);
    QSignalSpy failSpy(panel, &WebViewPanel::loadFailed);

    // Empty input is a no-op — nothing attempted, no error surfaced.
    open->click();
    QCOMPARE(failSpy.count(), 0);

    // Bare host[:port] gets an http scheme; offscreen the engine cannot
    // exist, so the panel fails honestly instead of crashing the host.
    edit->setText(QStringLiteral("localhost:9"));
    open->click();
    QCOMPARE(failSpy.count(), 1);
    QCOMPARE(panel->url().scheme(), QStringLiteral("http"));
    QCOMPARE(panel->url().host(), QStringLiteral("localhost"));
    QVERIFY(!panel->engineAvailable());
    QVERIFY(!panel->lastError().isEmpty());

    // A malformed URL is rejected as invalid before touching the engine.
    const int before = failSpy.count();
    edit->setText(QStringLiteral("http://[bad"));
    open->click();
    QCOMPARE(failSpy.count(), before + 1);
    QCOMPARE(panel->lastError(), QStringLiteral("无效的 URL"));
    QVERIFY(!panel->engineAvailable());
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestWebViewPanel tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_webviewpanel.moc"
