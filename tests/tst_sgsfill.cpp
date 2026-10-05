// 层：数据（测试壳位于 tests/，被测对象为 IJK 格架上的序贯高斯充填）
#include <QtTest/QtTest>

#include "algorithms/geostat/variogram.h"
#include "algorithms/stratgrid/propfill.h"
#include "algorithms/stratgrid/sgsfill.h"
#include "algorithms/stratgrid/stratgrid.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using namespace paleo::stratgrid;
using paleo::geostat::Sgs3Params;
using paleo::geostat::VariogramModel;
using paleo::geostat::VariogramModelType;

namespace
{

ZoneGrid flatGrid(int ni, int nj, int nk)
{
  SurfaceGrid top;
  top.cols = ni;
  top.rows = nj;
  top.originX = 0;
  top.originY = 0;
  top.dx = 50;
  top.dy = 50;
  top.z.assign(static_cast<std::size_t>(ni) * nj, 1000.f);
  SurfaceGrid bot = top;
  for (float &v : bot.z)
    v = 1100.f;
  ZoneGrid grid;
  QString err;
  if (!buildZoneGrid(top, bot, nk, &grid, &err))
    qFatal("buildZoneGrid failed: %s", qPrintable(err));
  return grid;
}

VariogramModel testModel(double sill)
{
  VariogramModel model;
  model.type = VariogramModelType::Gaussian;
  model.nugget = 0.1 * sill;
  model.sill = sill;
  model.range = 250;    // 5 个柱距
  model.verticalRangeRatio = 4; // 层厚 10 → 垂向快速去相关
  return model;
}

// 高斯噪声样本（Box-Muller，mt19937_64 可复现）。
std::vector<double> gaussianValues(int count, double mean, double sigma, std::uint64_t seed)
{
  std::mt19937_64 rng(seed);
  auto unit = [&rng] {
    return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
  };
  std::vector<double> values;
  values.reserve(static_cast<std::size_t>(count));
  while (static_cast<int>(values.size()) < count)
  {
    const double r = std::sqrt(-2.0 * std::log(unit()));
    const double angle = 2.0 * std::numbers::pi * unit();
    values.push_back(mean + sigma * r * std::cos(angle));
  }
  return values;
}

// 阶梯布井：全柱 nk 层每层一井柱（i,j 沿网格均匀散开）。
std::vector<Seed> spreadSeeds(const ZoneGrid &grid, int wells, std::uint64_t seed, double mean,
                              double sigma)
{
  const std::vector<double> values = gaussianValues(wells * grid.nk, mean, sigma, seed);
  std::vector<Seed> seeds;
  int n = 0;
  for (int w = 0; w < wells; ++w)
  {
    const int i = static_cast<int>(std::lround(( 0.15 + 0.7 * w / std::max(1, wells - 1) ) *
                                               ( grid.ni - 1 )));
    const int j = ( w % 3 ) * ( grid.nj / 3 );
    for (int k = 0; k < grid.nk; ++k)
      seeds.push_back(Seed{i, j, k, values[static_cast<std::size_t>(n++)]});
  }
  return seeds;
}

} // namespace

class SgsFillTests : public QObject
{
  Q_OBJECT
private slots:
  void hardDataExactEveryRealization();
  void reproducibleWithSameSeed();
  void statisticsConvergeAcrossRealizations();
  void faciesZonesSeparateAndDoNotLeak();
  void faultCurtainBlocksConditioning();
  void insufficientZoneStaysUnfilledHonestly();
  void invalidInputsAndCancel();
};

void SgsFillTests::hardDataExactEveryRealization()
{
  // Oracle 2：井点处所有 realization 精确复现输入值（float 存储精度）。
  const ZoneGrid grid = flatGrid(14, 12, 5);
  std::vector<Seed> seeds;
  seeds.push_back(Seed{3, 3, 1, 12.5});
  seeds.push_back(Seed{7, 5, 2, 18.25});
  seeds.push_back(Seed{11, 9, 0, 9.75});
  Sgs3Params params;
  params.nRealizations = 5;
  params.seed = 99;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(1.0), params, &out, &meta, {}, &err),
           qPrintable(err));
  QCOMPARE(static_cast<int>(out.size()), 5);
  QCOMPARE(meta.snappedSeedCells, 3);
  for (const PropertyVolume &volume : out)
    for (const Seed &seed : seeds)
    {
      const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(seed.i, seed.j, seed.k))];
      QCOMPARE(v, static_cast<float>(seed.value));
    }
}

void SgsFillTests::reproducibleWithSameSeed()
{
  const ZoneGrid grid = flatGrid(12, 10, 4);
  const std::vector<Seed> seeds = spreadSeeds(grid, 6, 7, 10.0, 2.0);
  Sgs3Params params;
  params.nRealizations = 3;
  params.seed = 20261005;
  std::vector<PropertyVolume> a;
  std::vector<PropertyVolume> b;
  SgsFillMeta metaA;
  SgsFillMeta metaB;
  QString err;
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(1.0), params, &a, &metaA, {}, &err),
           qPrintable(err));
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(1.0), params, &b, &metaB, {}, &err),
           qPrintable(err));
  QCOMPARE(a.size(), b.size());
  for (std::size_t r = 0; r < a.size(); ++r)
    for (std::size_t cell = 0; cell < a[r].values.size(); ++cell)
      QVERIFY2(a[r].values[cell] == b[r].values[cell], "same seed must be bitwise equal");

  params.seed = 4;
  std::vector<PropertyVolume> c;
  SgsFillMeta metaC;
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(1.0), params, &c, &metaC, {}, &err),
           qPrintable(err));
  int differing = 0;
  for (std::size_t cell = 0; cell < c[0].values.size(); ++cell)
    if (c[0].values[cell] != a[0].values[cell])
      ++differing;
  QVERIFY2(differing > 100, "different seeds must produce different fields");
}

void SgsFillTests::statisticsConvergeAcrossRealizations()
{
  // Oracle 3：N 实现的均值/离散度收敛到合成模型参数（容差断言，种子可复现）。
  const ZoneGrid grid = flatGrid(20, 16, 4);
  const double mean = 10.0;
  const double sigma = 2.0;
  const std::vector<Seed> seeds = spreadSeeds(grid, 10, 11, mean, sigma);
  Sgs3Params params;
  params.nRealizations = 24;
  params.seed = 5150;
  params.maxPoints = 16;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(sigma * sigma), params, &out, &meta, {},
                   &err),
           qPrintable(err));

  // 远种子 cell（距任一种子柱 ≥ 2 柱距）：跨实现池化均值收敛到总体均值，
  // 逐 cell 偏差有界（实现间相关，宽限），跨实现离散度与样本方差同量级
  //（SGS 边缘方差 = 条件前的全局方差）。
  auto seedColumns = [&seeds](int i, int j) {
    for (const Seed &seed : seeds)
      if (std::abs(seed.i - i) + std::abs(seed.j - j) <= 1)
        return true;
    return false;
  };
  double pooledSum = 0;
  double pooledSqSum = 0;
  int pooledCount = 0;
  double meanBiasMax = 0;
  double varRatioMin = 1e9;
  double varRatioMax = 0;
  int checked = 0;
  for (int j = 0; j < grid.nj; j += 2)
    for (int i = 0; i < grid.ni; i += 2)
    {
      if (seedColumns(i, j))
        continue;
      const std::size_t cell = static_cast<std::size_t>(grid.cellIndex(i, j, grid.nk / 2));
      double sum = 0;
      for (const PropertyVolume &volume : out)
        sum += volume.values[cell];
      const double cellMean = sum / out.size();
      double sq = 0;
      for (const PropertyVolume &volume : out)
        sq += ( volume.values[cell] - cellMean ) * ( volume.values[cell] - cellMean );
      const double cellVar = sq / ( out.size() - 1 );
      meanBiasMax = std::max(meanBiasMax, std::fabs(cellMean - mean) / sigma);
      const double ratio = cellVar / ( sigma * sigma );
      varRatioMin = std::min(varRatioMin, ratio);
      varRatioMax = std::max(varRatioMax, ratio);
      pooledSqSum += cellVar;
      pooledSum += sum;
      pooledCount += static_cast<int>(out.size());
      ++checked;
    }
  QVERIFY2(checked > 40, "need enough far cells for the statistics assertion");
  const double pooledMean = pooledSum / pooledCount;
  const double pooledBias = std::fabs(pooledMean - mean) / sigma;
  const double pooledVarRatio = ( pooledSqSum / checked ) / ( sigma * sigma );
  QVERIFY2(pooledBias <= 0.4,
           QStringLiteral("pooled mean bias %1 sigma").arg(pooledBias).toUtf8().constData());
  QVERIFY2(pooledVarRatio >= 0.4 && pooledVarRatio <= 2.2,
           QStringLiteral("pooled variance ratio %1").arg(pooledVarRatio).toUtf8().constData());
  QVERIFY2(meanBiasMax <= 1.5,
           QStringLiteral("max per-cell mean bias %1 sigma").arg(meanBiasMax).toUtf8().constData());
  QVERIFY2(varRatioMin >= 0.05 && varRatioMax <= 8.0,
           QStringLiteral("per-cell variance ratio [%1,%2] (sanity bounds)")
               .arg(varRatioMin)
               .arg(varRatioMax)
               .toUtf8()
               .constData());
}

void SgsFillTests::faciesZonesSeparateAndDoNotLeak()
{
  // Oracle 4：两相带各带独立统计；带界处不混参数（B 带样本剧变 → A 带逐位不变）。
  const ZoneGrid grid = flatGrid(20, 10, 3);
  std::vector<int> zones(static_cast<std::size_t>(grid.ni) * grid.nj, 1);
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 10; i < grid.ni; ++i)
      zones[static_cast<std::size_t>(grid.columnIndex(i, j))] = 2;

  std::vector<Seed> zoneA;
  std::vector<Seed> zoneB;
  {
    const std::vector<double> va = gaussianValues(6 * grid.nk, 10.0, 0.4, 21);
    const std::vector<double> vb = gaussianValues(6 * grid.nk, 20.0, 0.4, 22);
    int n = 0;
    for (int k = 0; k < grid.nk; ++k)
    {
      zoneA.push_back(Seed{2, 2, k, va[static_cast<std::size_t>(n)]});
      zoneA.push_back(Seed{4, 7, k, va[static_cast<std::size_t>(n + 1)]});
      zoneA.push_back(Seed{7, 4, k, va[static_cast<std::size_t>(n + 2)]});
      zoneB.push_back(Seed{13, 3, k, vb[static_cast<std::size_t>(n)]});
      zoneB.push_back(Seed{16, 8, k, vb[static_cast<std::size_t>(n + 1)]});
      zoneB.push_back(Seed{18, 5, k, vb[static_cast<std::size_t>(n + 2)]});
      n += 3;
    }
  }
  std::vector<Seed> all = zoneA;
  all.insert(all.end(), zoneB.begin(), zoneB.end());

  Sgs3Params params;
  params.nRealizations = 8;
  params.seed = 77;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;
  QVERIFY2(fillSgs(grid, all, {}, &zones, testModel(1.0), params, &out, &meta, {}, &err),
           qPrintable(err));
  QCOMPARE(meta.zoneCount, 2);
  QCOMPARE(meta.insufficientZones, 0);

  auto zoneMean = [&](int zone, const PropertyVolume &volume) {
    double sum = 0;
    int n = 0;
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < grid.ni; ++i)
      {
        if (zones[static_cast<std::size_t>(grid.columnIndex(i, j))] != zone)
          continue;
        for (int k = 0; k < grid.nk; ++k)
        {
          const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))];
          if (std::isfinite(v))
          {
            sum += v;
            ++n;
          }
        }
      }
    return sum / n;
  };
  for (const PropertyVolume &volume : out)
  {
    const double meanA = zoneMean(1, volume);
    const double meanB = zoneMean(2, volume);
    QVERIFY2(meanA > 8.0 && meanA < 12.0,
             QStringLiteral("zone A mean %1 leaked from zone B").arg(meanA).toUtf8().constData());
    QVERIFY2(meanB > 18.0 && meanB < 22.0,
             QStringLiteral("zone B mean %1 leaked from zone A").arg(meanB).toUtf8().constData());
  }

  // 零参数泄漏：B 带种子值整体抬到 200±1，A 带 cell 逐位不变（分区独立调用）。
  std::vector<Seed> poisoned = zoneA;
  {
    const std::vector<double> vb = gaussianValues(6 * grid.nk, 200.0, 1.0, 22);
    int n = 0;
    for (int k = 0; k < grid.nk; ++k)
    {
      poisoned.push_back(Seed{13, 3, k, vb[static_cast<std::size_t>(n)]});
      poisoned.push_back(Seed{16, 8, k, vb[static_cast<std::size_t>(n + 1)]});
      poisoned.push_back(Seed{18, 5, k, vb[static_cast<std::size_t>(n + 2)]});
      n += 3;
    }
  }
  std::vector<PropertyVolume> poisonedOut;
  SgsFillMeta poisonedMeta;
  QVERIFY2(fillSgs(grid, poisoned, {}, &zones, testModel(1.0), params, &poisonedOut,
                   &poisonedMeta, {}, &err),
           qPrintable(err));
  QCOMPARE(poisonedOut.size(), out.size());
  for (std::size_t r = 0; r < out.size(); ++r)
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 0; i < 10; ++i)
        for (int k = 0; k < grid.nk; ++k)
        {
          const std::size_t cell = static_cast<std::size_t>(grid.cellIndex(i, j, k));
          QVERIFY2(out[r].values[cell] == poisonedOut[r].values[cell],
                   "zone A must be bit-identical when zone B data changes");
        }
}

void SgsFillTests::faultCurtainBlocksConditioning()
{
  const ZoneGrid grid = flatGrid(16, 8, 3);
  FaultSegment curtain;
  curtain.x0 = 400;
  curtain.y0 = -50;
  curtain.x1 = 400;
  curtain.y1 = 850;
  // 三口井全在西侧（i ≤ 7 → x ≤ 375 < 400），每层一柱
  std::vector<Seed> seeds;
  const std::vector<double> values = gaussianValues(9, 10.0, 1.0, 31);
  int n = 0;
  for (const std::pair<int, int> well : {std::pair<int, int>{2, 1}, {5, 6}, {7, 4}})
    for (int k = 0; k < grid.nk; ++k)
      seeds.push_back(Seed{well.first, well.second, k, values[static_cast<std::size_t>(n++)]});
  QVERIFY(seeds.size() == 9);

  Sgs3Params params;
  params.nRealizations = 2;
  params.seed = 5;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;
  QVERIFY2(fillSgs(grid, seeds, {curtain}, nullptr, testModel(1.0), params, &out, &meta, {},
                   &err),
           qPrintable(err));
  for (const PropertyVolume &volume : out)
  {
    QCOMPARE(volume.blockCount, 2);
    for (int j = 0; j < grid.nj; ++j)
      for (int i = 8; i < grid.ni; ++i)
        for (int k = 0; k < grid.nk; ++k)
        {
          const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))];
          QVERIFY2(!std::isfinite(v), "east block must stay NaN (no conditioning across curtain)");
        }
  }
}

void SgsFillTests::insufficientZoneStaysUnfilledHonestly()
{
  const ZoneGrid grid = flatGrid(12, 6, 2);
  std::vector<int> zones(static_cast<std::size_t>(grid.ni) * grid.nj, 1);
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 6; i < grid.ni; ++i)
      zones[static_cast<std::size_t>(grid.columnIndex(i, j))] = 2;

  std::vector<Seed> seeds; // 带 2 只有 1 个种子（< 2）→ 未充填
  seeds.push_back(Seed{2, 2, 0, 10.0});
  seeds.push_back(Seed{2, 4, 1, 12.0});
  seeds.push_back(Seed{9, 3, 0, 20.0});

  Sgs3Params params;
  params.seed = 3;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;
  QVERIFY2(fillSgs(grid, seeds, {}, &zones, testModel(1.0), params, &out, &meta, {}, &err),
           qPrintable(err));
  QCOMPARE(meta.insufficientZones, 1);
  QCOMPARE(meta.insufficientZoneCodes, std::vector<int>{2});
  QVERIFY(meta.caliber.contains(QStringLiteral("样本不足")));
  const PropertyVolume &volume = out.front();
  for (int j = 0; j < grid.nj; ++j)
    for (int i = 6; i < grid.ni; ++i)
      for (int k = 0; k < grid.nk; ++k)
      {
        const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(i, j, k))];
        if (i == 9 && j == 3 && k == 0)
        {
          // 唯一井柱的种子 cell：实测值钉死（硬数据不属于「借全域参数」）
          QCOMPARE(v, 20.0f);
          continue;
        }
        QVERIFY2(!std::isfinite(v), "insufficient zone must stay unfilled, not borrow global stats");
      }
}

void SgsFillTests::invalidInputsAndCancel()
{
  const ZoneGrid grid = flatGrid(6, 6, 2);
  const std::vector<Seed> seeds = {Seed{2, 2, 1, 5.0}};
  Sgs3Params params;
  std::vector<PropertyVolume> out;
  SgsFillMeta meta;
  QString err;

  VariogramModel zeroSill = testModel(0.0);
  zeroSill.nugget = 0;
  QVERIFY(!fillSgs(grid, seeds, {}, nullptr, zeroSill, params, &out, &meta, {}, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(out.empty());

  std::vector<int> badZones(3, 0); // 尺寸不符
  err.clear();
  QVERIFY(!fillSgs(grid, seeds, {}, &badZones, testModel(1.0), params, &out, &meta, {}, &err));
  QVERIFY(!err.isEmpty());

  // 单种子（全域域）= 如实退化：正态得分表单组退化 + 样本不足口径——
  // 除钉死的种子 cell 外整域 NaN，meta/口径如实标注（不是全有值）。
  std::vector<PropertyVolume> degenerate;
  QVERIFY2(fillSgs(grid, seeds, {}, nullptr, testModel(1.0), params, &degenerate, &meta, {}, &err),
           qPrintable(err));
  QCOMPARE(static_cast<int>(degenerate.size()), 1);
  QCOMPARE(meta.snappedSeedCells, 1);
  QCOMPARE(meta.insufficientZones, 1);
  QVERIFY2(meta.caliber.contains(QStringLiteral("样本不足")),
           qPrintable(meta.caliber));
  int finiteCells = 0;
  for (float v : degenerate.front().values)
    if (std::isfinite(v))
      ++finiteCells;
  QCOMPARE(finiteCells, 1); // 只有种子 cell 钉死
  QCOMPARE(degenerate.front().values[static_cast<std::size_t>(grid.cellIndex(2, 2, 1))], 5.0f);

  // 取消：第一个进度点即取消
  std::vector<PropertyVolume> cancelled;
  int calls = 0;
  const bool ok = fillSgs(flatGrid(30, 30, 3), spreadSeeds(flatGrid(30, 30, 3), 4, 9, 10.0, 1.0),
                          {}, nullptr, testModel(1.0), params, &cancelled, &meta,
                          [&calls](double) {
                            ++calls;
                            return false;
                          },
                          &err);
  QVERIFY(!ok);
  QCOMPARE(err, QStringLiteral("已取消"));
  QVERIFY(cancelled.empty());
}

QTEST_MAIN( SgsFillTests )
#include "tst_sgsfill.moc"
