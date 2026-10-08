// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/gridsolver.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace paleo::gridsolver;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// 确定性「散点」抖动（散点≠规则格点，网格化语义要求）——minstd 同族线性
// 同余，种子固定，跨平台可复现。
struct Jitter
{
  unsigned state = 20261002u;
  double next()
  {
    state = state * 48271u % 2147483647u;
    return static_cast<double>(state) / 2147483647.0;
  }
};

// 解析面定义域 [0,1000]²，网格 cell=25 → 40×40；散点 20×20 @50m 带抖动。
struct Fixture
{
  GridGeometry geom;
  std::vector<ScatterPoint> points;
  double zMin = 0, zMax = 0;

  explicit Fixture(double (*f)(double, double))
  {
    QString err;
    geom = geometryForExtent(0, 1000, 0, 1000, 25.0, &err);
    Jitter rng;
    zMin = std::numeric_limits<double>::infinity();
    zMax = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 20; ++i)
      for (int j = 0; j < 20; ++j)
      {
        ScatterPoint p;
        p.x = 25.0 + i * 50.0 + (rng.next() - 0.5) * 20.0;
        p.y = 25.0 + j * 50.0 + (rng.next() - 0.5) * 20.0;
        p.z = f(p.x, p.y);
        points.push_back(p);
        zMin = std::min(zMin, p.z);
        zMax = std::max(zMax, p.z);
      }
  }
  double range() const { return zMax - zMin; }
};

double planeF(double x, double y) { return 100.0 + 0.40 * x - 0.30 * y; }
double quadraticF(double x, double y)
{
  const double dx = x - 500.0, dy = y - 500.0;
  return 100.0 + dx * dx / 2000.0 + dy * dy / 3000.0;
}
double sinusoidF(double x, double y)
{
  return 100.0 + 50.0 * std::sin(2.0 * std::numbers::pi * x / 500.0) *
                     std::cos(2.0 * std::numbers::pi * y / 700.0);
}

// 网格真值与解的全格 RMS（z 单位）。真值按像元中心取。
double rmsVsTruth(const std::vector<float> &z, const GridGeometry &g,
                  double (*f)(double, double))
{
  double sumSq = 0;
  int m = 0;
  for (int i = 0; i < g.rows; ++i)
    for (int j = 0; j < g.cols; ++j)
    {
      const double v = z[std::size_t(i) * g.cols + j];
      if (std::isnan(v))
        continue;
      const double cx = g.originX + (j + 0.5) * g.dx;
      const double cy = g.originY - (i + 0.5) * g.dy;
      const double d = v - f(cx, cy);
      sumSq += d * d;
      ++m;
    }
  return std::sqrt(sumSq / m);
}

// issue #293 复现夹具：48×48 @25 m，一条过网格中心的 angle° 斜墙（半格采样
// 栅格化，与 mincurvature.cpp 的 break_line 掩码同口径——会产出 1 格宽、
// 对角相接的障碍链），两侧数据 0 | 100。
struct DiagonalWall
{
  GridGeometry g;
  std::vector<ScatterPoint> pts;
  std::vector<std::uint8_t> mask;
  double dirX = 1.0, dirY = 0.0; // 墙方向（单位向量）

  explicit DiagonalWall(double angleDeg)
  {
    QString err;
    g = geometryForExtent(0, 1200, 0, 1200, 25.0, &err);
    Q_ASSERT(g.isValid());
    const double th = angleDeg * std::numbers::pi / 180.0;
    dirX = std::cos(th);
    dirY = std::sin(th);
    const double cx = 600.0, cy = 600.0;
    mask.assign(std::size_t(g.rows) * g.cols, 0);
    // 半格采样栅格化（照 mincurvature.cpp break_line 口径）：斜线必产出
    // 对角相接的 1 格宽障碍链——旧核在此发散或返回 ±1e22 量级离谱值。
    const double half = 1300.0; // 半长，保证横贯整个网格
    const int steps = std::max(1, static_cast<int>(std::ceil(2.0 * half / (g.dx * 0.5))));
    for (int s = 0; s <= steps; ++s)
    {
      const double x = cx - dirX * half + 2.0 * dirX * half * s / steps;
      const double y = cy - dirY * half + 2.0 * dirY * half * s / steps;
      const int c = static_cast<int>(std::floor((x - g.originX) / g.dx));
      const int r = static_cast<int>(std::floor((g.originY - y) / g.dy));
      if (c >= 0 && c < g.cols && r >= 0 && r < g.rows)
        mask[std::size_t(r) * g.cols + c] = 1;
    }
    // 两侧散点：格中心距墙 >1.5 格才采（50 m 间隔），贴墙点留给掩码。
    for (int i = 0; i < g.rows; i += 2)
      for (int j = 0; j < g.cols; j += 2)
      {
        const double x = g.originX + (j + 0.5) * g.dx;
        const double y = g.originY - (i + 0.5) * g.dy;
        const double sd = sideOf(x, y);
        if (std::fabs(sd) < 1.5 * g.dx)
          continue;
        ScatterPoint p;
        p.x = x;
        p.y = y;
        p.z = sd < 0.0 ? 0.0 : 100.0;
        pts.push_back(p);
      }
  }

  // 有符号距离（dir 单位向量 → 即真实距离）；左盘负、右盘正。
  double sideOf(double x, double y) const
  {
    return dirX * (y - 600.0) - dirY * (x - 600.0);
  }
};

} // namespace

class TestGridSolver : public QObject
{
  Q_OBJECT

private slots:
  // 守卫：超规模请求拒绝（审计 #33 同口径——拒绝而非 OOM 分配）。
  void guardRejectsOversizeGrid();
  void guardRejectsDegenerateInput();
  void guardAcceptsBoundaryBudget(); // 恰好 1 亿像元不拒绝

  // Oracle 1：解析面 RMS 断言。
  void planeExactAtZeroTension();
  void quadraticExactAtZeroTension();
  void quadraticBiasedUnderTension();
  void sinusoidWithinTolerance();

  // 质量标记 + 语义。
  void statsAndConvergence();
  void rejectedPointsCounted();
  void farEdgePointsKeptOnExactMultipleExtent();
  void collisionsAveraged();
  void barrierBlocksInterpolation();
  void barrierOnlyAffectsMaskedCells();
  // issue #293：斜向单格宽屏障墙发散 / 离谱值回归 + 停滞判失败。
  void diagonalBarrierWallsConverge();
  void stalledIterationFails();

  // 质量面。
  void distanceToDataAnalytic();

  // Oracle 1：留一法 CV 数值断言。
  void crossValidationPlane();
  void crossValidationSinusoid();

  // Oracle 5：取消 + 进度单调。
  void cancelStopsSolve();
  void progressMonotonic();

  // 参数校验。
  void invalidParamsRejected();
  void sampleBilinearSemantics();
};

void TestGridSolver::guardRejectsOversizeGrid()
{
  QString err;
  // 100km × 1m 像元 → 1e5 × 1e5 = 1e10 格，远超 1 亿预算。
  const GridGeometry g = geometryForExtent(0, 100000, 0, 100000, 1.0, &err);
  QVERIFY(!g.isValid());
  QVERIFY(!err.isEmpty());
  QVERIFY(err.contains("budget"));
}

void TestGridSolver::guardRejectsDegenerateInput()
{
  QString err;
  QVERIFY(!geometryForExtent(0, 100, 100, 0, 10.0, &err).isValid()); // 空/倒置范围
  QVERIFY(!err.isEmpty());
  QVERIFY(!geometryForExtent(0, 100, 0, 100, 0.0, &err).isValid()); // 非法 cell
  QVERIFY(!geometryForExtent(0, 100, 0, 100, std::nan(""), &err).isValid());
}

void TestGridSolver::guardAcceptsBoundaryBudget()
{
  QString err;
  // 10km × 10km @ 1m = 10000×10000 = 恰好 1 亿 → 放行（上限是「不超」）。
  const GridGeometry g = geometryForExtent(0, 10000, 0, 10000, 1.0, &err);
  QVERIFY2(g.isValid(), qPrintable(err));
  QCOMPARE(g.cols, 10000);
  QCOMPARE(g.rows, 10000);
  QVERIFY(err.isEmpty());
}

void TestGridSolver::planeExactAtZeroTension()
{
  Fixture fx(planeF);
  GriddingParams params; // T=0 默认
  params.maxSweeps = 2000;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY2(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err),
           qPrintable(err));
  QVERIFY(stats.converged);
  // 最近节点赋值的半格量化（GMT 默认语义）：平滑面 RMS ~0.5% 极差级。
  const double rms = rmsVsTruth(z, fx.geom, planeF);
  QVERIFY2(rms < 0.01 * fx.range(),
           qPrintable(QStringLiteral("plane RMS %1 (range %2)").arg(rms).arg(fx.range())));
}

void TestGridSolver::quadraticExactAtZeroTension()
{
  Fixture fx(quadraticF);
  GriddingParams params;
  params.maxSweeps = 4000;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY2(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err),
           qPrintable(err));
  QVERIFY(stats.converged);
  const double rms = rmsVsTruth(z, fx.geom, quadraticF);
  QVERIFY2(rms < 0.015 * fx.range(),
           qPrintable(QStringLiteral("quadratic RMS %1 (range %2)").arg(rms).arg(fx.range())));
}

void TestGridSolver::quadraticBiasedUnderTension()
{
  Fixture fx(quadraticF);
  GriddingParams params;
  params.tension = 0.5;
  params.maxSweeps = 4000;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY2(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err),
           qPrintable(err));
  QVERIFY(stats.converged);
  // 张力把二次面（∇²≠0）往膜方程解拉——偏差存在但受控（<5% 极差）。
  const double rms = rmsVsTruth(z, fx.geom, quadraticF);
  QVERIFY2(rms < 0.05 * fx.range(),
           qPrintable(QStringLiteral("tensioned quadratic RMS %1 (range %2)")
                          .arg(rms)
                          .arg(fx.range())));
}

void TestGridSolver::sinusoidWithinTolerance()
{
  Fixture fx(sinusoidF);
  GriddingParams params;
  params.tension = 0.25;
  params.maxSweeps = 4000;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY2(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err),
           qPrintable(err));
  QVERIFY(stats.converged);
  const double rms = rmsVsTruth(z, fx.geom, sinusoidF);
  QVERIFY2(rms < 0.05 * fx.range(),
           qPrintable(QStringLiteral("sinusoid RMS %1 (range %2)").arg(rms).arg(fx.range())));
}

void TestGridSolver::statsAndConvergence()
{
  Fixture fx(planeF);
  GriddingParams params;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err));
  QVERIFY(stats.sweeps >= 1);
  QVERIFY(stats.converged);
  QVERIFY(stats.finalDelta >= 0.0);
  QCOMPARE(static_cast<int>(z.size()), fx.geom.rows * fx.geom.cols);
  // 400 散点、40×40 网格：几乎每格都有约束（40×40=1600 格，400 点），
  // 约束格 ≤ 散点数。
  QVERIFY(stats.constrainedNodes > 0);
  QVERIFY(stats.constrainedNodes <= 400);
}

void TestGridSolver::rejectedPointsCounted()
{
  Fixture fx(planeF);
  auto pts = fx.points;
  ScatterPoint outside;
  outside.x = -100;
  outside.y = 500;
  outside.z = 0;
  pts.push_back(outside);
  ScatterPoint nanPt;
  nanPt.x = 500;
  nanPt.y = 500;
  nanPt.z = std::numeric_limits<double>::quiet_NaN();
  pts.push_back(nanPt);
  GriddingParams params;
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY(solveMinimumCurvature(pts, fx.geom, params, nullptr, &z, &stats, &err));
  QCOMPARE(stats.rejected, 2);
}

// #162：11×11 规则点阵 @25 m、像元 25 m（跨度恰为整数倍）→ 东/南边界 21 点旧代码被丢。
void TestGridSolver::farEdgePointsKeptOnExactMultipleExtent()
{
  std::vector<ScatterPoint> pts;
  for (int i = 0; i <= 10; ++i)
    for (int j = 0; j <= 10; ++j)
    {
      ScatterPoint p;
      p.x = 1000.0 + 25.0 * i;
      p.y = 5000.0 + 25.0 * j;
      p.z = planeF(p.x, p.y);
      pts.push_back(p);
    }
  QString err;
  const GridGeometry g = geometryForExtent(1000.0, 1250.0, 5000.0, 5250.0, 25.0, &err);
  QVERIFY2(g.isValid(), qPrintable(err));
  GriddingParams params;
  std::vector<float> z;
  GriddingStats stats;
  QVERIFY2(solveMinimumCurvature(pts, g, params, nullptr, &z, &stats, &err), qPrintable(err));
  QCOMPARE(stats.rejected, 0);
  // 真正在网格外的点仍拒绝。
  ScatterPoint outside;
  outside.x = 1250.0 + 1.0;
  outside.y = 5100.0;
  outside.z = 0;
  pts.push_back(outside);
  QVERIFY2(solveMinimumCurvature(pts, g, params, nullptr, &z, &stats, &err), qPrintable(err));
  QCOMPARE(stats.rejected, 1);
}

void TestGridSolver::collisionsAveraged()
{
  QString err;
  GridGeometry g = geometryForExtent(0, 100, 0, 100, 10.0, &err);
  QVERIFY(g.isValid());
  std::vector<ScatterPoint> pts;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 8; ++j)
    {
      // 同一最近节点两个不同 z 的点：(5,5) 恰是节点中心，(5.1,5.2) 亦归属
      // 同一节点 → 约束竞争，约束步迭代消解（面分别过两个数据点）。
      ScatterPoint a, b;
      a.x = 5.0 + i * 10;
      a.y = 5.0 + j * 10;
      a.z = 10.0;
      b.x = a.x + 0.1;
      b.y = a.y + 0.1;
      b.z = 20.0;
      pts.push_back(a);
      pts.push_back(b);
    }
  GriddingParams params;
  params.maxSweeps = 2000;
  std::vector<float> z;
  GriddingStats stats;
  QVERIFY(solveMinimumCurvature(pts, g, params, nullptr, &z, &stats, &err));
  QCOMPARE(stats.collisions, 64);
  QCOMPARE(stats.constrainedNodes, 64);
  // 同一最近节点两点（0.12 格距、Δz=10）：节点值 = 均值 15（Dirichlet
  // 冻结语义；亚格分辨不硬过每一点）。(5,5) 落第 9 行（北向上 y 小行大）。
  QCOMPARE(z[9 * 10 + 0], 15.0f);
}

void TestGridSolver::barrierBlocksInterpolation()
{
  // 左半平面 z=10、右半平面 z=100 的阶跃面；x∈[475,525] 竖直屏障。
  // 有屏障：两盘各自保持本侧值（断层语义）；对照（无屏障）：最小曲率
  // 在断层处给出平滑过渡带。
  QString err;
  const GridGeometry g = geometryForExtent(0, 1000, 0, 400, 25.0, &err);
  QVERIFY(g.isValid());
  std::vector<ScatterPoint> pts;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 20; ++j)
    {
      ScatterPoint left, right;
      left.x = 25.0 + j * 50.0;
      left.y = 25.0 + i * 50.0;
      right = left;
      left.z = 10.0;
      right.x += 500.0;
      right.z = 100.0;
      if (left.x < 475.0)
        pts.push_back(left);
      if (right.x > 525.0)
        pts.push_back(right);
    }
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(g.rows) * g.cols, 0);
  // 屏障墙取 2 格宽（cols 20,21 = x∈[500,550)）：双调和 13 点模板跨距 ±2，
  // 1 格宽的墙会被 j±2 邻居跳过，2 格才能在模板意义上完全隔离两盘。
  const int wallCol = 500 / 25;
  for (int i = 0; i < g.rows; ++i)
    for (int c = wallCol; c <= wallCol + 1; ++c)
      mask[static_cast<std::size_t>(i) * g.cols + c] = 1;

  GriddingParams params;
  params.maxSweeps = 3000;
  std::vector<float> zb, zFree;
  GriddingStats stB, stF;
  QVERIFY(solveMinimumCurvature(pts, g, params, mask.data(), &zb, &stB, &err));
  QVERIFY(solveMinimumCurvature(pts, g, params, nullptr, &zFree, &stF, &err));

  // 屏障格自身 NaN。
  QVERIFY(std::isnan(zb[static_cast<std::size_t>(5) * g.cols + wallCol]));
  QVERIFY(std::isnan(zb[static_cast<std::size_t>(5) * g.cols + wallCol + 1]));
  // 断层两盘邻格：有屏障时贴墙格保持本侧值（±10% 带容差）。
  const auto at = [&zb, &g](int row, int col) { return zb[std::size_t(row) * g.cols + col]; };
  QVERIFY2(at(5, wallCol - 1) < 25.0,
           qPrintable(QStringLiteral("left flank %1").arg(at(5, wallCol - 1))));
  QVERIFY2(at(5, wallCol + 2) > 75.0,
           qPrintable(QStringLiteral("right flank %1").arg(at(5, wallCol + 2))));
  // 无屏障对照：同位置应出现过渡（贴墙格被对侧拉离本侧极值）。
  const auto atF = [&zFree, &g](int row, int col) { return zFree[std::size_t(row) * g.cols + col]; };
  QVERIFY(atF(5, wallCol - 1) > at(5, wallCol - 1));
  QVERIFY(atF(5, wallCol + 2) < at(5, wallCol + 2));
}

void TestGridSolver::barrierOnlyAffectsMaskedCells()
{
  Fixture fx(planeF);
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(fx.geom.rows) * fx.geom.cols, 0);
  GriddingParams params;
  params.maxSweeps = 2000;
  std::vector<float> zPlain, zBarriered;
  GriddingStats st1, st2;
  QString err;
  QVERIFY(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &zPlain, &st1, &err));
  QVERIFY(
      solveMinimumCurvature(fx.points, fx.geom, params, mask.data(), &zBarriered, &st2, &err));
  // 全零屏障 = 无屏障：结果逐位一致。
  QCOMPARE(zPlain, zBarriered);
}

void TestGridSolver::diagonalBarrierWallsConverge()
{
  // issue #293 回归：斜向单格宽屏障墙（对角相接障碍链）曾令松弛发散报
  // 「iteration diverged」，或在 ok=true 下发布 −4e22…1e37 的离谱值。
  // 验收口径：30°/45°（另加 issue 复现过的 15°/60°/75°）必须收敛，
  // 输出全部落在 [0,100] 容许 ±10 过冲，两盘各自保持本侧值。
  for (const double angle : {15.0, 30.0, 45.0, 60.0, 75.0})
  {
    DiagonalWall w(angle);
    GriddingParams params;
    params.tension = 0.25; // issue 复现参数（T=0.25、ω=1.5、500 遍 × 多级）
    std::vector<float> z;
    GriddingStats st;
    QString err;
    QVERIFY2(solveMinimumCurvature(w.pts, w.g, params, w.mask.data(), &z, &st, &err),
             qPrintable(QStringLiteral("angle %1: %2").arg(angle).arg(err)));
    QVERIFY2(st.converged,
             qPrintable(QStringLiteral("angle %1 not converged").arg(angle)));
    for (int i = 0; i < w.g.rows; ++i)
      for (int j = 0; j < w.g.cols; ++j)
      {
        const std::size_t idx = std::size_t(i) * w.g.cols + j;
        if (w.mask[idx])
        {
          QVERIFY2(std::isnan(z[idx]),
                   qPrintable(QStringLiteral("angle %1 barrier cell not NaN").arg(angle)));
          continue;
        }
        QVERIFY2(z[idx] >= -10.0 && z[idx] <= 110.0,
                 qPrintable(QStringLiteral("angle %1 cell (%2,%3) out of bounds: %4")
                                .arg(angle)
                                .arg(i)
                                .arg(j)
                                .arg(z[idx])));
        // 距墙 ≥3 格的格：左盘 ≤25、右盘 ≥75（两盘隔离）。
        const double x = w.g.originX + (j + 0.5) * w.g.dx;
        const double y = w.g.originY - (i + 0.5) * w.g.dy;
        const double sd = w.sideOf(x, y);
        if (sd <= -3.0 * w.g.dx)
          QVERIFY2(z[idx] <= 25.0,
                   qPrintable(QStringLiteral("angle %1 left cell (%2,%3) = %4")
                                  .arg(angle)
                                  .arg(i)
                                  .arg(j)
                                  .arg(z[idx])));
        else if (sd >= 3.0 * w.g.dx)
          QVERIFY2(z[idx] >= 75.0,
                   qPrintable(QStringLiteral("angle %1 right cell (%2,%3) = %4")
                                  .arg(angle)
                                  .arg(i)
                                  .arg(j)
                                  .arg(z[idx])));
      }
  }
}

void TestGridSolver::stalledIterationFails()
{
  // issue #293 验收：迭代停滞（末遍更新量仍 ≫ 数据极差）时如实判失败，
  // 而不是 ok=true 静默发布数值荒谬的栅格（旧行为：只有 PALEO_CONVERGED=0
  // 元数据能看出来）。maxSweeps=1 不可能收敛 → 必须 false + 报错。
  Fixture fx(sinusoidF);
  GriddingParams params;
  params.maxSweeps = 1;
  std::vector<float> z;
  GriddingStats st;
  QString err;
  QVERIFY(!solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &st, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(z.empty()); // 失败路径不发布结果
}

void TestGridSolver::distanceToDataAnalytic()
{
  QString err;
  const GridGeometry g = geometryForExtent(0, 200, 0, 200, 10.0, &err);
  QVERIFY(g.isValid());
  std::vector<ScatterPoint> pts;
  ScatterPoint p;
  p.x = 50.0;
  p.y = 50.0;
  p.z = 0.0;
  pts.push_back(p);
  const std::vector<float> d = distanceToData(pts, g, &err);
  QCOMPARE(static_cast<int>(d.size()), g.rows * g.cols);
  // 最近格（含散点）：距离 ≤ 半对角；最远角格：≈ 解析距离。
  double minD = std::numeric_limits<double>::infinity();
  for (float v : d)
    minD = std::min(minD, double(v));
  QVERIFY(minD <= 10.0 * std::sqrt(2.0) / 2.0 + 1e-6);
  // 右上角格中心到散点的解析距离（originY=200 → 行 0 中心 y=195）。
  const double cy = g.originY - 0.5 * g.dy;
  const double cx = g.originX + (g.cols - 0.5) * g.dx;
  const double want = std::hypot(cx - 50.0, cy - 50.0);
  const float corner = d[g.cols - 1]; // 行 0、最右列
  QVERIFY2(std::fabs(corner - want) < 1e-3,
           qPrintable(QStringLiteral("corner %1 vs %2").arg(corner).arg(want)));
  QVERIFY(distanceToData({}, g, &err).empty());
  QVERIFY(!err.isEmpty());
}

void TestGridSolver::crossValidationPlane()
{
  Fixture fx(planeF);
  GriddingParams params;
  params.maxSweeps = 1500;
  CrossValidationResult cv;
  QString err;
  QVERIFY2(
      crossValidateLeaveOneOut(fx.points, fx.geom, params, 24, true, &cv, &err),
      qPrintable(err));
  QCOMPARE(cv.folds, 24);
  QCOMPARE(static_cast<int>(cv.residuals.size()), 24);
  QCOMPARE(static_cast<int>(cv.heldOutIndex.size()), 24);
  // 平面 CV 残差 = 最近节点量化的半格效应（<1.5% 极差）。
  QVERIFY2(cv.rms < 0.015 * fx.range(),
           qPrintable(QStringLiteral("plane CV RMS %1 (range %2)").arg(cv.rms).arg(fx.range())));
  // 单折边界（maxPoints=1）不触发除零/未定义行为。
  CrossValidationResult one;
  QString oneErr;
  QVERIFY2(crossValidateLeaveOneOut(fx.points, fx.geom, params, 1, true, &one, &oneErr),
           qPrintable(oneErr));
  QCOMPARE(one.folds, 1);
  QVERIFY(std::isfinite(one.rms));
}

void TestGridSolver::crossValidationSinusoid()
{
  Fixture fx(sinusoidF);
  GriddingParams params;
  params.maxSweeps = 1500;
  CrossValidationResult cv;
  QString err;
  QVERIFY(crossValidateLeaveOneOut(fx.points, fx.geom, params, 16, true, &cv, &err));
  QCOMPARE(cv.folds, 16);
  // 正弦面留一重构有界误差（<25% 极差——过冲/欠采样的诚实上界）。
  QVERIFY2(cv.rms < 0.25 * fx.range(),
           qPrintable(QStringLiteral("sinusoid CV RMS %1 (range %2)").arg(cv.rms).arg(fx.range())));
  QVERIFY(cv.maxAbs >= cv.rms);
}

void TestGridSolver::cancelStopsSolve()
{
  Fixture fx(sinusoidF);
  GriddingParams params;
  params.maxSweeps = 100000; // 取消前跑不完
  int calls = 0;
  IterationControl control;
  control.cancelRequested = [&calls]
  {
    return ++calls > 3; // 第 4 次查询起取消
  };
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  const bool ok = solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats,
                                        &err, control);
  QVERIFY(!ok);
  QCOMPARE(err, QStringLiteral("canceled"));
  QVERIFY(z.empty());
}

void TestGridSolver::progressMonotonic()
{
  Fixture fx(sinusoidF);
  GriddingParams params;
  params.maxSweeps = 4000; // 收敛则提前停；未收敛也在有限时间内结束
  std::vector<int> sweeps;
  IterationControl control;
  control.onSweep = [&sweeps](int s, int maxS, double d)
  {
    sweeps.push_back(s);
    Q_UNUSED(maxS);
    Q_UNUSED(d);
  };
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  QVERIFY2(solveMinimumCurvature(fx.points, fx.geom, params, nullptr, &z, &stats, &err,
                                 control),
           qPrintable(err));
  QVERIFY(sweeps.size() >= 3);
  for (std::size_t i = 1; i < sweeps.size(); ++i)
    QVERIFY2(sweeps[i] > sweeps[i - 1],
             qPrintable(QStringLiteral("sweep %1 not > %2").arg(sweeps[i]).arg(sweeps[i - 1])));
}

void TestGridSolver::invalidParamsRejected()
{
  Fixture fx(planeF);
  std::vector<float> z;
  GriddingStats stats;
  QString err;
  GriddingParams bad;
  bad.tension = 1.5;
  QVERIFY(!solveMinimumCurvature(fx.points, fx.geom, bad, nullptr, &z, &stats, &err));
  QVERIFY(!err.isEmpty());
  bad.tension = 0.0;
  bad.relaxation = 2.5;
  QVERIFY(!solveMinimumCurvature(fx.points, fx.geom, bad, nullptr, &z, &stats, &err));
  bad.relaxation = 1.8;
  bad.maxSweeps = 0;
  QVERIFY(!solveMinimumCurvature(fx.points, fx.geom, bad, nullptr, &z, &stats, &err));
  // 过小网格 / 空散点。
  GridGeometry tiny = geometryForExtent(0, 60, 0, 60, 20.0, &err);
  QVERIFY(tiny.isValid());
  QVERIFY(!solveMinimumCurvature(fx.points, tiny, GriddingParams(), nullptr, &z, &stats, &err));
  QVERIFY(!solveMinimumCurvature({}, fx.geom, GriddingParams(), nullptr, &z, &stats, &err));
}

void TestGridSolver::sampleBilinearSemantics()
{
  QString err;
  const GridGeometry g = geometryForExtent(0, 100, 0, 100, 10.0, &err);
  QVERIFY(g.isValid());
  std::vector<float> z(static_cast<std::size_t>(g.rows) * g.cols);
  for (int i = 0; i < g.rows; ++i)
    for (int j = 0; j < g.cols; ++j)
      z[std::size_t(i) * g.cols + j] = float(i * 100 + j);
  // 平面 z = x + ...：格 (i,j) 值 = 100i + j，双线性在 (5,5) 中心处还原格值。
  const double cx = g.originX + 5.5 * g.dx;
  const double cy = g.originY - 5.5 * g.dy;
  QCOMPARE(sampleBilinear(z, g, cx, cy), float(5 * 100 + 5));
  // 格点间线性：两中心中点 = 均值。
  const double midX = g.originX + 6.0 * g.dx;
  QCOMPARE(sampleBilinear(z, g, midX, cy), float(5 * 100 + 5 + 0.5));
  // 越界 → NaN。
  QVERIFY(std::isnan(sampleBilinear(z, g, -1.0, 50.0)));
  QVERIFY(std::isnan(sampleBilinear(z, g, 150.0, 50.0)));
}

QTEST_MAIN(TestGridSolver)
#include "tst_gridsolver.moc"
