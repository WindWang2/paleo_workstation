#include <QtTest>
#include <QFile>

#include "../src/io/timedeptool.h"

// plan §10：TVD 对 TVD 列插值；TVD 空（-99999）用 MD 对 MD 列；
// -99999 行不进插值；端点外推；无可用行失败。
class TestTimeDepthTool : public QObject
{
  Q_OBJECT

private slots:
  void interpolatesOnTvdColumn();
  void fallsBackToMdColumn();
  void skipsSentinelRows();
  void extrapolatesAtEnds();
  void failsWhenNoUsableRows();

private:
  static TimeDepthTable fromText(const QByteArray &t) { return parseTimeDepthText(t); }
};

void TestTimeDepthTool::interpolatesOnTvdColumn()
{
  const TimeDepthTable td = fromText(
      "#TimeDepth File From SMI\n# Well : A1\n"
      "#TIME TVDSS TVD MD\n"
      "100 -100 100 100\n"
      "200 -200 200 205\n"
      "300 -300 300 310\n");
  bool ok = false;
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 150.0, false, &ok), 150.0);
  QVERIFY(ok);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 250.0, false, &ok), 250.0);
}

void TestTimeDepthTool::fallsBackToMdColumn()
{
  // TVD 列被 -99999 打穿 → MD 列兜底（时间与 TVD 不同斜率，结果不同）。
  const TimeDepthTable td = fromText(
      "#TimeDepth File From SMI\n# Well : A1\n"
      "#TIME TVDSS TVD MD\n"
      "100 -100 -99999 100\n"
      "200 -200 -99999 250\n"
      "300 -300 -99999 400\n");
  bool ok = false;
  const double t = TimeDepthTool::interpolateTimeMs(td, 175.0, true, &ok); // MD=175
  QVERIFY(ok);
  QCOMPARE(t, 150.0);
  // useMd=false 时无 TVD 可用行 → 失败
  QVERIFY(!TimeDepthTool::interpolateTimeMs(td, 175.0, false, &ok) && !ok);
}

void TestTimeDepthTool::skipsSentinelRows()
{
  // 中间一行 TVD=-99999：不得参与插值（否则会拉低区间斜率）。
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "150 -150 -99999 160\n"
      "200 -200 200 210\n");
  bool ok = false;
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 150.0, false, &ok), 150.0);
  QVERIFY(ok);
}

void TestTimeDepthTool::extrapolatesAtEnds()
{
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "200 -200 200 210\n");
  bool ok = false;
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 50.0, false, &ok), 100.0);
  QVERIFY(ok);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 500.0, false, &ok), 200.0);
}

void TestTimeDepthTool::failsWhenNoUsableRows()
{
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 -99999 -99999\n");
  bool ok = true;
  TimeDepthTool::interpolateTimeMs(td, 100.0, false, &ok);
  QVERIFY(!ok);
  TimeDepthTool::interpolateTimeMs(td, 100.0, true, &ok);
  QVERIFY(!ok);
}

QTEST_MAIN(TestTimeDepthTool)
#include "tst_timedeptool.moc"
