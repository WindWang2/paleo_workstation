// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/gridsolver.h"
#include "algorithms/rasteralgebra.h"
#include "algorithms/surfacevolumes.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace paleo::gridsolver;
using namespace paleo::rasteralgebra;
using namespace paleo::surfacevolumes;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

struct Grid
{
  GridGeometry geom;
  std::vector<float> z;
  // [0,W]×[0,H] 网格 @cell：值按像元中心算 f(centerX, centerY)。
  Grid(double w, double h, double cell, double (*f)(double, double))
  {
    QString err;
    geom = geometryForExtent(0, w, 0, h, cell, &err);
    Q_ASSERT(geom.isValid());
    z.resize(std::size_t(geom.rows) * geom.cols);
    for (int i = 0; i < geom.rows; ++i)
      for (int j = 0; j < geom.cols; ++j)
      {
        const double cx = geom.originX + (j + 0.5) * geom.dx;
        const double cy = geom.originY - (i + 0.5) * geom.dy;
        z[std::size_t(i) * geom.cols + j] = float(f(cx, cy));
      }
  }
};

double zeroF(double, double) { return 0.0; }
double wedgeF(double x, double) { return x; }
double prismTopF(double, double) { return 10.0; }
double prismBaseF(double, double) { return 2.0; }

// 锥面：z = H(1 − r/R)，R=40、H=30，圆心 (40,40)；圆外 NaN。
double coneF(double x, double y)
{
  constexpr double R = 40.0, H = 30.0;
  const double r = std::hypot(x - R, y - R);
  return r <= R ? H * (1.0 - r / R)
                : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

class TestSurfaceVolumes : public QObject
{
  Q_OBJECT

private slots:
  // Oracle 3：规则几何面解析断言（≥2 用例）。
  void prismExactVolume();     // 棱柱：精确
  void wedgeExactVolume();     // 楔形：线性面矩形法则精确
  void coneWithinTolerance();  // 锥形：曲边界 O(h) 容差
  void aboveDatumWedge();      // 基准面上方体积：夹断线性精确
  void nullCellsExcluded();
  void signedVolumeAndStats();
  void invalidInputs();

  // Oracle 2：等厚图端到端——散点网格化（两个解析层位）→ 栅格代数相减 →
  // 体积积分对解析值断言。
  void isopachEndToEnd();
};

void TestSurfaceVolumes::prismExactVolume()
{
  const Grid top(100, 50, 2.0, prismTopF);
  const Grid base(100, 50, 2.0, prismBaseF);
  VolumeReport r;
  QString err;
  QVERIFY(volumeBetween(top.z.data(), base.z.data(), top.geom.cols, top.geom.rows,
                        top.geom.dx, top.geom.dy, &r, &err));
  const double expect = 8.0 * 100.0 * 50.0; // (10−2)·面积
  QVERIFY2(std::fabs(r.volume - expect) < 1e-9 * expect,
           qPrintable(QStringLiteral("prism %1 vs %2").arg(r.volume).arg(expect)));
  QCOMPARE(r.cells, 50 * 25);
  QCOMPARE(r.nullCells, 0);
  QCOMPARE(r.volume, r.absVolume);
}

void TestSurfaceVolumes::wedgeExactVolume()
{
  const Grid top(100, 50, 2.0, wedgeF);
  const Grid base(100, 50, 2.0, zeroF);
  VolumeReport r;
  QString err;
  QVERIFY(volumeBetween(top.z.data(), base.z.data(), top.geom.cols, top.geom.rows,
                        top.geom.dx, top.geom.dy, &r, &err));
  // ∫₀¹⁰⁰ x dx · 50 = 5000·50 = 250000；像元中心矩形法则对线性面精确。
  const double expect = 250000.0;
  QVERIFY2(std::fabs(r.volume - expect) < 1e-9 * expect,
           qPrintable(QStringLiteral("wedge %1 vs %2").arg(r.volume).arg(expect)));
  QCOMPARE(r.positiveCells, 50 * 25);
}

void TestSurfaceVolumes::coneWithinTolerance()
{
  // 锥 z = H(1 − r/R)，定义域内切圆 R=40（圆外格置 NaN）；解析体积
  // πR²H/3。矩形法则在曲边界 O(h) 误差 → 3% 容差。
  const double R = 40.0, H = 30.0, cell = 2.0;
  const Grid g(80, 80, cell, coneF);
  const Grid zero(80, 80, cell, zeroF);
  // 底面也要同掩膜（圆外 NaN）——把 zero 栅格的圆外格置 NaN。
  std::vector<float> base = zero.z;
  for (int i = 0; i < g.geom.rows; ++i)
    for (int j = 0; j < g.geom.cols; ++j)
      if (std::isnan(g.z[std::size_t(i) * g.geom.cols + j]))
        base[std::size_t(i) * g.geom.cols + j] = kNan;
  VolumeReport r;
  QString err;
  QVERIFY(volumeBetween(g.z.data(), base.data(), g.geom.cols, g.geom.rows, g.geom.dx,
                        g.geom.dy, &r, &err));
  const double expect = std::numbers::pi * R * R * H / 3.0;
  const double rel = std::fabs(r.volume - expect) / expect;
  QVERIFY2(rel < 0.03,
           qPrintable(QStringLiteral("cone %1 vs %2 (rel %3)")
                          .arg(r.volume)
                          .arg(expect)
                          .arg(rel)));
  QVERIFY(r.nullCells > 0); // 圆外格确实被排除
  QCOMPARE(r.cells + r.nullCells, 40 * 40);
}

void TestSurfaceVolumes::aboveDatumWedge()
{
  const Grid w(100, 50, 2.0, wedgeF);
  VolumeReport r;
  QString err;
  QVERIFY(volumeAboveDatum(w.z.data(), w.geom.cols, w.geom.rows, w.geom.dx, w.geom.dy,
                           25.0, &r, &err));
  // 真积分 ∫₂₅¹⁰⁰ (x−25) dx · 50 = 140625。像元中心矩形法则的精确值可
  // 解析算出：夹断点 25 恰是像元中心（cell=2 → 中心 1,3,…,99），该格贡献
  // 0 而真值 0.5·dx·dy·rows = 25，其余线性段精确 → 140600。
  const double expectRaster = 140600.0;
  const double expectAnalytic = 140625.0;
  QVERIFY2(std::fabs(r.volume - expectRaster) < 1e-6,
           qPrintable(QStringLiteral("above datum %1 vs %2")
                          .arg(r.volume)
                          .arg(expectRaster)));
  // 离散误差相对真值 0.018%（半像元效应，O(h) 口径的实证）。
  QVERIFY2(std::fabs(r.volume - expectAnalytic) / expectAnalytic < 2e-4,
           qPrintable(QStringLiteral("above datum %1 vs analytic %2")
                          .arg(r.volume)
                          .arg(expectAnalytic)));
  QCOMPARE(r.negativeCells, 0); // 负侧不计
}

void TestSurfaceVolumes::nullCellsExcluded()
{
  auto top = std::vector<float>{10.0f, 10.0f, kNan, 10.0f};
  auto base = std::vector<float>{0.0f, kNan, 0.0f, 0.0f};
  VolumeReport r;
  QString err;
  QVERIFY(volumeBetween(top.data(), base.data(), 4, 1, 1.0, 1.0, &r, &err));
  QCOMPARE(r.cells, 2);
  QCOMPARE(r.nullCells, 2);
  QCOMPARE(r.volume, 20.0);
  QCOMPARE(r.area, 2.0);
}

void TestSurfaceVolumes::signedVolumeAndStats()
{
  // 顶/底互换的楔形 → 体积为负（如实带符号）。
  const Grid top(100, 50, 2.0, zeroF);
  const Grid base(100, 50, 2.0, wedgeF);
  VolumeReport r;
  QString err;
  QVERIFY(volumeBetween(top.z.data(), base.z.data(), top.geom.cols, top.geom.rows,
                        top.geom.dx, top.geom.dy, &r, &err));
  QVERIFY(r.volume < 0);
  QVERIFY(std::fabs(r.volume + 250000.0) < 1e-6);
  QCOMPARE(r.negativeCells, 50 * 25);
  QCOMPARE(r.absVolume, 250000.0);
  QCOMPARE(r.minThickness, -99.0);
  QCOMPARE(r.maxThickness, -1.0);
  QCOMPARE(r.meanThickness, -50.0);
}

void TestSurfaceVolumes::invalidInputs()
{
  VolumeReport r;
  QString err;
  QVERIFY(!volumeBetween(nullptr, nullptr, 4, 4, 1.0, 1.0, &r, &err));
  QVERIFY(!volumeBetween(nullptr, nullptr, 0, 4, 1.0, 1.0, &r, &err));
  QVERIFY(!volumeBetween(nullptr, nullptr, 4, 4, 0.0, 1.0, &r, &err));
  const std::vector<float> z(16, 1.0f);
  QVERIFY(!volumeAboveDatum(z.data(), 4, 4, 1.0, 1.0, std::nan(""), &r, &err));
  QVERIFY(!err.isEmpty());
}

void TestSurfaceVolumes::isopachEndToEnd()
{
  // 两个解析层位：top = 100 + x/10（平面），base = 50 + y/25（平面）。
  // 真厚度 = 50 + x/10 − y/25。解析体积 = ∫∫ (50 + x/10 − y/25) dA。
  const auto topF = [](double x, double) { return 100.0 + x / 10.0; };
  const auto baseF = [](double, double y) { return 50.0 + y / 25.0; };
  QString err;
  const GridGeometry geom = geometryForExtent(0, 1000, 0, 800, 20.0, &err);
  QVERIFY(geom.isValid());
  auto scatter = [&](double (*f)(double, double))
  {
    std::vector<ScatterPoint> pts;
    for (int i = 0; i < 20; ++i)
      for (int j = 0; j < 25; ++j)
      {
        ScatterPoint p;
        p.x = 25.0 + j * 40.0;
        p.y = 25.0 + i * 40.0;
        p.z = f(p.x, p.y);
        pts.push_back(p);
      }
    return pts;
  };
  GriddingParams params;
  params.maxSweeps = 3000;
  std::vector<float> zTop, zBase;
  QString solveErr;
  QVERIFY2(solveMinimumCurvature(scatter(topF), geom, params, nullptr, &zTop, nullptr,
                                 &solveErr),
           qPrintable(solveErr));
  QVERIFY2(solveMinimumCurvature(scatter(baseF), geom, params, nullptr, &zBase, nullptr,
                                 &solveErr),
           qPrintable(solveErr));

  // 等厚 = top − base（栅格代数 Subtract）。
  const std::size_t n = std::size_t(geom.rows) * geom.cols;
  std::vector<float> iso(n);
  applyBinary(zTop.data(), zBase.data(), n, BinaryOp::Subtract, iso.data());

  // 体积积分：解析 ∫₀¹⁰⁰⁰∫₀⁸⁰⁰ (50 + x/10 − y/25) dy dx
  //   = 1000·[50y + ... ] 逐项：50·A + (1/10)∫x dx·800 − (1/25)∫y dy·1000
  //   = 50·800000 + (1/10)(500000·800) − (1/25)(320000·1000)
  //   = 40e6 + 40e6 − 12.8e6 = 67.2e6。
  VolumeReport r;
  QString volErr;
  const std::vector<float> zero(n, 0.0f);
  QVERIFY(volumeBetween(iso.data(), zero.data(), geom.cols, geom.rows, geom.dx, geom.dy,
                        &r, &volErr));
  const double expect = 67.2e6;
  const double rel = std::fabs(r.volume - expect) / expect;
  QVERIFY2(rel < 0.01,
           qPrintable(QStringLiteral("isopach e2e volume %1 vs %2 (rel %3)")
                          .arg(r.volume)
                          .arg(expect)
                          .arg(rel)));
  QCOMPARE(r.nullCells, 0);
  // 厚度极值：max = 50 + 1000/10 − 0/25 = 150（近边界），min ≈ 50 + 0 − 800/25
  // = 18。像元中心范围 [10,990]×[10,790] → [50+1−31.6, 50+99−0.4]。
  QVERIFY(r.minThickness > 19.0 && r.minThickness < 20.0);
  QVERIFY(r.maxThickness > 148.0 && r.maxThickness < 150.0);
}

QTEST_MAIN(TestSurfaceVolumes)
#include "tst_surfacevolumes.moc"
