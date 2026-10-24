// PW-020/021/103：共享曲线刻度映射（ui/common/curvescale.h）。
#include <QtTest>

#include "ui/common/curvescale.h"

using paleo::curvescale::normalise;
using paleo::curvescale::normaliseClamped;

class TestCurveScale : public QObject
{
  Q_OBJECT
private slots:
  void linear()
  {
    QCOMPARE(normalise(50, 0, 100, false), 0.5);
    QCOMPARE(normaliseClamped(150, 0, 100, false), 1.0);
  }
  void reversedFlipsInsteadOfCollapsing()
  {
    // NPHI 0.45 → -0.15：0.45 在左缘、-0.15 在右缘、0.15 居中。
    QCOMPARE(normalise(0.45, 0.45, -0.15, false), 0.0);
    QCOMPARE(normalise(-0.15, 0.45, -0.15, false), 1.0);
    QVERIFY(qAbs(normalise(0.15, 0.45, -0.15, false) - 0.5) < 1e-12);
    // DT 140 → 40
    QCOMPARE(normalise(90, 140, 40, false), 0.5);
  }
  void logScale()
  {
    // 电阻率 0.2–2000：每十倍程占 1/4 道宽。
    QVERIFY(qAbs(normalise(2.0, 0.2, 2000, true) - 0.25) < 1e-12);
    QVERIFY(qAbs(normalise(200.0, 0.2, 2000, true) - 0.75) < 1e-12);
    QVERIFY(std::isnan(normalise(0.0, 0.2, 2000, true)));
    QVERIFY(std::isnan(normalise(-1.0, 0.2, 2000, true)));
    QVERIFY(std::isnan(normalise(1.0, 0.0, 2000, true)));
    // 反向对数
    QVERIFY(qAbs(normalise(2.0, 2000, 0.2, true) - 0.75) < 1e-12);
  }
  void degenerate()
  {
    QVERIFY(std::isnan(normalise(5, 5, 5, false)));
    QVERIFY(std::isnan(normalise(qQNaN(), 0, 1, false)));
  }
};

QTEST_APPLESS_MAIN(TestCurveScale)
#include "tst_curvescale.moc"
