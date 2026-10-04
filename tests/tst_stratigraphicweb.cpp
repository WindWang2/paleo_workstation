#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "../src/workflow/stratigraphicwebsession.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/webviewpanel.h"
#include "../src/ui/paleotheme.h"

#if PALEO_HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineView>
#endif

class TestStratigraphicWeb : public QObject
{
  Q_OBJECT
private:
  QTcpServer m_server;
  int m_requests = 0;
  QString endpoint(quint16 port) const
  {
    return QStringLiteral("http://127.0.0.1:%1/web_prototype/workspace.html").arg(port);
  }
  quint16 unusedPort()
  {
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0))
      return 0;
    return reservation.serverPort();
  }
private slots:
  void initTestCase()
  {
    QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = m_server.nextPendingConnection())
      {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          socket->readAll();
          ++m_requests;
          const QByteArray body("<!doctype html><html><body>fixture workspace</body></html>");
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
  }
  void init()
  {
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
  }
  void configurationStaysInLocalSettings()
  {
    StratigraphicWebSession session;
    QVERIFY(session.setEndpoint(QStringLiteral("localhost:8772")));
    QCOMPARE(session.endpoint().path(), QStringLiteral("/web_prototype/workspace.html"));
    session.setProjectDirectory(QStringLiteral("/external/private-workspace"));
    session.setPythonExecutable(QStringLiteral("/external/python"));
    QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
    QCOMPARE(settings.value(QStringLiteral("correlationWeb/projectDirectory")).toString(),
             session.projectDirectory());
    QCOMPARE(settings.value(QStringLiteral("correlationWeb/url")).toString(),
             session.endpoint().toString());
  }
  void rejectsInvalidEndpoints_data()
  {
    QTest::addColumn<QString>("value");
    for (const QString &url : {QString(), QStringLiteral("file:///private/index.html"),
         QStringLiteral("javascript:alert(1)"), QStringLiteral("http://[broken"),
         QStringLiteral("http://user:password@localhost:8771")})
      QTest::newRow(qPrintable(url)) << url;
  }
  void rejectsInvalidEndpoints()
  {
    QFETCH(QString, value);
    StratigraphicWebSession session;
    QSignalSpy failed(&session, &StratigraphicWebSession::failed);
    QVERIFY(!session.setEndpoint(value));
    QCOMPARE(failed.count(), 1);
    QVERIFY(!session.busy());
    QVERIFY(!session.ownsService());
  }
  void reusesExistingServiceWithoutOwningIt()
  {
    {
      StratigraphicWebSession session;
      QVERIFY(session.setEndpoint(endpoint(m_server.serverPort())));
      QSignalSpy ready(&session, &StratigraphicWebSession::ready);
      session.startLocalService(); // 空目录也可连接已有服务
      QTRY_COMPARE(ready.count(), 1);
      QVERIFY(!session.ownsService());
      QVERIFY(!session.busy());
    }
    QVERIFY(m_server.isListening());
    StratigraphicWebSession another;
    QVERIFY(another.setEndpoint(endpoint(m_server.serverPort())));
    QSignalSpy ready(&another, &StratigraphicWebSession::ready);
    another.connectToService();
    QTRY_COMPARE(ready.count(), 1);
  }
  void localLaunchNeedsExternalProject()
  {
    StratigraphicWebSession session;
    QVERIFY(session.setEndpoint(endpoint(unusedPort())));
    QSignalSpy failed(&session, &StratigraphicWebSession::failed);
    session.startLocalService();
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("独立项目目录")));
    QVERIFY(!session.ownsService());
  }
  void remoteEndpointCannotLaunchLocalProcess()
  {
    StratigraphicWebSession session;
    QVERIFY(session.setEndpoint(QStringLiteral("https://example.invalid/workspace.html")));
    QSignalSpy failed(&session, &StratigraphicWebSession::failed);
    session.startLocalService();
    QCOMPARE(failed.count(), 1);
    QVERIFY(!session.busy());
    QVERIFY(!session.ownsService());
  }
  void launchesAndReapsOnlyOwnedProcess()
  {
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
      QSKIP("Python3 required for subprocess fixture");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("web_prototype")));
    QFile html(directory.filePath(QStringLiteral("web_prototype/workspace.html")));
    QVERIFY(html.open(QIODevice::WriteOnly));
    html.write("fixture workspace");
    html.close();
    QFile script(directory.filePath(QStringLiteral("run.py")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("import argparse, http.server\n"
                 "p=argparse.ArgumentParser()\n"
                 "p.add_argument('--host'); p.add_argument('--port', type=int)\n"
                 "a=p.parse_args()\n"
                 "http.server.ThreadingHTTPServer((a.host,a.port), http.server.SimpleHTTPRequestHandler).serve_forever()\n");
    script.close();
    const quint16 port = unusedPort();
    {
      StratigraphicWebSession session;
      QVERIFY(session.setEndpoint(endpoint(port)));
      session.setProjectDirectory(directory.path());
      session.setPythonExecutable(python);
      QSignalSpy ready(&session, &StratigraphicWebSession::ready);
      QSignalSpy failed(&session, &StratigraphicWebSession::failed);
      session.startLocalService();
      session.startLocalService(); // 重复点击不会重复拉进程
      QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
      QCOMPARE(failed.count(), 0);
      QVERIFY(session.ownsService());
      session.startLocalService();
      QTRY_COMPARE(ready.count(), 2);
    }
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(!socket.waitForConnected(500)); // 析构确实回收，不留下孤儿服务
  }
  void failedPythonLaunchIsRecoverable()
  {
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("web_prototype")));
    for (const QString &name : {QStringLiteral("run.py"), QStringLiteral("web_prototype/workspace.html")})
    {
      QFile file(directory.filePath(name));
      QVERIFY(file.open(QIODevice::WriteOnly));
    }
    StratigraphicWebSession session;
    QVERIFY(session.setEndpoint(endpoint(unusedPort())));
    session.setProjectDirectory(directory.path());
    session.setPythonExecutable(directory.filePath(QStringLiteral("missing-python")));
    QSignalSpy failed(&session, &StratigraphicWebSession::failed);
    session.startLocalService();
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!session.busy());
    QVERIFY(!session.ownsService());
    QVERIFY(session.setEndpoint(endpoint(m_server.serverPort())));
    QSignalSpy ready(&session, &StratigraphicWebSession::ready);
    session.connectToService();
    QTRY_COMPARE(ready.count(), 1);
  }
  void centralPageWorksWithoutPaleoProjectAndKeepsSession()
  {
    PaleoMainWindow window(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto *save = new QAction(&window);
    save->setObjectName(QStringLiteral("saveProjectAction"));
    save->setShortcut(QKeySequence::Save);
    window.addAction(save);
    auto *session = window.findChild<StratigraphicWebSession *>();
    QVERIFY(session);
    QVERIFY(session->setEndpoint(endpoint(m_server.serverPort())));
    auto *web = window.findChild<WebViewPanel *>(QStringLiteral("correlationWebView"));
    QVERIFY(web);
    QVERIFY(!web->engineAvailable());
    window.show();
    auto *right = window.findChild<QDockWidget *>(QStringLiteral("pagePanelDock"));
    QVERIFY(right);
    right->show();
    QSignalSpy ready(session, &StratigraphicWebSession::ready);
    window.showPage(QStringLiteral("correlation"));
    QVERIFY(save->shortcut().isEmpty());
    QCOMPARE(window.findChild<QStackedWidget *>(QStringLiteral("centerStack"))->currentIndex(), 1);
    QCOMPARE(window.findChild<QStackedWidget *>(QStringLiteral("workspaceStack"))->currentIndex(), 2);
    QVERIFY(right->isHidden());
    QVERIFY(window.findChild<QAction *>(QStringLiteral("correlationWebStartAction")));
    QTRY_COMPARE(ready.count(), 1);
    QCOMPARE(web->url(), session->endpoint());
    window.showPage(QStringLiteral("data"));
    QCOMPARE(save->shortcut(), QKeySequence(QKeySequence::Save));
    QVERIFY(!right->isHidden());
    QCOMPARE(window.findChild<QStackedWidget *>(QStringLiteral("centerStack"))->currentIndex(), 0);
    window.showPage(QStringLiteral("correlation"));
    QTest::qWait(100);
    QCOMPARE(ready.count(), 1); // 切回不探测/刷新，不丢解释状态
    QCOMPARE(window.findChild<WebViewPanel *>(QStringLiteral("correlationWebView")), web);
    window.showStartup();
    QVERIFY(!right->isHidden());
    QCOMPARE(save->shortcut(), QKeySequence(QKeySequence::Save));
  }
  void realExternalWorkspaceSmoke()
  {
    const QString url = qEnvironmentVariable("PALEO_CORRELATION_SMOKE_URL");
    if (url.isEmpty())
      QSKIP("Set PALEO_CORRELATION_SMOKE_URL for private external workspace smoke");
#if PALEO_HAVE_WEBENGINE
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Use a graphical Qt platform for WebEngine smoke");
    PaleoTheme::applyLightTheme();
    PaleoMainWindow window(nullptr, nullptr, nullptr, nullptr, nullptr);
    window.resize(1600, 1000);
    auto *session = window.findChild<StratigraphicWebSession *>();
    QVERIFY(session && session->setEndpoint(url));
    window.findChild<QLineEdit *>(QStringLiteral("correlationWebAddress"))->setText(url);
    auto *panel = window.findChild<WebViewPanel *>(QStringLiteral("correlationWebView"));
    QVERIFY(panel);
    QSignalSpy loaded(panel, &WebViewPanel::loadFinished);
    window.show();
    window.showPage(QStringLiteral("correlation"));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 30000);
    QVERIFY(loaded.last().first().toBool());
    auto *view = panel->findChild<QWebEngineView *>();
    QVERIFY(view);
    bool workspaceReady = false;
    for (int i = 0; i < 100 && !workspaceReady; ++i)
    {
      bool completed = false;
      view->page()->runJavaScript(QStringLiteral(
          "Boolean(document.querySelector('#projectSelect option') && "
          "document.querySelector('#steps [data-step=correlation]'))"),
          [&](const QVariant &value) { workspaceReady = value.toBool(); completed = true; });
      QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
      if (!workspaceReady)
        QTest::qWait(200);
    }
    QVERIFY2(workspaceReady, "Workspace must load projects through its own API");
    bool interacted = false;
    view->page()->runJavaScript(QStringLiteral(
        "document.querySelector('#steps [data-step=correlation]').click(); true;"),
        [&](const QVariant &value) { interacted = value.toBool(); });
    QTRY_VERIFY(interacted);
    bool chartReady = false;
    for (int i = 0; i < 240 && !chartReady; ++i)
    {
      bool completed = false;
      view->page()->runJavaScript(QStringLiteral(
          "Boolean(document.querySelector('.correlation-scroll svg path'))"),
          [&](const QVariant &value) { chartReady = value.toBool(); completed = true; });
      QTRY_VERIFY_WITH_TIMEOUT(completed, 1000);
      if (!chartReady)
        QTest::qWait(250);
    }
    QVERIFY2(chartReady, "Real correlation diagram must render through the external API");
    const QString capture = qEnvironmentVariable("PALEO_CORRELATION_CAPTURE");
    if (!capture.isEmpty())
      QVERIFY(window.grab().save(capture));
    bool stateSet = false;
    view->page()->runJavaScript(QStringLiteral("window.paleoSessionMarker='retained'; true;"),
        [&](const QVariant &value) { stateSet = value.toBool(); });
    QTRY_VERIFY(stateSet);
    window.showPage(QStringLiteral("data"));
    window.showPage(QStringLiteral("correlation"));
    bool retained = false;
    view->page()->runJavaScript(QStringLiteral("window.paleoSessionMarker==='retained'"),
        [&](const QVariant &value) { retained = value.toBool(); });
    QTRY_VERIFY(retained);
    bool themeMatches = false;
    view->page()->runJavaScript(QStringLiteral(
        "getComputedStyle(document.body).color==='rgb(36, 48, 62)'"),
        [&](const QVariant &value) { themeMatches = value.toBool(); });
    QTRY_VERIFY(themeMatches);
    PaleoTheme::applyDarkTheme();
    bool darkMatches = false;
    QTest::qWait(200);
    view->page()->runJavaScript(QStringLiteral(
        "getComputedStyle(document.body).color==='rgb(228, 234, 242)'"),
        [&](const QVariant &value) { darkMatches = value.toBool(); });
    QTRY_VERIFY(darkMatches);
    PaleoTheme::applyLightTheme();
#else
    QSKIP("WebEngine unavailable in this build");
#endif
  }
  void embeddedBlobDownloadSmoke()
  {
#if PALEO_HAVE_WEBENGINE
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Use a graphical Qt platform for WebEngine download smoke");
    WebViewPanel panel;
    panel.show();
    QSignalSpy loaded(&panel, &WebViewPanel::loadFinished);
    QVERIFY(panel.setUrl(QUrl(endpoint(m_server.serverPort()))));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    QVERIFY(loaded.last().first().toBool());
    auto *view = panel.findChild<QWebEngineView *>();
    QVERIFY(view);
    QTemporaryDir downloadDirectory;
    const QString target = downloadDirectory.filePath(QStringLiteral("export.txt"));
    QTimer acceptSave;
    connect(&acceptSave, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget()))
      {
        dialog->selectFile(target);
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
      }
    });
    acceptSave.start(50);
    view->page()->runJavaScript(QStringLiteral(
        "(()=>{const a=document.createElement('a');"
        "a.href=URL.createObjectURL(new Blob(['host-download-fixture'],{type:'text/plain'}));"
        "a.download='export.txt';document.body.append(a);a.click();a.remove();})()"));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target), 10000);
    acceptSave.stop();
    QFile file(target);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("host-download-fixture"));
#else
    QSKIP("WebEngine unavailable in this build");
#endif
  }
};

int main(int argc, char **argv)
{
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  TestStratigraphicWeb test;
  return QTest::qExec(&test, argc, argv);
}
#include "tst_stratigraphicweb.moc"
