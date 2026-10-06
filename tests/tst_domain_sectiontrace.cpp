#include <QtTest>

#include "../src/domain/sectiontrace.h"

class TestDomainSectionTrace : public QObject
{
  Q_OBJECT

private slots:
  void segyTraceAndGeometryDefaults();
  void gridForEmptyTraces();
  void gridForTracesCalculation();
  void sampleAtInterpolation();
  void sampleAtEdgeCases();
  void mutationDemonstration_interpolationBounds();
};

void TestDomainSectionTrace::segyTraceAndGeometryDefaults()
{
  SegyTrace trace;
  QCOMPARE(trace.cdp, 0);
  QCOMPARE(trace.lineNo, 0);
  QCOMPARE(trace.xlineNo, 0);
  QVERIFY(trace.samples.isEmpty());
  QVERIFY(std::isnan(trace.startTimeMs));

  SegyGeometry geom;
  QCOMPARE(geom.inlineMin, 0);
  QCOMPARE(geom.inlineMax, 0);
  QCOMPARE(geom.startTimeMs, 0.0);
}

void TestDomainSectionTrace::gridForEmptyTraces()
{
  QVector<SegyTrace> empty;
  const SegySectionGrid g1 = SegySectionGrid::forTraces(empty, 2000.0f, 0.0);
  QCOMPARE(g1.startMs, 0.0);
  QCOMPARE(g1.rows, 1);

  // 包含空样本文档
  SegyTrace emptySampleTrace;
  emptySampleTrace.samples.clear();
  QVector<SegyTrace> traces{emptySampleTrace};
  const SegySectionGrid g2 = SegySectionGrid::forTraces(traces, 2000.0f, 0.0);
  QCOMPARE(g2.rows, 1);
}

void TestDomainSectionTrace::gridForTracesCalculation()
{
  QVector<SegyTrace> traces;
  SegyTrace t1;
  t1.sampleIntervalUs = 2000.0f; // 2 ms
  t1.startTimeMs = 100.0;
  t1.samples = QVector<float>(100, 1.0f); // 100 采样点，跨度 (100 - 1)*2 = 198 ms
  traces.append(t1);

  SegyTrace t2;
  t2.sampleIntervalUs = 2000.0f;
  t2.startTimeMs = 150.0;
  t2.samples = QVector<float>(100, 2.0f);
  traces.append(t2);

  const SegySectionGrid grid = SegySectionGrid::forTraces(traces, 2000.0f, 0.0);
  QCOMPARE(grid.startMs, 100.0);
  QVERIFY(grid.rows > 1);
  QVERIFY(grid.endMs() >= 348.0); // 150 + 198 = 348 ms
}

void TestDomainSectionTrace::sampleAtInterpolation()
{
  SegyTrace trace;
  trace.sampleIntervalUs = 1000.0f; // 1 ms
  trace.startTimeMs = 10.0;
  trace.samples = {10.0f, 20.0f, 30.0f, 40.0f}; // 4 个点：10ms(10), 11ms(20), 12ms(30), 13ms(40)

  SegySectionGrid grid;
  grid.startMs = 10.0;
  grid.stepMs = 0.5; // 每半毫秒一行
  grid.rows = 7;

  float val = 0.0f;
  // 采样第 0 行：时间 10.0ms -> 采样点 0 -> 10.0f
  QVERIFY(grid.sampleAt(trace, 0, 1000.0f, 0.0, &val));
  QCOMPARE(val, 10.0f);

  // 采样第 1 行：时间 10.5ms -> 介于 10 与 20 之间线性插值 -> 15.0f
  QVERIFY(grid.sampleAt(trace, 1, 1000.0f, 0.0, &val));
  QCOMPARE(val, 15.0f);

  // 采样第 2 行：时间 11.0ms -> 20.0f
  QVERIFY(grid.sampleAt(trace, 2, 1000.0f, 0.0, &val));
  QCOMPARE(val, 20.0f);
}

void TestDomainSectionTrace::sampleAtEdgeCases()
{
  SegyTrace trace;
  trace.sampleIntervalUs = 2000.0f;
  trace.startTimeMs = 50.0;
  trace.samples = {1.0f, 2.0f};

  SegySectionGrid grid;
  grid.startMs = 0.0;
  grid.stepMs = 10.0;
  grid.rows = 20;

  float val = 0.0f;
  // 空指针防护
  QVERIFY(!grid.sampleAt(trace, 5, 2000.0f, 50.0, nullptr));

  // 越界：在 trace 起始时间之前（row 0 为 0ms < 50ms）
  QVERIFY(!grid.sampleAt(trace, 0, 2000.0f, 50.0, &val));

  // 越界：在 trace 结束之后（row 10 为 100ms > 52ms）
  QVERIFY(!grid.sampleAt(trace, 10, 2000.0f, 50.0, &val));
}

void TestDomainSectionTrace::mutationDemonstration_interpolationBounds()
{
  // 变异测试示范：线性插值权重必须严格在 [lo, hi] 范围内
  SegyTrace trace;
  trace.sampleIntervalUs = 1000.0f;
  trace.startTimeMs = 0.0;
  trace.samples = {100.0f, 200.0f};

  SegySectionGrid grid;
  grid.startMs = 0.0;
  grid.stepMs = 0.25;
  grid.rows = 5;

  for (int r = 0; r <= 4; ++r)
  {
    float sample = 0.0f;
    QVERIFY(grid.sampleAt(trace, r, 1000.0f, 0.0, &sample));
    QVERIFY(sample >= 100.0f && sample <= 200.0f);
  }
}

QTEST_GUILESS_MAIN(TestDomainSectionTrace)
#include "tst_domain_sectiontrace.moc"
