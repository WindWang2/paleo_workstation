#include <QtTest>
#include <QFile>

#include "../src/io/timedeptool.h"

// plan §3/§10：TVD 对 TVD 列线性插值；井分层 TVD 空用 MD 对 MD 列。
// 行保持文件顺序——不排序、不夹取、不外推；-99999 行在单调性与样点数
// 检查之前剔除。结果是 Ok+ms 或原因码（无时深表/超出时深表/时深表无序）。
class TestTimeDepthTool : public QObject
{
  Q_OBJECT

private slots:
  void interpolatesOnTvdColumn();
  void keepsFileOrderNoSorting();
  void fallsBackToMdColumn();
  void skipsSentinelRows();
  void nonMonotonicColumnFails();
  void outOfRangeNeverClamps();
  void tooFewSamplesIsNoTable();
  void reasonTextMatchesPlan();

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
  const TimeDepthTool::TdResult mid = TimeDepthTool::interpolateTimeMs(td, 150.0, false);
  QVERIFY(mid.ok());
  QCOMPARE(mid.status, TimeDepthTool::TdStatus::Ok);
  QCOMPARE(mid.timeMs, 150.0);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 250.0, false).timeMs, 250.0);
  // 首末样点本身在范围内（f=0 / f=1），取整点时间。
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 100.0, false).timeMs, 100.0);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, 300.0, false).timeMs, 300.0);
}

void TestTimeDepthTool::keepsFileOrderNoSorting()
{
  // TVD 列文件序严格递增，但 TIME 列不按任何键排序：配对用文件序相邻行。
  // 文件序 (100→100ms, 200→400ms, 300→300ms)：TVD=150 落在第一对之间。
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "400 -400 200 205\n"
      "300 -300 300 310\n");
  const TimeDepthTool::TdResult r = TimeDepthTool::interpolateTimeMs(td, 150.0, false);
  QVERIFY2(r.ok(), "TVD column is strictly increasing in file order → Ok");
  QCOMPARE(r.timeMs, 250.0); // 100 + (150-100)/(200-100) * (400-100)

  // 查找列不按文件序递增 → 不排序修补，直接判无序。
  const TimeDepthTable unsorted = fromText(
      "# Well : A1\n"
      "100 -100 200 200\n"
      "200 -200 100 100\n"
      "300 -300 300 300\n");
  const TimeDepthTool::TdResult bad = TimeDepthTool::interpolateTimeMs(unsorted, 150.0, false);
  QCOMPARE(bad.status, TimeDepthTool::TdStatus::NonMonotonic);
  QVERIFY(qIsNaN(bad.timeMs));
}

void TestTimeDepthTool::fallsBackToMdColumn()
{
  // TVD 列被 -99999 打穿 → 调用方用 useMd=true 走 MD 列兜底。
  const TimeDepthTable td = fromText(
      "#TimeDepth File From SMI\n# Well : A1\n"
      "#TIME TVDSS TVD MD\n"
      "100 -100 -99999 100\n"
      "200 -200 -99999 250\n"
      "300 -300 -99999 400\n");
  const TimeDepthTool::TdResult r = TimeDepthTool::interpolateTimeMs(td, 175.0, true); // MD=175
  QVERIFY(r.ok());
  QCOMPARE(r.timeMs, 150.0);
  // useMd=false 时无 TVD 可用行 → 无时深表。
  const TimeDepthTool::TdResult none = TimeDepthTool::interpolateTimeMs(td, 175.0, false);
  QCOMPARE(none.status, TimeDepthTool::TdStatus::NoTable);
  QVERIFY(qIsNaN(none.timeMs));
}

void TestTimeDepthTool::skipsSentinelRows()
{
  // 中间一行 TVD=-99999：剔除后 [100,200] 仍单调，插值正常。
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "150 -150 -99999 160\n"
      "200 -200 200 210\n");
  const TimeDepthTool::TdResult r = TimeDepthTool::interpolateTimeMs(td, 150.0, false);
  QVERIFY(r.ok());
  QCOMPARE(r.timeMs, 150.0);

  // 剔除在单调性检查之前：哨兵行不制造无序，也不补足样点数。
  const TimeDepthTable sparse = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "150 -150 -99999 160\n"
      "160 -160 -99999 170\n"
      "200 -200 200 210\n");
  QVERIFY(TimeDepthTool::interpolateTimeMs(sparse, 150.0, false).ok());
}

void TestTimeDepthTool::nonMonotonicColumnFails()
{
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "200 -200 300 300\n"
      "300 -300 200 210\n"); // TVD 100→300→200 非递增
  const TimeDepthTool::TdResult r = TimeDepthTool::interpolateTimeMs(td, 250.0, false);
  QCOMPARE(r.status, TimeDepthTool::TdStatus::NonMonotonic);
  QVERIFY(qIsNaN(r.timeMs));
  QCOMPARE(TimeDepthTool::reasonText(r.status), QStringLiteral("时深表无序"));

  // 相等键也不满足严格递增。
  const TimeDepthTable flat = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "200 -200 100 110\n"
      "300 -300 200 210\n");
  QCOMPARE(TimeDepthTool::interpolateTimeMs(flat, 150.0, false).status,
           TimeDepthTool::TdStatus::NonMonotonic);
}

void TestTimeDepthTool::outOfRangeNeverClamps()
{
  const TimeDepthTable td = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n"
      "200 -200 200 210\n");
  const TimeDepthTool::TdResult shallow = TimeDepthTool::interpolateTimeMs(td, 50.0, false);
  QCOMPARE(shallow.status, TimeDepthTool::TdStatus::OutOfRange);
  QVERIFY(qIsNaN(shallow.timeMs)); // 不夹取：端点值 100 绝不返回
  QVERIFY(shallow.timeMs != 100.0);
  QCOMPARE(TimeDepthTool::reasonText(shallow.status), QStringLiteral("超出时深表"));

  const TimeDepthTool::TdResult deep = TimeDepthTool::interpolateTimeMs(td, 500.0, false);
  QCOMPARE(deep.status, TimeDepthTool::TdStatus::OutOfRange);
  QVERIFY(qIsNaN(deep.timeMs));
  QVERIFY(deep.timeMs != 200.0);

  // NaN 深度同样不在范围内。
  QCOMPARE(TimeDepthTool::interpolateTimeMs(td, qQNaN(), false).status,
           TimeDepthTool::TdStatus::OutOfRange);
}

void TestTimeDepthTool::tooFewSamplesIsNoTable()
{
  // 空表、单行、哨兵占满 → 都判无时深表（样点不足两个）。
  QCOMPARE(TimeDepthTool::interpolateTimeMs(TimeDepthTable(), 100.0, false).status,
           TimeDepthTool::TdStatus::NoTable);

  const TimeDepthTable one = fromText(
      "# Well : A1\n"
      "100 -100 100 100\n");
  QCOMPARE(TimeDepthTool::interpolateTimeMs(one, 100.0, false).status,
           TimeDepthTool::TdStatus::NoTable);

  const TimeDepthTable sentinels = fromText(
      "# Well : A1\n"
      "100 -100 -99999 -99999\n"
      "200 -200 -99999 -99999\n");
  QCOMPARE(TimeDepthTool::interpolateTimeMs(sentinels, 100.0, false).status,
           TimeDepthTool::TdStatus::NoTable);
  QCOMPARE(TimeDepthTool::interpolateTimeMs(sentinels, 100.0, true).status,
           TimeDepthTool::TdStatus::NoTable);
  QCOMPARE(TimeDepthTool::reasonText(TimeDepthTool::TdStatus::NoTable),
           QStringLiteral("无时深表"));
}

void TestTimeDepthTool::reasonTextMatchesPlan()
{
  QCOMPARE(TimeDepthTool::reasonText(TimeDepthTool::TdStatus::Ok), QString());
  QCOMPARE(TimeDepthTool::reasonText(TimeDepthTool::TdStatus::NoTable),
           QStringLiteral("无时深表"));
  QCOMPARE(TimeDepthTool::reasonText(TimeDepthTool::TdStatus::OutOfRange),
           QStringLiteral("超出时深表"));
  QCOMPARE(TimeDepthTool::reasonText(TimeDepthTool::TdStatus::NonMonotonic),
           QStringLiteral("时深表无序"));
}

QTEST_MAIN(TestTimeDepthTool)
#include "tst_timedeptool.moc"
