#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <paleo_data/service.h>

#include "services/areawellchart.h"
#include "services/welltracksource.h"

namespace {

bool addCurveSource(paleo::data::DataService &service, const QString &path, const std::string &wellName,
                    int samples, double offset, long long &id, std::string &error) {
  const auto bytes = path.toUtf8();
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
    return false;
  file.close();

  paleo::data::ParsedFile parsed;
  parsed.kind = "logs";
  parsed.status = "parsed";
  paleo::data::WellDataset well;
  well.wellInfo.wellName = wellName;
  paleo::data::LogSet logs;
  logs.logSetName = "test log set";
  logs.depthInfo.depthType = "MD";
  logs.depthInfo.depthUnit = "ft";
  paleo::data::LogCurve curve;
  curve.curveName = "GR";
  curve.curveUnit = "API";
  for (int i = 0; i < samples; ++i) {
    logs.samplingAxis.depthValues.push_back(i);
    curve.sampleValues.push_back(i + offset);
  }
  logs.logCurves.push_back(std::move(curve));
  well.wellbore.logSets.push_back(std::move(logs));
  parsed.wells.push_back(std::move(well));
  const auto sha = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
  return service.importPreparedFile(path.toStdString(), path.toStdString(), path.toStdString(),
                                    sha, parsed, id, error);
}

} // namespace

class TestWellTrackSource : public QObject {
  Q_OBJECT
private slots:
  void exactCurveReadsOnlyItsRequestedMdWindow();
};

void TestWellTrackSource::exactCurveReadsOnlyItsRequestedMdWindow() {
  QTemporaryDir fixture;
  QVERIFY(fixture.isValid());
  const auto model = fixture.filePath("area-model");
  const auto firstFile = fixture.filePath("first.LAS");
  const auto otherFile = fixture.filePath("second.las");
  const auto xmlFile = fixture.filePath("other-format.xml");
  const auto otherWellFile = fixture.filePath("foreign-well.las");
  paleo::data::DataService service;
  std::string detail;
  QVERIFY2(service.createProject(model.toStdString(), "well track source", detail), detail.c_str());
  long long firstId = 0, otherId = 0, xmlId = 0, otherWellId = 0;
  QVERIFY2(addCurveSource(service, firstFile, "A-1", 9000, 10, firstId, detail), detail.c_str());
  QVERIFY2(addCurveSource(service, otherFile, "A-1", 3, 20, otherId, detail), detail.c_str());
  QVERIFY2(addCurveSource(service, xmlFile, "A-1", 3, 30, xmlId, detail), detail.c_str());
  QVERIFY2(addCurveSource(service, otherWellFile, "B-2", 3, 40, otherWellId, detail), detail.c_str());
  std::vector<long long> wells, otherWells;
  QVERIFY2(service.listSourceWells(firstId, wells, detail), detail.c_str());
  QVERIFY2(service.listSourceWells(otherWellId, otherWells, detail), detail.c_str());
  QCOMPARE(wells.size(), size_t(1));
  QCOMPARE(otherWells.size(), size_t(1));
  service.closeProject();
  QVERIFY(QFile::remove(firstFile));
  QVERIFY(QFile::remove(otherFile));

  using namespace paleo::welltrack;
  QVector<CurveChoice> choices;
  QString error;
  QVERIFY2(listCurveChoices(model, "doc", wells.front(), choices, &error), qPrintable(error));
  QCOMPARE(choices.size(), 2);
  const auto &first = choices.at(0);
  const auto &other = choices.at(1);
  QCOMPARE(first.sourceId, firstId);
  QCOMPARE(other.sourceId, otherId);
  QCOMPARE(first.fileName, QStringLiteral("first.LAS"));
  QCOMPARE(first.filePath, firstFile);
  QCOMPARE(first.curveName, QStringLiteral("GR"));
  QCOMPARE(first.curveName, other.curveName);
  QVERIFY(first.curveId != other.curveId);

  paleo::document::SourceRecord source;
  source.ref = first.source;
  source.modelDirectory = model;
  source.fingerprint = paleo::document::SourceFingerprint::fromStored(first.source.fileRevisionId);
  AreaWellCurveWindow window;
  window.topMd = 8189.75 * 0.3048;
  window.bottomMd = 8191.25 * 0.3048;
  window.maxSamples = 2;
  window.maxReadBytes = 120000;

  const auto result = readCurve(source, wells.front(), first.logSetId, first.curveId, window);
  QVERIFY2(result.ok(), qPrintable(result.error));
  QCOMPARE(result.curve.name, QStringLiteral("GR"));
  QCOMPARE(result.curve.unit, QStringLiteral("API"));
  QCOMPARE(result.curve.modelSourceId, firstId);
  QCOMPARE(result.curve.modelLogSetId, first.logSetId);
  QCOMPARE(result.curve.depths, QVector<float>({float(8190 * 0.3048), float(8191 * 0.3048)}));
  QCOMPARE(result.curve.values, QVector<float>({8200, 8201}));
  QCOMPARE(result.curve.minScale, 8200.f);
  QCOMPARE(result.curve.maxScale, 8201.f);

  auto singleSample = window;
  singleSample.bottomMd = 8190.25 * 0.3048;
  const auto constant = readCurve(source, wells.front(), first.logSetId, first.curveId, singleSample);
  QVERIFY2(constant.ok(), qPrintable(constant.error));
  QVERIFY(constant.curve.minScale < 8200.f);
  QVERIFY(constant.curve.maxScale > 8200.f);
  QVERIFY(result.stats.chunked);
  QCOMPARE(result.stats.axisBlocksRead, 1LL);
  QCOMPARE(result.stats.valueBlocksRead, 1LL);
  QCOMPARE(result.stats.legacyBytesMaterialized, 0LL);

  QVERIFY(!readCurve(source, wells.front(), other.logSetId, other.curveId, window).ok());
  QVERIFY(!readCurve(source, otherWells.front(), first.logSetId, first.curveId, window).ok());
  QVERIFY(!readCurve(source, wells.front(), first.logSetId, first.curveId, window, [] { return true; }).ok());

  auto stale = source;
  stale.ref.fileRevisionId = QString(64, QLatin1Char('a'));
  stale.fingerprint = paleo::document::SourceFingerprint::fromStored(stale.ref.fileRevisionId);
  const auto rejected = readCurve(stale, wells.front(), first.logSetId, first.curveId, window);
  QVERIFY(!rejected.ok());
  QCOMPARE(rejected.stats.payloadBytesRead, 0LL);

  window.maxReadBytes = 64;
  const auto budgeted = readCurve(source, wells.front(), first.logSetId, first.curveId, window);
  QVERIFY(!budgeted.ok());
  QCOMPARE(budgeted.stats.payloadBytesRead, 0LL);
}

QTEST_GUILESS_MAIN(TestWellTrackSource)
#include "tst_welltracksource.moc"
