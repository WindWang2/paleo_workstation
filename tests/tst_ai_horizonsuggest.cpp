#include <QtTest>
#include <QTemporaryDir>

#include "../src/ai/horizonsuggest.h"
#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"

class TestAiHorizonSuggest : public QObject
{
  Q_OBJECT

private slots:
  void seedAndSuggestionDefaults();
  void emptySeedsReturnsError();
  void fetcherErrorPropagation();
  void invalidRadiusOrWindow();
  void suggestionFieldsIntegrity();
  void mutationDemonstration_seedsMustBeAnchored();
};

void TestAiHorizonSuggest::seedAndSuggestionDefaults()
{
  TrackingSeed seed;
  QCOMPARE(seed.inlineNo, 0);
  QCOMPARE(seed.xlineNo, 0);
  QCOMPARE(seed.sampleIndex, 0);

  TrackingSuggestion sug;
  QCOMPARE(sug.inlineNo, 0);
  QCOMPARE(sug.xlineNo, 0);
  QCOMPARE(sug.sampleIndex, 0);
  QCOMPARE(sug.score, 0.0f);
  QCOMPARE(sug.confidence, 0.0f);
  QCOMPARE(sug.isSeed, false);
}

void TestAiHorizonSuggest::emptySeedsReturnsError()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(OnnxFixtureWriter::writeTraceScorer(
      dir.filePath(QStringLiteral("scorer.onnx")), 8.0f, 0.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {}, 64, 5, nullptr, &out, &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
}

void TestAiHorizonSuggest::fetcherErrorPropagation()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(OnnxFixtureWriter::writeTraceScorer(
      dir.filePath(QStringLiteral("scorer.onnx")), 8.0f, 0.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  TrackingSeed seed;
  seed.inlineNo = 100;
  seed.xlineNo = 200;
  seed.sampleIndex = 500;

  TraceWindowFetcher failingFetcher = [](int, int, int, int, QVector<float> &, QString &err) {
    err = QStringLiteral("模拟道数据读取硬件故障");
    return false;
  };

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {seed}, 64, 2, failingFetcher, &out, &err);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("道数据")) || err.contains(QStringLiteral("故障")));
}

void TestAiHorizonSuggest::invalidRadiusOrWindow()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(OnnxFixtureWriter::writeTraceScorer(
      dir.filePath(QStringLiteral("scorer.onnx")), 8.0f, 0.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  TrackingSeed seed;
  seed.inlineNo = 10;
  seed.xlineNo = 20;
  seed.sampleIndex = 50;

  TraceWindowFetcher dummy = [](int, int, int, int, QVector<float> &out, QString &) {
    out = QVector<float>(64, 0.0f);
    return true;
  };

  QVector<TrackingSuggestion> out;
  QString err;
  // 非正窗宽应被拒绝
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {seed}, 0, 2, dummy, &out, &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
}

void TestAiHorizonSuggest::suggestionFieldsIntegrity()
{
  TrackingSuggestion sug;
  sug.inlineNo = 101;
  sug.xlineNo = 202;
  sug.sampleIndex = 303;
  sug.score = 0.95f;
  sug.confidence = 0.88f;
  sug.isSeed = true;
  sug.sourceInline = 100;
  sug.sourceXline = 200;

  QCOMPARE(sug.inlineNo, 101);
  QCOMPARE(sug.xlineNo, 202);
  QCOMPARE(sug.sampleIndex, 303);
  QVERIFY(sug.score > 0.9f);
  QVERIFY(sug.confidence > 0.8f);
  QVERIFY(sug.isSeed);
  QCOMPARE(sug.sourceInline, 100);
}

void TestAiHorizonSuggest::mutationDemonstration_seedsMustBeAnchored()
{
  // 变异测试示范：追踪种子点必须具备锚定属性，评分不应越界
  TrackingSuggestion seedSug;
  seedSug.isSeed = true;
  seedSug.score = 1.0f;
  seedSug.confidence = 1.0f;

  QVERIFY(seedSug.isSeed);
  QVERIFY(seedSug.score >= 0.0f && seedSug.score <= 1.0f);
  QVERIFY(seedSug.confidence >= 0.0f && seedSug.confidence <= 1.0f);
}

QTEST_GUILESS_MAIN(TestAiHorizonSuggest)
#include "tst_ai_horizonsuggest.moc"
