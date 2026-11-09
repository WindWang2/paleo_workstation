// 层：测试壳
#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "services/areawellchart.h"
#include "workflow/welltracksourcesession.h"
#include <algorithm>

namespace {

QByteArray las(int offset) {
  QByteArray source = "~Version\nVERS. 2.0 : LAS\nWRAP. NO :\n~Well\nWELL. WT-1 : Well\nNULL. -999.25 :\n"
                      "~Curve\nDEPT.M : Depth\nGR.API : Gamma\n~Ascii\n";
  for (int depth = 0; depth < 33000; ++depth)
    source += QByteArray::number(depth) + ' ' + QByteArray::number(depth + offset) + '\n';
  return source;
}

} // namespace

class TestWellTrackSourceSession : public QObject {
  Q_OBJECT
private slots:
  void exactFileWindowAndPersistence();
};

void TestWellTrackSourceSession::exactFileWindowAndPersistence() {
  using namespace paleo::welltrack;
  document_fixture::Fixture fixture;
  QVERIFY(fixture.init());
  const auto first = fixture.temp.filePath(QStringLiteral("first.las"));
  const auto second = fixture.temp.filePath(QStringLiteral("second.las"));
  const auto secondBytes = las(1000);
  QVERIFY(document_fixture::write(first, las(0)));
  QVERIFY(document_fixture::write(second, secondBytes));
  QVERIFY2(fixture.importModel(first), qPrintable(fixture.error));
  QVERIFY2(fixture.importModel(second), qPrintable(fixture.error));
  QVERIFY2(fixture.bind(), qPrintable(fixture.error));
  const auto wells = fixture.catalog.storedEntities(QStringLiteral("well"));
  QCOMPARE(wells.size(), 1);
  const auto &well = wells.front();
  const auto modelWellId = well.extra.value(QStringLiteral("areaModelWellId")).toLongLong();
  {
    paleo::data::DataService sdk;
    std::string detail;
    QVERIFY2(sdk.openProjectReadOnly(paleo::area_model::directory(fixture.project).toStdString(), detail),
             detail.c_str());
    AreaWellChartQuery query;
    query.allowSourceTrackFallback = true;
    query.maxDefaultPlotValues = 10;
    WellComposite::ComprehensiveWellData shell;
    QString error;
    QVERIFY2(readAreaWellChart(sdk, modelWellId, shell, &error, {}, &query), qPrintable(error));
    QVERIFY(shell.continuousCurves.isEmpty());
    QCOMPARE(shell.modelWellId, modelWellId);
    QVERIFY(shell.minDepth <= 25.0 && shell.maxDepth >= 32000.0);
  }
  QString trackId;
  {
    WellTrackSourceSession controller(&fixture.session, &fixture.tasks, fixture.project, well.id, modelWellId);
    QVector<CurveChoice> choices;
    QString error;
    connect(&controller, &WellTrackSourceSession::choicesReady, this,
        [&](const QVector<CurveChoice> &values, const QString &why) { choices = values; error = why; });
    WellComposite::CurveData curve;
    int reads = 0;
    connect(&controller, &WellTrackSourceSession::trackReady, this,
        [&](const QString &id, const WellComposite::CurveData &value, const QString &why) {
          trackId = id; curve = value; error = why; ++reads;
        });
    connect(&controller, &WellTrackSourceSession::failed, this, [&](const QString &why) { error = why; });
    controller.requestWindow(25.0, 27.0);
    controller.requestChoices();
    QTRY_VERIFY(!choices.isEmpty() || !error.isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(choices.size(), 2);
    const auto picked = std::find_if(choices.cbegin(), choices.cend(), [](const CurveChoice &choice) {
      return choice.fileName == QLatin1String("second.las");
    });
    QVERIFY(picked != choices.cend());
    controller.addCurve(*picked);
    QTRY_VERIFY(reads > 0 || !error.isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(curve.depths, (QVector<float>{25, 26, 27}));
    QCOMPARE(curve.values, (QVector<float>{1025, 1026, 1027}));
    QCOMPARE(fixture.session.wellCurveTracks(well.id).front().source, picked->source);

    controller.requestWindow(25.5, 26.5);
    QTest::qWait(250);
    QCOMPARE(reads, 1);
    controller.requestWindow(4500.0, 4502.0);
    QTRY_COMPARE(reads, 2);
    QCOMPARE(curve.depths, (QVector<float>{4500, 4501, 4502}));
    QCOMPARE(curve.values, (QVector<float>{5500, 5501, 5502}));

    controller.requestWindow(0.0, 32999.0);
    QTRY_COMPARE(reads, 3);
    QVERIFY(!error.isEmpty());
    controller.requestWindow(20000.0, 20002.0);
    QTRY_COMPARE(reads, 4);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(curve.values, (QVector<float>{21000, 21001, 21002}));
  }

  fixture.session.close();
  QVERIFY2(fixture.bind(), qPrintable(fixture.error));
  const auto tracks = fixture.session.wellCurveTracks(well.id);
  QCOMPARE(tracks.size(), 1);
  QCOMPARE(tracks.front().id, trackId);
  WellTrackSourceSession reopened(&fixture.session, &fixture.tasks, fixture.project, well.id, modelWellId);
  reopened.removeTrack(trackId);
  QVERIFY(fixture.session.wellCurveTracks(well.id).isEmpty());
  QFile original(second);
  QVERIFY(original.open(QIODevice::ReadOnly));
  QCOMPARE(original.readAll(), secondBytes);
}

QTEST_GUILESS_MAIN(TestWellTrackSourceSession)
#include "tst_welltracksourcesession.moc"
