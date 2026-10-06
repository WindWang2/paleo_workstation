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
};

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

QTEST_GUILESS_MAIN(TimeDepthModelTests)
#include "tst_timedepthmodel.moc"
