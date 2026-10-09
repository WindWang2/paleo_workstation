#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsView>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include "../src/workflow/officepreviewsession.h"
#include "../src/ui/datapreview/officepreviewwidget.h"
#include "../src/ui/paleotheme.h"

class TestOfficePreview : public QObject {
  Q_OBJECT
  QString fixture(const QString &ext) const {
    QString directory = qEnvironmentVariable("PALEO_OFFICE_FIXTURE_DIR");
    if (directory.isEmpty()) directory = QFileInfo(QFINDTESTDATA("fixtures/office/sample.docx")).absolutePath();
    return directory + "/sample." + ext;
  }
  QByteArray hash(const QString &path) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash sha(QCryptographicHash::Sha256); sha.addData(&file);
    return sha.result().toHex();
  }
  bool hasInk(const QImage &image) const {
    int pixels = 0;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x)
        if (qGray(image.pixel(x, y)) < 230 && ++pixels > 20) return true;
    return false;
  }
private slots:
  void initTestCase() { PaleoTheme::applyLightTheme(); }
  void missingComponentIsExplicit() {
    const auto previous = qgetenv("PALEO_OFFICE_RENDERER");
    qputenv("PALEO_OFFICE_RENDERER", "/does-not-exist/renderer");
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(fixture("docx"));
    qputenv("PALEO_OFFICE_RENDERER", previous);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains("未安装"));
    QVERIFY(!session.findChild<QProcess *>());
  }
  void cancelledVerificationCannotLaunch() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open("/does-not-exist.docx"); session.stop();
    QTest::qWait(100);
    QCOMPARE(failed.count(), 0);
    QVERIFY(!session.findChild<QProcess *>());
  }
  void shaMismatchCannotLaunch() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    OfficePreviewSession session;
    QSignalSpy failed(&session, &OfficePreviewSession::failed);
    session.open(fixture("docx"), QString(64, '0'));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
    QVERIFY(failed.first().first().toString().contains("SHA-256"));
    QVERIFY(!session.findChild<QProcess *>());
  }
  void realPages_data() {
    QTest::addColumn<QString>("extension");
    for (const QString &ext : {"doc", "docx", "xls", "xlsx", "ppt", "pptx"})
      QTest::newRow(qPrintable(ext)) << ext;
  }
  void realPages() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    QFETCH(QString, extension);
    const QString path = fixture(extension);
    const QByteArray before = hash(path); QVERIFY(!before.isEmpty());
    QPointer<QProcess> process;
    qint64 processId = 0;
    int ticks = 0; qint64 maximumGap = 0;
    QElapsedTimer elapsed; elapsed.start(); qint64 previous = 0;
    QTimer heartbeat; heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this, [&] {
      const qint64 now = elapsed.elapsed(); maximumGap = qMax(maximumGap, now - previous); previous = now; ++ticks;
    });
    heartbeat.start();
    {
      OfficePreviewWidget preview(path, QString::fromLatin1(before));
      preview.resize(1000, 700); preview.show();
      auto *session = preview.findChild<OfficePreviewSession *>(); QVERIFY(session);
      QSignalSpy pages(session, &OfficePreviewSession::pageReady), failed(session, &OfficePreviewSession::failed);
      QTRY_VERIFY_WITH_TIMEOUT(!pages.isEmpty() || !failed.isEmpty(), 130000);
      QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
      const QImage image = qvariant_cast<QImage>(pages.last().at(1));
      QVERIFY(!image.isNull()); QVERIFY(hasInk(image));
      QVERIFY(preview.findChild<QGraphicsView *>("officePageView"));
      process = session->findChild<QProcess *>(); QVERIFY(process);
      processId = process->processId(); QVERIFY(processId > 0);
      QVERIFY(QFileInfo(process->program()).fileName().startsWith("paleo_office_renderer"));
      QCOMPARE(process->arguments().size(), 2);
      auto *selector = preview.findChild<QComboBox *>("officePageSelector"); QVERIFY(selector);
      QVERIFY(selector->count() > 0);
      if (extension == "xlsx" || extension == "xls") QVERIFY(selector->count() >= 2);
      if (selector->count() > 1) {
        const int count = pages.count();
        selector->setCurrentIndex(selector->count() - 1);
        QTRY_VERIFY_WITH_TIMEOUT(pages.count() > count || !failed.isEmpty(), 60000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(pages.last().at(0).toInt(), selector->count() - 1);
        QVERIFY(hasInk(qvariant_cast<QImage>(pages.last().at(1))));
      }
      QVERIFY(ticks > 5);
      QVERIFY2(maximumGap < 500, qPrintable(QString::number(maximumGap)));
      if (extension == "docx" && !qEnvironmentVariable("PALEO_OFFICE_SCREENSHOT").isEmpty())
        QVERIFY(preview.grab().save(qEnvironmentVariable("PALEO_OFFICE_SCREENSHOT")));
    }
    QTRY_VERIFY_WITH_TIMEOUT(process.isNull(), 5000);
#ifdef Q_OS_LINUX
    QVERIFY(!QFileInfo::exists(QStringLiteral("/proc/%1").arg(processId)));
#endif
    QCOMPARE(hash(path), before);
  }
  void corruptDocumentFailsWithoutBlocking() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    QTemporaryDir directory;
    const QString path = directory.filePath("broken.docx");
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("not a document"); file.close();
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready), failed(&session, &OfficePreviewSession::failed);
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 130000);
    QCOMPARE(ready.count(), 0);
  }
  void longWorkbookRendersLaterPagesAndCoalescesNavigation() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready), pages(&session, &OfficePreviewSession::pageReady),
               failed(&session, &OfficePreviewSession::failed);
    session.open(QFileInfo(fixture("xlsx")).dir().filePath("long.xlsx"));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 130000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    const QStringList labels = ready.first().first().toStringList();
    QVERIFY(labels.size() > 3);
    int lastFirstSheet = 0;
    const QString sheetName = labels.first().section(" · ", 0, 0);
    for (int i = 0; i < labels.size(); ++i)
      if (labels[i].section(" · ", 0, 0) == sheetName) lastFirstSheet = i;
    QVERIFY(lastFirstSheet > 1);
    session.requestPage(0);
    session.requestPage(1);
    session.requestPage(lastFirstSheet);
    QTRY_VERIFY_WITH_TIMEOUT(!pages.isEmpty() || !failed.isEmpty(), 60000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(pages.last().first().toInt(), lastFirstSheet);
    QVERIFY(hasInk(qvariant_cast<QImage>(pages.last().at(1))));
    QCOMPARE(pages.count(), 1); // obsolete requests never replace the selected page
  }
  void inlineWorkbookShowsText() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready), pages(&session, &OfficePreviewSession::pageReady),
               failed(&session, &OfficePreviewSession::failed);
    session.open(QFileInfo(fixture("xlsx")).dir().filePath("inline.xlsx"));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() || !failed.isEmpty(), 130000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    session.requestPage(0);
    QTRY_VERIFY_WITH_TIMEOUT(!pages.isEmpty() || !failed.isEmpty(), 60000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QVERIFY(hasInk(qvariant_cast<QImage>(pages.first().at(1))));
  }
  void concurrentWordPreviewsKeepPagesStable() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    QList<std::shared_ptr<OfficePreviewSession>> sessions;
    int completed = 0;
    QString error;
    for (int i = 0; i < 3; ++i) {
      auto session = std::make_shared<OfficePreviewSession>();
      auto *worker = session.get();
      auto firstImage = std::make_shared<QImage>();
      auto rendered = std::make_shared<int>(0);
      connect(worker, &OfficePreviewSession::ready, this, [&, worker](const QStringList &labels) {
        if (labels.size() != 1) error = "Unexpected pagination of a one-paragraph document";
        worker->requestPage(0);
      });
      connect(session.get(), &OfficePreviewSession::failed, this, [&](const QString &reason) { error = reason; });
      connect(worker, &OfficePreviewSession::pageReady, this, [&, worker, firstImage, rendered](int, const QImage &image) {
        if (!hasInk(image)) error = "Empty Word page";
        if (*rendered == 0) *firstImage = image;
        else if (image != *firstImage && error.isEmpty()) error = "Rendering changed the document layout";
        if (++*rendered < 3) worker->requestPage(0);
        else ++completed;
      });
      sessions.append(session);
      session->open(fixture("docx"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(completed == 3 || !error.isEmpty(), 130000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (const auto &session : sessions) session->stop();
  }
  void closingDuringLoadDropsResults() {
    if (OfficePreviewSession::rendererPath().isEmpty()) QSKIP("Build vendor/fetch-calligra.sh first");
    OfficePreviewSession session;
    QSignalSpy ready(&session, &OfficePreviewSession::ready), failed(&session, &OfficePreviewSession::failed);
    session.open(fixture("docx"));
    QTRY_VERIFY_WITH_TIMEOUT(session.findChild<QProcess *>(), 2000);
    QPointer<QProcess> process = session.findChild<QProcess *>();
    QElapsedTimer elapsed; elapsed.start(); session.stop();
    QVERIFY(elapsed.elapsed() < 100);
    QTRY_VERIFY_WITH_TIMEOUT(process.isNull(), 5000);
    QCOMPARE(ready.count(), 0); QCOMPARE(failed.count(), 0);
  }
};
QTEST_MAIN(TestOfficePreview)
#include "tst_officepreview.moc"
