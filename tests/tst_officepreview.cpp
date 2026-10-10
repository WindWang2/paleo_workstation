#include <QtTest>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "../src/catalog/datacatalog.h"
#include "../src/ui/datapreview/officepreviewwidget.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/webviewpanel.h"
#include "../src/workflow/officepreviewsession.h"

class TestOfficePreview : public QObject {
  Q_OBJECT
  struct Env {
    QByteArray key, previous;
    bool had = false;
    Env(const char *name, const QByteArray &value)
        : key(name), previous(qgetenv(name)), had(qEnvironmentVariableIsSet(name))
    {
      qputenv(key, value);
    }
    ~Env()
    {
      if (had) qputenv(key, previous);
      else qunsetenv(key.constData());
    }
  };
  QString fixture(const QString &ext) const {
    QString directory = qEnvironmentVariable("PALEO_OFFICE_FIXTURE_DIR");
    if (directory.isEmpty()) directory = QFileInfo(QFINDTESTDATA("fixtures/office/sample.docx")).absolutePath();
    return directory + "/sample." + ext;
  }
  QByteArray hashOf(const QString &path) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash sha(QCryptographicHash::Sha256);
    sha.addData(&file);
    return sha.result().toHex();
  }
  QByteArray readAll(const QString &path) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
  }
  QTemporaryDir m_editor;
private slots:
  void initTestCase() {
    PaleoTheme::applyLightTheme();
    QVERIFY(m_editor.isValid());
    QFile index(m_editor.filePath(QStringLiteral("index.html")));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("editor-shell");
    index.close();
    QFile editorPage(m_editor.filePath(QStringLiteral("editor.html")));
    QVERIFY(editorPage.open(QIODevice::WriteOnly));
    editorPage.write("editor-app");
    editorPage.close();
    QFile worker(m_editor.filePath(QStringLiteral("sw.js")));
    QVERIFY(worker.open(QIODevice::WriteOnly));
    worker.write("worker-should-not-be-served");
  }
  void missingEditorIsExplicit() {
    Env env("PALEO_OFFICE_EDITOR", "/does-not-exist/office-editor");
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(fixture(QStringLiteral("docx")));
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("未安装")));
    QVERIFY(session.endpoint().isEmpty());
  }
  void cancelledVerificationCannotLaunch() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    QSignalSpy ready(&session, &OfficePreviewSession::ready);
    session.open(QStringLiteral("/does-not-exist.docx"));
    session.stop();
    const auto idle = [&session] {
      for (auto *watcher : session.findChildren<QFutureWatcherBase *>())
        if (!watcher->isFinished()) return false;
      return true;
    };
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 5000);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(ready.count(), 0);
    QVERIFY(session.endpoint().isEmpty());
    QVERIFY(session.documentUrl().isEmpty());
  }
  void shaMismatchCannotLaunch() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(fixture(QStringLiteral("docx")), QString(64, QLatin1Char('0')));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
    QVERIFY(failed.first().first().toString().contains(QStringLiteral("SHA-256")));
    QVERIFY(session.endpoint().isEmpty());
  }
  void servesOnlyTheOpenedFile() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    const QString path = fixture(QStringLiteral("docx"));
    const QByteArray before = hashOf(path);
    const QByteArray original = readAll(path);
    QVERIFY(!before.isEmpty());
    QVERIFY(!original.isEmpty());
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready);
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(path, QString::fromLatin1(before));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 2000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(session.endpoint().host(), QStringLiteral("127.0.0.1"));
    QCOMPARE(session.endpoint().scheme(), QStringLiteral("http"));

    QNetworkAccessManager network;
    QNetworkReply *document = network.get(QNetworkRequest(session.documentUrl()));
    QTRY_VERIFY_WITH_TIMEOUT(document->isFinished(), 3000);
    QCOMPARE(document->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(document->readAll(), original);
    document->deleteLater();

    QUrl shell = session.endpoint();
    shell.setPath(QStringLiteral("/index.html"));
    QNetworkReply *index = network.get(QNetworkRequest(shell));
    QTRY_VERIFY_WITH_TIMEOUT(index->isFinished(), 3000);
    QCOMPARE(index->readAll(), QByteArray("editor-shell"));
    index->deleteLater();
    QUrl editorPage = session.endpoint();
    editorPage.setPath(QStringLiteral("/editor"));
    editorPage.setQuery(QStringLiteral("embed=1"));
    QNetworkReply *editor = network.get(QNetworkRequest(editorPage));
    QTRY_VERIFY_WITH_TIMEOUT(editor->isFinished(), 3000);
    QCOMPARE(editor->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(editor->readAll(), QByteArray("editor-app"));
    editor->deleteLater();
    QUrl worker = session.endpoint();
    worker.setPath(QStringLiteral("/sw.js"));
    QNetworkReply *sw = network.get(QNetworkRequest(worker));
    QTRY_VERIFY_WITH_TIMEOUT(sw->isFinished(), 3000);
    QCOMPARE(sw->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    sw->deleteLater();
    QDir(m_editor.path()).mkpath(QStringLiteral("assets"));
    QFile bundle(m_editor.filePath(QStringLiteral("assets/document-stub.js")));
    QVERIFY(bundle.open(QIODevice::WriteOnly));
    bundle.write("features:{spellcheck:{mode:!1,change:!1}}");
    bundle.close();
    QUrl script = session.endpoint();
    script.setPath(QStringLiteral("/assets/document-stub.js"));
    QNetworkReply *scriptReply = network.get(QNetworkRequest(script));
    QTRY_VERIFY_WITH_TIMEOUT(scriptReply->isFinished(), 3000);
    QCOMPARE(scriptReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(scriptReply->readAll(),
             QByteArray("features:{spellcheck:{mode:!1,change:!1},featuresTips:!1}"));
    scriptReply->deleteLater();

    const QString token = session.documentUrl().path().section(QLatin1Char('/'), 2, 2);
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(session.endpoint().port()));
    QVERIFY(socket.waitForConnected(1000));
    const QByteArray request = QByteArray("GET /paleo-doc/") + token.toUtf8()
                               + "/../../index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    QCOMPARE(socket.write(request), request.size());
    QVERIFY(socket.flush());
    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 && !response.contains("\r\n\r\n"))
    {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      response += socket.readAll();
    }
    QVERIFY2(response.startsWith("HTTP/1.1 404"), response.constData());
    QVERIFY(!response.contains("editor-shell"));
    QCOMPARE(hashOf(path), before);
  }
  void saveRoundTripLeavesOriginalUntouched() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    const QString path = fixture(QStringLiteral("xlsx"));
    const QByteArray before = hashOf(path);
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready);
    QSignalSpy saved(&session, &OfficePreviewSession::documentSaved);
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(path, QString::fromLatin1(before));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 2000);
    QVERIFY(failed.isEmpty());
    QUrl save = session.endpoint();
    save.setPath(QStringLiteral("/paleo-save/") + session.documentUrl().path().section(QLatin1Char('/'), 2, 2));
    QNetworkRequest request(save);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    QNetworkAccessManager network;
    QNetworkReply *reply = network.post(request, QByteArray("edited-bytes"));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 3000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 204);
    QCOMPARE(saved.count(), 1);
    QCOMPARE(readAll(saved.first().first().toString()), QByteArray("edited-bytes"));
    QCOMPARE(hashOf(path), before);
    reply->deleteLater();
  }
  void stopRevokesTheEndpoint() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready);
    session.open(fixture(QStringLiteral("ppt")));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 2000);
    const QUrl url = session.endpoint();
    session.stop();
    QVERIFY(session.endpoint().isEmpty());
    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(QNetworkRequest(url));
    // Windows 上对已停端点的拒绝返回耗时可达 ~3.5s——轮询提前退出，放宽无代价。
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
    QVERIFY(reply->error() != QNetworkReply::NoError);
    reply->deleteLater();
  }
  void widgetUsesLocalHttpAndFallsBackHeadless() {
    Env env("PALEO_OFFICE_EDITOR", m_editor.path().toUtf8());
    const QString path = fixture(QStringLiteral("pptx"));
    OfficePreviewWidget preview(path, QString::fromLatin1(hashOf(path)));
    preview.resize(800, 600);
    preview.show();
    auto *session = preview.findChild<OfficePreviewSession *>();
    QVERIFY(session);
    QSignalSpy failed(session, &OfficePreviewSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(session->endpoint().isValid() || !failed.isEmpty(), 2000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    auto *web = preview.findChild<WebViewPanel *>(QStringLiteral("officeEditor"));
    QVERIFY(web);
    QCOMPARE(web->url().host(), QStringLiteral("127.0.0.1"));
    if (!web->engineAvailable())
    {
      auto *external = preview.findChild<QPushButton *>(QStringLiteral("webExternalButton"));
      QVERIFY(external);
      QVERIFY(external->isVisible());
    }
  }
  void commitEditRegistersDerivedVersion() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QString error;
    QVERIFY2(catalog.open(project.path(), &error), qPrintable(error));
    CatalogAsset asset;
    asset.id = catalog.nextAssetId();
    asset.type = QStringLiteral("document");
    asset.format = QStringLiteral("docx");
    asset.displayName = QStringLiteral("sample.docx");
    QVERIFY2(catalog.addAsset(asset, &error), qPrintable(error));
    const QString rawPath = fixture(QStringLiteral("docx"));
    const QByteArray before = hashOf(rawPath);
    CatalogVersion raw;
    raw.id = catalog.nextVersionId();
    raw.assetId = asset.id;
    raw.stage = QStringLiteral("RAW");
    raw.managed = false;
    raw.path = rawPath;
    raw.fileName = QStringLiteral("sample.docx");
    raw.sha256 = QString::fromLatin1(before);
    QVERIFY2(catalog.addVersion(raw, &error), qPrintable(error));
    QTemporaryDir edits;
    QVERIFY(edits.isValid());
    const QString saved = edits.filePath(QStringLiteral("saved.docx"));
    QFile out(saved);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write("new-bytes");
    out.close();
    QVERIFY2(OfficePreviewSession::commitEdit(&catalog, asset.id, raw.id, saved, &error), qPrintable(error));
    const QVector<CatalogVersion> versions = catalog.versionsForAsset(asset.id);
    QCOMPARE(versions.size(), 2);
    const CatalogVersion derived = versions.last();
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{raw.id});
    QCOMPARE(derived.extra.value(QStringLiteral("producer")).toString(), QStringLiteral("ranuts-document"));
    const QString stored = DataCatalog::resolvedVersionPath(project.path(), derived);
    QCOMPARE(readAll(stored), QByteArray("new-bytes"));
    QCOMPARE(hashOf(rawPath), before);
    QVERIFY2(!OfficePreviewSession::commitEdit(nullptr, asset.id, raw.id, saved, &error), qPrintable(error));
    QVERIFY(error.contains(QStringLiteral("未打开")));
  }
};

QTEST_MAIN(TestOfficePreview)
#include "tst_officepreview.moc"
