#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>

#include "../src/ui/webviewpanel.h"

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
