// 层：测试壳
#include <QtTest>

#include "../src/algorithms/velocitymodel.h"

#include <QJsonDocument>
#include <QtMath>
#include <cstring>
#include <cmath>

// goal/time-depth-velocity 轮2：速度模型核数值验收（Oracle 第 2 条）。
//   · 恒速 / 两层层间平均 / V0-k 闭合式均有独立于实现的解析/手算期望值；
//   · 井锚点零误差（层间平均=插值语义，位级精确命中存档深度）；
//   · V0-k 锚点残差由 fitRmsMs 如实报告（回归模型口径，不冒充精确）；
//   · 序列化幂等、栅格转换幂等。
using namespace paleo::velmodel;

namespace {

VelocityWellControl well(const QString &id, double x, double y,
                         std::initializer_list<std::pair<double, double>> twtDepth)
{
  VelocityWellControl c;
  c.wellId = id;
  c.x = x; c.y = y;
  for (const auto &td : twtDepth)
    c.knots.append(VelocityKnot{td.first, td.second, QString()});
  return c;
}

// 独立数值积分（Simpson）：TWT(z) = 2000·∫₀^z dz'/(V0+k·z')——不用被测闭合式。
double twtByQuadrature(double v0, double k, double z, int steps)
{
  const double h = z / steps;
  double s = 1.0 / v0; // f(0)
  for (int i = 1; i < steps; ++i)
  {
    const double zi = i * h;
    s += (i % 2 == 1 ? 4.0 : 2.0) / (v0 + k * zi);
  }
  s += 1.0 / (v0 + k * z);
  return 2000.0 * s * h / 3.0;
}

} // namespace

class TestVelocityModel : public QObject
{
  Q_OBJECT

private slots:
  void constantVelocityAnalytic();
  void twoLayerIntervalHandValues();
  void intervalNoExtrapolation();
  void anchorsExactAtWells();
  void v0kClosedFormMatchesQuadrature();
  void v0kFitRecoversSyntheticTruth();
  void v0kFitRmsReportsAnchorResidual();
  void serializationRoundtrip();
  void fitRejectsBadControls();
  void gridConversionConstantVelocity();
  void gridConversionTwoWellsSpatialBlend();
  void gridConversionIdempotentAndCounts();
};

// 用例1：恒速 2500 m/s（k=0 极限 + 层间平均同值）——z = v·twt/2000 解析。
void TestVelocityModel::constantVelocityAnalytic()
{
  const auto c = well(QStringLiteral("W1"), 0.0, 0.0, {{1000.0, 1250.0}, {2000.0, 2500.0}});
  const VelocityModel interval = VelocityModel::fit({c}, ModelType::IntervalAverage);
  const VelocityModel v0k = VelocityModel::fit({c}, ModelType::V0kLinear);
  QVERIFY(interval.isValid());
  QVERIFY(v0k.isValid());
  QCOMPARE(v0k.wells().first().v0, 2500.0);
  QCOMPARE(v0k.wells().first().k, 0.0);

  QCOMPARE(interval.depthForTwt(300.0, 400.0, 1000.0), 1250.0); // 单井 → 空间查询=井答案
  QCOMPARE(v0k.depthForTwt(300.0, 400.0, 1600.0), 2000.0);      // 2500·1600/2000
  QCOMPARE(interval.velocityAt(0.0, 0.0, 1200.0), 2500.0);
  QCOMPARE(v0k.velocityAt(0.0, 0.0, 1200.0), 2500.0);
  QCOMPARE(interval.averageVelocityAt(0.0, 0.0, 2000.0), 2500.0);
  QCOMPARE(v0k.averageVelocityAt(0.0, 0.0, 2000.0), 2500.0);
  QVERIFY(qIsNaN(interval.averageVelocityAt(0.0, 0.0, 0.0))); // twt≤0 无定义
}

// 用例2：两层（Vint 2250 / 2750）手算——z(1000ms)=1062.5m、z(2000ms)=2312.5m、
// Vavg(2000)=2312.5·2/2000=2312.5… 见逐行注释。
void TestVelocityModel::twoLayerIntervalHandValues()
{
  const auto c = well(QStringLiteral("W1"), 100.0, 200.0,
                      {{500.0, 500.0}, {1500.0, 1625.0}, {2500.0, 3000.0}});
  const VelocityModel m = VelocityModel::fit({c}, ModelType::IntervalAverage);
  QVERIFY(m.isValid());
  // 层1 Vint = 2000·(1625−500)/(1500−500) = 2250；层2 = 2000·(3000−1625)/(2500−1500) = 2750。
  QCOMPARE(m.velocityAt(100.0, 200.0, 600.0), 2250.0);
  QCOMPARE(m.velocityAt(100.0, 200.0, 1600.0), 2750.0);
  // z(1000) = 500 + (1000−500)·2250/2000 = 1062.5；z(2000) = 1625 + 500·2750/2000 = 2312.5。
  QCOMPARE(m.depthForTwt(100.0, 200.0, 1000.0), 1062.5);
  QCOMPARE(m.depthForTwt(100.0, 200.0, 2000.0), 2312.5);
  // Vavg(2000) = 2·2312.5/2000 = 2312.5；Vavg(1000) = 2·1062.5/1000 = 2125。
  QCOMPARE(m.averageVelocityAt(100.0, 200.0, 2000.0), 2312.5);
  QCOMPARE(m.averageVelocityAt(100.0, 200.0, 1000.0), 2125.0);
  // 反向分段互逆：T(1062.5) = 1000、T(2312.5) = 2000。
  QCOMPARE(m.wellTwtForDepth(m.wells().first(), 1062.5), 1000.0);
  QCOMPARE(m.wellTwtForDepth(m.wells().first(), 2312.5), 2000.0);
}

// 用例3：插值语义不外推（样点范围外 NaN）；V0-k 回归模型语义允许外推。
void TestVelocityModel::intervalNoExtrapolation()
{
  const auto c = well(QStringLiteral("W1"), 0.0, 0.0, {{500.0, 500.0}, {1500.0, 1750.0}});
  const VelocityModel interval = VelocityModel::fit({c}, ModelType::IntervalAverage);
  QVERIFY(qIsNaN(interval.depthForTwt(0.0, 0.0, 499.0)));
  QVERIFY(qIsNaN(interval.depthForTwt(0.0, 0.0, 1501.0)));
  QVERIFY(qIsNaN(interval.depthForTwt(0.0, 0.0, -10.0)));

  const VelocityModel v0k = VelocityModel::fit({c}, ModelType::V0kLinear);
  QVERIFY(std::isfinite(v0k.depthForTwt(0.0, 0.0, 2500.0))); // 闭合式仍可算
}

// Oracle：井锚点处零误差——层间平均模型在井位查分层 TWT 位级命中存档深度。
void TestVelocityModel::anchorsExactAtWells()
{
  QVector<VelocityWellControl> cs;
  cs << well(QStringLiteral("A1"), 5288.67, 8219.94,
             {{900.0, 950.0}, {1500.0, 1700.0}, {2100.0, 2600.0}, {2600.0, 3400.0}});
  cs << well(QStringLiteral("B2"), 12000.5, 3000.25,
             {{800.0, 850.0}, {1400.0, 1520.0}, {2050.0, 2450.0}});
  cs << well(QStringLiteral("C3"), 300.0, 9000.0,
             {{1000.0, 1100.0}, {1700.0, 2000.0}});
  const VelocityModel m = VelocityModel::fit(cs, ModelType::IntervalAverage);
  QVERIFY(m.isValid());
  QCOMPARE(m.wells().size(), 3);
  for (const auto &w : m.wells())
    for (const auto &kn : w.knots)
      QCOMPARE(m.depthForTwt(w.x, w.y, kn.twtMs), kn.depthM); // 位级相等，零误差

  // 井位/近井连续性冒烟：偏离 1m 后仍有有限答案。
  QVERIFY(std::isfinite(m.velocityAt(5288.67, 8219.94, 1800.0)));
  QVERIFY(std::isfinite(m.velocityAt(5289.67, 8219.94, 1800.0)));
}

// 用例4：V0-k 闭合式 vs 独立 Simpson 积分（两条相互独立的数值路径）。
void TestVelocityModel::v0kClosedFormMatchesQuadrature()
{
  const auto c = well(QStringLiteral("W1"), 0.0, 0.0,
                      {{600.0, 610.0}, {1400.0, 1550.0}, {2400.0, 2900.0}});
  const VelocityModel m = VelocityModel::fit({c}, ModelType::V0kLinear);
  const VelocityModel::Well &w = m.wells().first();
  QVERIFY(w.k > 0.0); // 层速度随深度增 → k>0
  for (const double twt : {600.0, 1200.0, 1800.0, 2400.0})
  {
    const double z = m.wellDepthForTwt(w, twt);
    const double expectedTwt = twtByQuadrature(w.v0, w.k, z, 200000);
    QVERIFY2(qAbs(twt - expectedTwt) < 1e-6 * twt,
             qPrintable(QStringLiteral("twt=%1 quadrature=%2").arg(twt).arg(expectedTwt)));
  }
  // 反闭合式互逆：T(z(t)) = t（1e-9 相对）。
  const double t = 1800.0, z = m.wellDepthForTwt(w, t);
  QVERIFY(qAbs(m.wellTwtForDepth(w, z) - t) < 1e-9 * t);
}

// 用例5：合成真值（v0=1800, k=0.6）→ 拟合参数恢复、残差≈0、中途点闭合式命中。
void TestVelocityModel::v0kFitRecoversSyntheticTruth()
{
  const double v0True = 1800.0, kTrue = 0.6;
  QVector<VelocityKnot> knots;
  for (const double twt : {600.0, 1200.0, 1800.0, 2400.0})
  {
    const double z = v0True / kTrue * std::expm1(kTrue * twt / 2000.0);
    knots.append(VelocityKnot{twt, z, QStringLiteral("L%1").arg(knots.size())});
  }
  VelocityWellControl c;
  c.wellId = QStringLiteral("SYN"); c.x = 0.0; c.y = 0.0; c.knots = knots;
  const VelocityModel m = VelocityModel::fit({c}, ModelType::V0kLinear);
  const VelocityModel::Well &w = m.wells().first();
  QVERIFY(qAbs(w.v0 - v0True) < 1e-6 * v0True);
  QVERIFY(qAbs(w.k - kTrue) < 1e-6 * kTrue);
  QVERIFY(w.fitRmsMs < 1e-6);
  // 中途非控制点：z(1500) 与真值闭合式一致。
  const double z1500 = m.wellDepthForTwt(w, 1500.0);
  const double zTrue = v0True / kTrue * std::expm1(kTrue * 1500.0 / 2000.0);
  QVERIFY(qAbs(z1500 - zTrue) < 1e-9 * zTrue);
}

// 用例6：带噪声控制点 → 拟合残差 > 0（V0-k 锚点不精确是回归模型本性，如实报告）。
void TestVelocityModel::v0kFitRmsReportsAnchorResidual()
{
  const auto c = well(QStringLiteral("W1"), 0.0, 0.0,
                      {{1000.0, 1180.0}, {2000.0, 2510.0}, {3000.0, 3940.0}});
  const VelocityModel m = VelocityModel::fit({c}, ModelType::V0kLinear);
  const VelocityModel::Well &w = m.wells().first();
  QVERIFY(w.fitRmsMs > 1.0);  // 故意错位 ~1% 深度 → 残差毫秒级可见
  QVERIFY(w.fitRmsMs < 100.0);
}

// Oracle：模型可存可取——JSON 往返文档级相等，查询位级一致。
void TestVelocityModel::serializationRoundtrip()
{
  QVector<VelocityWellControl> cs;
  cs << well(QStringLiteral("A1"), 5288.67, 8219.94, {{900.0, 950.0}, {2100.0, 2600.0}});
  cs << well(QStringLiteral("B2"), 12000.5, 3000.25, {{800.0, 850.0}, {2050.0, 2450.0}});
  const auto compact = [](const VelocityModel &m) {
    return QJsonDocument(m.toJson()).toJson(QJsonDocument::Compact);
  };
  for (const ModelType type : {ModelType::IntervalAverage, ModelType::V0kLinear})
  {
    const VelocityModel m = VelocityModel::fit(cs, type);
    QVERIFY(m.isValid());
    QString err;
    const VelocityModel back = VelocityModel::fromJson(m.toJson(), &err);
    QVERIFY2(back.isValid(), qPrintable(err));
    QCOMPARE(compact(back), compact(m)); // 文档幂等
    QCOMPARE(back.type(), m.type());
    // 重复转换幂等（Oracle 第 3 条）：同模型任意查询位级一致。
    for (const double twt : {850.0, 1500.0, 2000.0})
      QCOMPARE(back.depthForTwt(8000.0, 5000.0, twt), m.depthForTwt(8000.0, 5000.0, twt));
  }
  QString err;
  QVERIFY(!VelocityModel::fromJson(QJsonObject(), &err).isValid());
  QVERIFY(!err.isEmpty());
}

// 建模入口守卫：坏井跳过记 notes；全坏 → 无效 + error。
void TestVelocityModel::fitRejectsBadControls()
{
  QVector<VelocityWellControl> cs;
  cs << well(QStringLiteral("single"), 0.0, 0.0, {{100.0, 100.0}});                    // 点数不足
  cs << well(QStringLiteral("unsorted"), 0.0, 0.0, {{1000.0, 900.0}, {500.0, 400.0}}); // 非递增
  cs << well(QStringLiteral("noxy"), qQNaN(), 0.0, {{100.0, 100.0}, {200.0, 180.0}});  // 缺坐标
  QString err;
  const VelocityModel m = VelocityModel::fit(cs, ModelType::IntervalAverage, &err);
  QVERIFY(!m.isValid());
  QVERIFY(!err.isEmpty());
  QCOMPARE(m.notes().size(), 3);

  const auto good = well(QStringLiteral("ok"), 0.0, 0.0, {{500.0, 500.0}, {1500.0, 1750.0}});
  const VelocityModel mixed = VelocityModel::fit(cs << good, ModelType::IntervalAverage);
  QVERIFY(mixed.isValid());
  QCOMPARE(mixed.wells().size(), 1);
  QCOMPARE(mixed.notes().size(), 3); // 跳过原因留档（QC 透明，不静默）
}

// 用例7：恒速模型整幅转换——每格 z = v·t/2000 手算值；nodata 透传不臆造。
void TestVelocityModel::gridConversionConstantVelocity()
{
  const auto c = well(QStringLiteral("W1"), 50.0, 50.0, {{1000.0, 1250.0}, {2000.0, 2500.0}});
  const VelocityModel m = VelocityModel::fit({c}, ModelType::IntervalAverage);
  const int rows = 3, cols = 3;
  const double gt[6] = {0.0, 100.0, 0.0, 300.0, 0.0, -100.0};
  QVector<float> t(rows * cols, 1600.0f);
  t[4] = -9999.0f; // 中心 nodata
  const DepthGridResult r = convertTimeGridToDepth(m, t, rows, cols, gt, -9999.0);
  QCOMPARE(r.convertedCells, 8);
  QCOMPARE(r.nodataCells, 1);
  QCOMPARE(r.outsideModelCells, 0);
  for (int i = 0; i < rows * cols; ++i)
  {
    if (i == 4)
      QVERIFY(qIsNaN(r.depthM[i]));
    else
      QCOMPARE(r.depthM[i], 2000.0f); // 2500·1600/2000
  }
}

// 用例8：双井空间加权——井位命中各自常速答案（位级）；等距中点 = 均值；
// 偏向井的格 = 权重比 9:1 的混合（w=1/d²，d=250/750 → 手算 1100/1900）。
void TestVelocityModel::gridConversionTwoWellsSpatialBlend()
{
  const auto a = well(QStringLiteral("A"), 0.0, 0.0, {{1000.0, 1000.0}, {2000.0, 2000.0}});   // 2000 m/s
  const auto b = well(QStringLiteral("B"), 1000.0, 0.0, {{1000.0, 2000.0}, {2000.0, 4000.0}}); // 4000 m/s
  const VelocityModel m = VelocityModel::fit({a, b}, ModelType::IntervalAverage);
  // 井位命中：IDW 距离 0 → 直接返回该井答案（锚点语义）。
  QCOMPARE(m.depthForTwt(0.0, 0.0, 1000.0), 1000.0);
  QCOMPARE(m.depthForTwt(1000.0, 0.0, 1000.0), 2000.0);

  const int rows = 1, cols = 3;
  const double gt[6] = {0.0, 500.0, 0.0, -50.0, 0.0, 100.0}; // 行格心 y=0（对齐井线）
  const QVector<float> t(rows * cols, 1000.0f);
  const DepthGridResult r = convertTimeGridToDepth(m, t, rows, cols, gt, -9999.0);
  QCOMPARE(r.convertedCells, 3);
  // x=250：dA=250, dB=750 → w 比 9:1 → (9·1000+1·2000)/10 = 1100。
  QVERIFY(qAbs(r.depthM[0] - 1100.0f) < 1e-3f);
  // x=750：dA=750, dB=250 → w 比 1:9 → (1000+9·2000)/10 = 1900。
  QVERIFY(qAbs(r.depthM[1] - 1900.0f) < 1e-3f);
  // x=1250：dA=1250, dB=250 → w 比 1:25 → (1000+25·2000)/26 ≈ 1961.54。
  QVERIFY(qAbs(r.depthM[2] - (1000.0 + 25.0 * 2000.0) / 26.0) < 1e-3);
}

// Oracle：重复转换幂等（位级）+ nodata/范围外计数 + 分段手算值。
void TestVelocityModel::gridConversionIdempotentAndCounts()
{
  const auto a = well(QStringLiteral("A"), 0.0, 0.0, {{500.0, 600.0}, {1200.0, 1500.0}});
  const VelocityModel m = VelocityModel::fit({a}, ModelType::IntervalAverage);
  const int rows = 2, cols = 2;
  const double gt[6] = {0.0, 100.0, 0.0, 200.0, 0.0, -100.0};
  QVector<float> t{400.0f, 800.0f, 1300.0f, qQNaN()};
  const DepthGridResult r1 = convertTimeGridToDepth(m, t, rows, cols, gt, -9999.0);
  const DepthGridResult r2 = convertTimeGridToDepth(m, t, rows, cols, gt, -9999.0);
  QCOMPARE(r1.nodataCells, 1);       // NaN 格
  QCOMPARE(r1.outsideModelCells, 2); // 400 < 500 与 1300 > 1200：样点范围外
  QCOMPARE(r1.convertedCells, 1);
  // 幂等：位级相同（NaN != NaN，须按字节比较）。
  QCOMPARE(std::memcmp(r1.depthM.constData(), r2.depthM.constData(),
                       r1.depthM.size() * sizeof(float)), 0);
  // z(800) = 600 + (800−500)·(1500−600)/(1200−500) = 600 + 270000/700 ≈ 985.714。
  QVERIFY(qAbs(r1.depthM[1] - (600.0 + 300.0 * 900.0 / 700.0)) < 1e-3);
}

QTEST_MAIN(TestVelocityModel)
#include "tst_velocitymodel.moc"
