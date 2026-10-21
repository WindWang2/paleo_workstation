#include <QtTest>

#include "domain/seismic/timedepthmodel.h"

#include <cmath>

using seismic::TdPoint;
using seismic::TimeDepthModel;

// 时深模型纯域测试（无 QGIS/UI 依赖）。
class TimeDepthModelTests : public QObject
{
  Q_OBJECT
  private slots:
    void lenientDropsTimeReversalPoints();
    void lenientMonotoneTableUnchanged();
    void velocityBoundsSymmetric();
    void strictRejectsNegativeFirstPoint(); // #476
    void singlePointZeroTimeDoesNotInventVelocity(); // L1.2
};

// #476：setCheckshots strict 路径此前不查首点负深度/负时间（setPoints 有
// >= 0 检查）——负值首点会带着「合法 strict」标记进入插值面。
void TimeDepthModelTests::strictRejectsNegativeFirstPoint()
{
  TimeDepthModel model;
  {
    std::vector<TdPoint> bad;
    bad.push_back(TdPoint{ -100.0, 0.0 });
    bad.push_back(TdPoint{ 1000.0, 800.0 });
    bad.push_back(TdPoint{ 2000.0, 1600.0 });
    QVERIFY(!model.setCheckshots(bad)); // 首点负深度必须拒
  }
  {
    std::vector<TdPoint> bad;
    bad.push_back(TdPoint{ 0.0, -50.0 });
    bad.push_back(TdPoint{ 1000.0, 800.0 });
    QVERIFY(!model.setCheckshots(bad)); // 首点负时间同样拒
  }
  {
    std::vector<TdPoint> ok;
    ok.push_back(TdPoint{ 0.0, 0.0 });
    ok.push_back(TdPoint{ 1000.0, 800.0 });
    ok.push_back(TdPoint{ 2000.0, 1600.0 });
    QVERIFY(model.setCheckshots(ok)); // 合法表照常（首点 0/0 允许）
  }
}

void TimeDepthModelTests::lenientDropsTimeReversalPoints()
{
  // #218：宽松口径收进的脏表——1500m 处时间回落到 700ms（< 1000m 的 800ms）。
  // 修复前按 depth 排序后 TwtMsToDepth 在未按 time 排序的数组上 lower_bound，
  // 返回静默错值；修复后回落点被剔除，两个方向一致且互逆。
  TimeDepthModel model;
  model.setPoints({{0.0, 0.0}, {1000.0, 800.0}, {1500.0, 700.0}, {2000.0, 1600.0}});
  QCOMPARE(model.points().size(), std::size_t{3});
  for (std::size_t i = 1; i < model.points().size(); ++i)
    QVERIFY(model.points()[i].timeMs > model.points()[i - 1].timeMs);

  // 1200ms 落在 (1000m,800ms)-(2000m,1600ms) 段中点 → 1500m。
  QVERIFY(std::abs(model.TwtMsToDepth(1200.0) - 1500.0) < 1e-6);
  QVERIFY(std::abs(model.DepthToTwtMs(1500.0) - 1200.0) < 1e-6);
  for (double t = 0.0; t <= 1600.0; t += 50.0)
    QVERIFY(std::abs(model.DepthToTwtMs(model.TwtMsToDepth(t)) - t) < 1e-6);

  // 时间持平点同样剔除（零斜率段会让反向换算失去唯一解）。
  TimeDepthModel flat;
  flat.setPoints({{0.0, 0.0}, {500.0, 400.0}, {600.0, 400.0}, {1000.0, 800.0}});
  QCOMPARE(flat.points().size(), std::size_t{3});
}

void TimeDepthModelTests::lenientMonotoneTableUnchanged()
{
  TimeDepthModel model;
  model.setPoints({{2000.0, 1500.0}, {0.0, 0.0}, {1000.0, 800.0}, {3000.0, 2100.0}});
  QCOMPARE(model.points().size(), std::size_t{4});
  QVERIFY(std::abs(model.DepthToTwtMs(1500.0) - 1150.0) < 1e-6);
  QVERIFY(std::abs(model.TwtMsToDepth(1150.0) - 1500.0) < 1e-6);
}

void TimeDepthModelTests::velocityBoundsSymmetric()
{
  // opt-02（W6）：构造器/isValid 与 setVelocity/setCalibration 同界
  // [100,20000)。界外速度（手改时深 JSON 等）按缺省 2500 回落——旧实现
  // 单参构造器只查下界，30000 m/s 静默生效（先红：2500m → 166.67ms）。
  const TimeDepthModel absurd(30000.0);
  QVERIFY(absurd.isValid());
  QCOMPARE(absurd.DepthToTwtMs(2500.0),
           TimeDepthModel().DepthToTwtMs(2500.0)); // 2000 ms（缺省口径）
  QVERIFY(TimeDepthModel(150.0).isValid());
  QCOMPARE(TimeDepthModel(0.0).DepthToTwtMs(2500.0),
           TimeDepthModel().DepthToTwtMs(2500.0)); // 下界回落同口径
  // setVelocity 拒收路径的行为基准（口径不变量）。
  TimeDepthModel m(2000.0);
  m.setVelocity(30000.0);
  QCOMPARE(m.DepthToTwtMs(1000.0), 1000.0); // 仍是 2000 m/s
}

// L1.2（#592）：宽松口径的单点表且该点 timeMs==0（缺测记 0）时，旧实现用
// std::max(1e-3, 0) 兜底推出约 2×10⁹ m/s 常速，DepthToTwtMs 返回微秒级伪值
// 而不是报错。现在推不出合法速度带内的常速 → 两个方向都 NaN。
void TimeDepthModelTests::singlePointZeroTimeDoesNotInventVelocity()
{
  TimeDepthModel zero;
  zero.setPoints({{1000.0, 0.0}});
  QCOMPARE(zero.points().size(), std::size_t{1});
  QVERIFY(std::isnan(zero.DepthToTwtMs(500.0)));
  QVERIFY(std::isnan(zero.TwtMsToDepth(0.0005)));

  // 单点但速度落在合法带外（时差过小）同样不编速度。
  TimeDepthModel absurd;
  absurd.setPoints({{1000.0, 0.5}}); // 4×10⁶ m/s
  QVERIFY(std::isnan(absurd.DepthToTwtMs(500.0)));
  QVERIFY(std::isnan(absurd.TwtMsToDepth(200.0)));

  // 合法单点表照常反推常速（1000 m / 400 ms → 5000 m/s，双向互逆）。
  TimeDepthModel ok;
  ok.setPoints({{1000.0, 400.0}});
  QVERIFY(std::abs(ok.DepthToTwtMs(500.0) - 200.0) < 1e-6);
  QVERIFY(std::abs(ok.TwtMsToDepth(200.0) - 500.0) < 1e-6);

  // 近零深度单点仍回落缺省速度（深度太浅无从反推，不是脏数据）。
  TimeDepthModel shallow;
  shallow.setPoints({{0.0005, 400.0}});
  QVERIFY(std::abs(shallow.DepthToTwtMs(2500.0) -
                   TimeDepthModel().DepthToTwtMs(2500.0)) < 1e-9);
}

QTEST_GUILESS_MAIN(TimeDepthModelTests)
#include "tst_timedepthmodel.moc"
