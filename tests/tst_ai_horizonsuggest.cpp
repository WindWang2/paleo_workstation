#include <QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <tuple>

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
  void cooperativeCancellationStopsExpansion();
  void logitScorerRejectedOutsideProbabilityDomain();
  // #479 去平方化（QSet 去重 + 最近邻距离场）的行为锚定：多种子扩张的
  // 建议 source 归属必须与朴素参考实现（全量扫描 + 平手取小 inline/xline）
  // 逐条一致，重叠邻域不重复出建议。
  void multiSeedExpansionMatchesNaiveNearestAnchor();
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
                                         {}, 64, 5, nullptr, {}, &out, &err);
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
                                         {seed}, 64, 2, failingFetcher, {}, &out, &err);
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
                                         {seed}, 0, 2, dummy, {}, &out, &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
}

void TestAiHorizonSuggest::suggestionFieldsIntegrity()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(OnnxFixtureWriter::writeTraceScorer(
      dir.filePath(QStringLiteral("scorer.onnx")), 8.0f, 0.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  TrackingSeed a;
  a.inlineNo = 10;
  a.xlineNo = 20;
  a.sampleIndex = 500;
  TrackingSeed b;
  b.inlineNo = 10;
  b.xlineNo = 24;
  b.sampleIndex = 700;

  TraceWindowFetcher fetch = [](int, int, int, int, QVector<float> &out, QString &) {
    out = QVector<float>(64, 0.0f); // 全零道窗 → p≡0.5（概率域内）
    return true;
  };

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {a, b}, 64, 2, fetch, {}, &out, &err);
  QVERIFY2(ok, qPrintable(err));
  QVERIFY2(out.size() > 2, qPrintable(QString::number(out.size())));

  // 字段完整性：跑真实引擎后逐条查输出域与追溯链——原实现只在本函数内给
  // 结构体赋字面量再读回，无论 suggestHorizonTracking 怎么坏都恒绿。
  const auto pickedAt = [&out](int il, int xl) {
    for (const TrackingSuggestion &s : out)
      if (s.inlineNo == il && s.xlineNo == xl)
        return true;
    return false;
  };
  int seeds = 0;
  for (const TrackingSuggestion &s : out)
  {
    // 输出域：score/confidence 恒在概率域（logits 型 scorer 由域守卫整跑拒绝）
    QVERIFY2(s.score >= 0.0f && s.score <= 1.0f,
             qPrintable(QStringLiteral("score=%1").arg(double(s.score))));
    QVERIFY2(s.confidence >= 0.0f && s.confidence <= 1.0f,
             qPrintable(QStringLiteral("confidence=%1").arg(double(s.confidence))));
    // 追溯链：来源道必须是本次输出里的某一条已定拾取（否则建议无锚可查）
    QVERIFY2(pickedAt(s.sourceInline, s.sourceXline),
             qPrintable(QStringLiteral("来源 (%1,%2) 不在输出中")
                            .arg(s.sourceInline)
                            .arg(s.sourceXline)));
    if (s.isSeed)
    {
      ++seeds;
      QCOMPARE(s.sourceInline, s.inlineNo); // 种子行自引用（锚定属性）
      QCOMPARE(s.sourceXline, s.xlineNo);
      QCOMPARE(s.score, 1.0f); // 解释员给定的锚不经模型：满置信
      QCOMPARE(s.confidence, 1.0f);
    }
    else
    {
      QVERIFY2(!(s.sourceInline == s.inlineNo && s.sourceXline == s.xlineNo),
               "扩张建议不得把自己当来源");
    }
  }
  QCOMPARE(seeds, 2);
}

void TestAiHorizonSuggest::cooperativeCancellationStopsExpansion()
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

  // 闸门式取消（非 sleep）：fetch 计数到 3 后置取消位，引擎必须在下一候选
  // 道前停——不跑完 288 个候选道，也不把半途建议当成功外发。
  int calls = 0;
  TraceWindowFetcher fetch = [&calls](int, int, int, int, QVector<float> &out, QString &) {
    out = QVector<float>(64, 0.0f);
    ++calls;
    return true;
  };
  auto cancelled = [&calls]() { return calls >= 3; };

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {seed}, 64, 8, fetch, cancelled, &out, &err);
  QVERIFY(!ok);
  QVERIFY2(err.contains(QStringLiteral("已取消")), qPrintable(err));
  QVERIFY(calls < (2 * 8 + 1) * (2 * 8 + 1));
  QVERIFY(out.isEmpty());
}

// 输出域守卫：logits 型 scorer（无 Sigmoid，值域无界）必须被整跑拒绝——
// 修前 score>1、置信恒 1（bernoulliConfidence 钳位），解释员看到全置信
// 建议被误导裁决。
void TestAiHorizonSuggest::logitScorerRejectedOutsideProbabilityDomain()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  // gain=8, offset=2：x∈[-1,1] 的道窗 → 输出 [-6,10]，恒越 [0,1]
  QVERIFY(OnnxFixtureWriter::writeTraceLogitScorer(
      dir.filePath(QStringLiteral("logit.onnx")), 8.0f, 2.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  TrackingSeed seed;
  seed.inlineNo = 10;
  seed.xlineNo = 20;
  seed.sampleIndex = 50;

  TraceWindowFetcher fetch = [](int, int, int, int, QVector<float> &out, QString &) {
    out = QVector<float>(64, 0.25f); // 8*0.25+2 = 4.0 > 1
    return true;
  };

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("logit"),
                                         {seed}, 64, 2, fetch, {}, &out, &err);
  QVERIFY(!ok);
  QVERIFY2(err.contains(QStringLiteral("概率域")), qPrintable(err));
  QVERIFY(out.isEmpty());
}

QTEST_GUILESS_MAIN(TestAiHorizonSuggest)
#include "tst_ai_horizonsuggest.moc"

// #479：多种子成功路径的行为锚定。参考实现 = 旧 O(n·m) 算法的逐字复刻
// （QVector find_if 去重 + 逐候选全量扫 picked 最近锚），新实现（QSet 去重 +
// 增量距离场）的输出必须与其逐字段一致——扩张序、去重、最近锚归属（含
// 「已拾取的扩张建议也可作锚」）、sampleIndex 路由全部对照。
// scorer 用 sigmoid(gain=8)：全零道窗 → p≡0.5 → bestIdx 恒 0 → 每条扩张
// 建议 sampleIndex = 锚点 sample − window/2（确定性）。
void TestAiHorizonSuggest::multiSeedExpansionMatchesNaiveNearestAnchor()
{
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("onnxruntime 不在 vendor 树中");

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QVERIFY(OnnxFixtureWriter::writeTraceScorer(
      dir.filePath(QStringLiteral("scorer.onnx")), 8.0f, 0.0f));
  PaleoOnnxService svc;
  svc.setModelRoot(dir.path());

  TrackingSeed a;
  a.inlineNo = 10;
  a.xlineNo = 20;
  a.sampleIndex = 500;
  TrackingSeed b;
  b.inlineNo = 10;
  b.xlineNo = 24;
  b.sampleIndex = 700;
  const int radius = 2;
  const int window = 64;

  TraceWindowFetcher fetch = [](int, int, int, int, QVector<float> &out, QString &) {
    out = QVector<float>(64, 0.0f);
    return true;
  };

  QVector<TrackingSuggestion> out;
  QString err;
  const bool ok = suggestHorizonTracking(&svc, QStringLiteral("scorer"),
                                         {a, b}, window, radius, fetch, {},
                                         &out, &err);
  QVERIFY2(ok, qPrintable(err));

  // ---- 朴素参考（旧算法复刻，O(n²)/O(n·m) 原样保留作语义基准）----
  struct Naive
  {
    int il, xl, sample, srcIl, srcXl;
    bool seed;
  };
  QVector<Naive> picked;
  const auto pickedAt = [&picked](int il, int xl) -> int {
    for (int i = 0; i < picked.size(); ++i)
      if (picked[i].il == il && picked[i].xl == xl)
        return i;
    return -1;
  };
  const TrackingSeed seedArr[2] = {a, b};
  for (const TrackingSeed &s : seedArr)
    if (pickedAt(s.inlineNo, s.xlineNo) < 0)
      picked.append({s.inlineNo, s.xlineNo, s.sampleIndex, s.inlineNo,
                     s.xlineNo, true});

  struct Cand
  {
    int il, xl, dist;
  };
  QVector<Cand> order;
  for (const TrackingSeed &s : seedArr)
    for (int dil = -radius; dil <= radius; ++dil)
      for (int dxl = -radius; dxl <= radius; ++dxl)
      {
        const int il = s.inlineNo + dil, xl = s.xlineNo + dxl;
        if (pickedAt(il, xl) >= 0)
          continue;
        bool dup = false;
        for (const Cand &c : order)
          if (c.il == il && c.xl == xl) { dup = true; break; }
        if (!dup)
          order.append({il, xl, std::max(std::abs(dil), std::abs(dxl))});
      }
  std::sort(order.begin(), order.end(), [](const Cand &x, const Cand &y) {
    return std::tie(x.dist, x.il, x.xl) < std::tie(y.dist, y.il, y.xl);
  });
  for (const Cand &c : order)
  {
    // 最近已拾取锚：全量扫 picked（d2 最小，平手取小 il 再小 xl）。
    int bestIdx = -1, bestD2 = std::numeric_limits<int>::max();
    for (int i = 0; i < picked.size(); ++i)
    {
      const int dIl = picked[i].il - c.il;
      const int dXl = picked[i].xl - c.xl;
      const int d2 = dIl * dIl + dXl * dXl;
      if (d2 < bestD2 ||
          (d2 == bestD2 && bestIdx >= 0 &&
           (picked[i].il < picked[bestIdx].il ||
            (picked[i].il == picked[bestIdx].il &&
             picked[i].xl < picked[bestIdx].xl)))) {
        bestD2 = d2;
        bestIdx = i;
      }
    }
    QVERIFY(bestIdx >= 0);
    picked.append({c.il, c.xl, picked[bestIdx].sample - window / 2,
                   picked[bestIdx].il, picked[bestIdx].xl, false});
  }

  // ---- 对照：位置集合一致，逐字段一致（sampleIndex/source/isSeed）----
  QCOMPARE(out.size(), picked.size());
  for (const TrackingSuggestion &s : out)
  {
    const int idx = pickedAt(s.inlineNo, s.xlineNo);
    QVERIFY2(idx >= 0, "新实现给出了参考实现没有的建议（去重/覆盖回归）");
    QCOMPARE(s.isSeed, picked[idx].seed);
    QCOMPARE(s.sampleIndex, picked[idx].sample);
    QCOMPARE(s.sourceInline, picked[idx].srcIl);
    QCOMPARE(s.sourceXline, picked[idx].srcXl);
  }
  QCOMPARE(out.size(), 45); // 并集 25+25−5 重叠格：种子 2 + 扩张 43
}
