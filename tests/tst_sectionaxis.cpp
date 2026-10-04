// 层：测试壳（被测对象为 header-only 剖面轴换算 domain/seismic/sectionaxis.h）
#include <QtTest/QtTest>

#include "domain/seismic/sectionaxis.h"

#include <vector>

using namespace seismic;

class TestSectionAxis : public QObject
{
  Q_OBJECT

private slots:
  // #147：线距 >1 的列轴——列 ↔ 线号按实际表换算，不是 colMin+col。
  void nonUnitLineSpacing()
  {
    const std::vector<int> lines = {1000, 1002, 1004, 1006};
    int line = 0;
    QVERIFY(sectionLineForColumn(lines, 1000, 3, &line));
    QCOMPARE(line, 1006); // 旧实现 colMin+col = 1003（不存在的线）
    QVERIFY(!sectionLineForColumn(lines, 1000, 4, &line));
    QVERIFY(!sectionLineForColumn(lines, 1000, -1, &line));
    QCOMPARE(sectionColumnForLine(lines, 1000, 1004), 2); // 旧实现 = 4（越界）
    QCOMPARE(sectionColumnForLine(lines, 1000, 1003), -1); // 不在轴上 → 拒绝
    QCOMPARE(sectionColumnForLine(lines, 1000, 2000), -1);
    for (int c = 0; c < int(lines.size()); ++c)
    {
      QVERIFY(sectionLineForColumn(lines, 1000, c, &line));
      QCOMPARE(sectionColumnForLine(lines, 1000, line), c);
    }
  }

  // 空列轴 → 旧单位线距语义（兼容夹具与时间切片）。
  void emptyAxisFallsBackToColMin()
  {
    int line = 0;
    QVERIFY(sectionLineForColumn({}, 2000, 7, &line));
    QCOMPARE(line, 2007);
    QCOMPARE(sectionColumnForLine({}, 2000, 2007), 7);
  }

  // #146：样点 ↔ TWT 含记录延迟 t0（startTimeMs=100、dt=2：显示 1000 ms → 样点 450）。
  void timeOriginRoundTrip()
  {
    QCOMPARE(sectionSampleForTwt(1000.0, 100.0, 2.0), 450); // 旧实现 round(1000/2)=500
    QCOMPARE(sectionTwtForSample(450, 100.0, 2.0), 1000.0); // 旧实现 450*2=900
    QCOMPARE(sectionSampleForTwt(1001.2, 100.0, 2.0), 451);
    QCOMPARE(sectionSampleForTwt(1000.0, 0.0, 2.0), 500);
    QCOMPARE(sectionSampleForTwt(1000.0, 0.0, 0.0), -1);
    for (int s : {0, 1, 17, 999})
      QCOMPARE(sectionSampleForTwt(sectionTwtForSample(s, 100.0, 4.0), 100.0, 4.0), s);
  }
};

QTEST_MAIN(TestSectionAxis)
#include "tst_sectionaxis.moc"
