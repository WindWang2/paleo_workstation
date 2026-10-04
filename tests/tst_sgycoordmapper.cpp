// tst_sgycoordmapper — SgyCoordinateMapper 仿射拟合数值稳定性（#135）。
//
// 旧实现用未中心化的 3x3 法方程 + Cramer 法则：inline/xline ~1e3、
// 坐标 ~1e5..1e6（UTM）时灾难性抵消，精确仿射的 2x4 测网拟出 rms=2m
// （FMA 收缩下 6404m）、5x5 测网被 25m 门限拒绝。这里钉住：精确仿射
// 测网残差≈0、正反映射闭合、与坐标绝对量级无关、退化测网如实拒绝。
#include <QtTest>

#include <cmath>

#include "Data/Sgy/SgyCoordinateMapper.h"

using seismic::SgyCoordinateMapper;
using seismic::SgyCoordinateSample;
using seismic::SgyIndex;

namespace
{
struct Survey
{
  int inlines = 0;
  int xlines = 0;
  double originX = 0.0;
  double originY = 0.0;
  double rotationRad = 0.0;
  double xlineBin = 25.0;
  double inlineBin = 25.0;
};

void truth(const Survey &s, double il, double xl, double &x, double &y)
{
  const double i = il - 1000.0;
  const double j = xl - 2000.0;
  const double c = std::cos(s.rotationRad);
  const double sn = std::sin(s.rotationRad);
  x = s.originX + s.xlineBin * j * c - s.inlineBin * i * sn;
  y = s.originY + s.xlineBin * j * sn + s.inlineBin * i * c;
}

SgyIndex makeIndex(const Survey &s)
{
  SgyIndex idx;
  idx.coordinateFieldsPresent = true;
  idx.inlineMin = 1000;
  idx.inlineMax = 1000 + s.inlines - 1;
  idx.xlineMin = 2000;
  idx.xlineMax = 2000 + s.xlines - 1;
  for (int i = 0; i < s.inlines; ++i)
    for (int j = 0; j < s.xlines; ++j)
    {
      SgyCoordinateSample sample;
      sample.inlineNo = 1000 + i;
      sample.xlineNo = 2000 + j;
      truth(s, sample.inlineNo, sample.xlineNo, sample.x, sample.y);
      idx.coordinateSamples.push_back(sample);
    }
  return idx;
}
} // namespace

class TestSgyCoordMapper : public QObject
{
  Q_OBJECT
private slots:
  void exactAffineSurveys_data()
  {
    QTest::addColumn<int>("inlines");
    QTest::addColumn<int>("xlines");
    QTest::addColumn<double>("originX");
    QTest::addColumn<double>("originY");
    QTest::addColumn<double>("rotationDeg");
    // perffixtures::makeSyntheticSegy 的几何（tst_crossplot_samples 曾因此恒红）
    QTest::newRow("fixture-2x4-utm") << 2 << 4 << 500000.0 << 4000000.0 << 0.0;
    QTest::newRow("5x5-utm") << 5 << 5 << 500000.0 << 4000000.0 << 0.0;
    QTest::newRow("5x5-utm-rot30") << 5 << 5 << 512345.0 << 4123456.0 << 30.0;
    QTest::newRow("10x20-utm") << 10 << 20 << 500000.0 << 4000000.0 << 0.0;
    QTest::newRow("2x400-utm") << 2 << 400 << 500000.0 << 4000000.0 << 0.0;
    QTest::newRow("60x80-utm-rot-17") << 60 << 80 << 712000.0 << 3456789.0 << -17.0;
    QTest::newRow("5x5-small-coords") << 5 << 5 << 1000.0 << 5000.0 << 0.0;
  }
  void exactAffineSurveys()
  {
    QFETCH(int, inlines);
    QFETCH(int, xlines);
    QFETCH(double, originX);
    QFETCH(double, originY);
    QFETCH(double, rotationDeg);
    Survey s;
    s.inlines = inlines;
    s.xlines = xlines;
    s.originX = originX;
    s.originY = originY;
    s.rotationRad = rotationDeg * 3.14159265358979323846 / 180.0; // 不用 M_PI：MSVC 需 _USE_MATH_DEFINES
    const SgyIndex idx = makeIndex(s);
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    QVERIFY2(mapper.valid(), mapper.fit().rejectionReason.c_str());
    QVERIFY2(mapper.fit().rmsResidual < 1e-6,
             qPrintable(QStringLiteral("rms=%1").arg(mapper.fit().rmsResidual)));
    QVERIFY(mapper.fit().maxResidual < 1e-6);

    // 正映射对真值、反映射闭合（含测网外一格）
    for (const auto &[il, xl] : {std::pair{1000.0, 2000.0},
                                 std::pair{double(1000 + inlines - 1), double(2000 + xlines - 1)},
                                 std::pair{999.0, 1999.5}})
    {
      double x = 0, y = 0, ex = 0, ey = 0;
      QVERIFY(mapper.MapInlineXline(il, xl, x, y));
      truth(s, il, xl, ex, ey);
      QVERIFY2(std::hypot(x - ex, y - ey) < 1e-6,
               qPrintable(QStringLiteral("forward err %1").arg(std::hypot(x - ex, y - ey))));
      double il2 = 0, xl2 = 0;
      QVERIFY(mapper.MapXY(x, y, il2, xl2));
      QVERIFY(std::abs(il2 - il) < 1e-6);
      QVERIFY(std::abs(xl2 - xl) < 1e-6);
    }
  }

  void noisySurveyStillWithinTolerance()
  {
    // 坐标按整米写入道头（真实文件常见）：残差应为亚米级而非被拒。
    Survey s{12, 30, 500000.0, 4000000.0, 0.4, 12.5, 25.0};
    SgyIndex idx = makeIndex(s);
    for (auto &sample : idx.coordinateSamples)
    {
      sample.x = std::round(sample.x);
      sample.y = std::round(sample.y);
    }
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx);
    QVERIFY2(mapper.valid(), mapper.fit().rejectionReason.c_str());
    QVERIFY(mapper.fit().maxResidual < 1.0);
  }

  void degenerateSurveysRejected()
  {
    Survey line{1, 10, 500000.0, 4000000.0, 0.0};
    const SgyCoordinateMapper single = SgyCoordinateMapper::Fit(makeIndex(line));
    QVERIFY(!single.valid());
    QVERIFY(QString::fromStdString(single.fit().rejectionReason).contains("degenerate"));

    // inline 与 xline 同步递增（对角线采样）——共线
    SgyIndex diag;
    diag.coordinateFieldsPresent = true;
    for (int k = 0; k < 10; ++k)
    {
      SgyCoordinateSample sample;
      sample.inlineNo = 1000 + k;
      sample.xlineNo = 2000 + k;
      sample.x = 500000.0 + 25.0 * k;
      sample.y = 4000000.0 + 25.0 * k;
      diag.coordinateSamples.push_back(sample);
    }
    QVERIFY(!SgyCoordinateMapper::Fit(diag).valid());

    SgyIndex few = makeIndex(Survey{1, 5, 0.0, 0.0, 0.0});
    QVERIFY(!SgyCoordinateMapper::Fit(few).valid());
  }

  void irregularSurveyRejected()
  {
    Survey s{6, 6, 500000.0, 4000000.0, 0.0};
    SgyIndex idx = makeIndex(s);
    idx.coordinateSamples[7].x += 500.0; // 一道坐标严重错位
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(idx, 25.0);
    QVERIFY(!mapper.valid());
    QVERIFY(QString::fromStdString(mapper.fit().rejectionReason).contains("residual too large"));
  }
};

QTEST_GUILESS_MAIN(TestSgyCoordMapper)
#include "tst_sgycoordmapper.moc"
