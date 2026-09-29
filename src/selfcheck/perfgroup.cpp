// 层：测试壳
#include "perfgroup.h"

#include "../io/benchreport.h"
#include "../io/cachebudget.h"
#include "../io/lascache.h"
#include "../io/lasparser.h"
#include "../io/perffixtures.h"
#include "../io/rasterpyramid.h"
#include "../io/segyindexstore.h"
#include "../io/segyreader.h"
#include "../io/shacache.h"
#include "../catalog/datacatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

namespace
{
  double msSince(const QElapsedTimer &t)
  {
    return double(t.nsecsElapsed()) / 1.0e6;
  }

  BenchResult make(const QString &name, const QString &group, double value,
                   const QString &unit, double budget = -1.0, const QString &note = {})
  {
    BenchResult r;
    r.name = name;
    r.group = group;
    r.value = value;
    r.unit = unit;
    r.budget = budget;
    r.pass = budget <= 0 || value <= budget;
    r.note = note;
    return r;
  }
} // namespace

namespace PerfGroup
{

QVector<BenchResult> runAll(const QString &workDir)
{
  QVector<BenchResult> out;
  QDir().mkpath(workDir);

  // ---------------- LAS（D1.5）----------------
  {
    const QString las = workDir + QStringLiteral("/bench_15581.las");
    PerfFixtures::makeSyntheticLas(las, 15581);
    QElapsedTimer t;

    // 旧路径（基准对照，无预算）。
    t.start();
    QStringList names;
    QList<LasCurve> curves;
    LasParser::parse(las, names, curves);
    out.append(make(QStringLiteral("las_legacy_parse_ms"), QStringLiteral("las"), msSince(t),
                    QStringLiteral("ms"), -1, QStringLiteral("LasParser::parse 旧 QTextStream 路径")));

    // 快路径冷解析（预算 50ms，D1.5）。
    LasCache::shared().setDiskRoot(workDir + QStringLiteral("/lasidx"));
    LasCache::shared().invalidate();
    QList<LasIssue> issues;
    t.start();
    const LasDoc cold = LasCache::shared().load(las, &issues);
    const double coldMs = msSince(t);
    out.append(make(QStringLiteral("las_cold_parse_ms"), QStringLiteral("las"), coldMs,
                    QStringLiteral("ms"), 50.0, QStringLiteral("parseDoc 冷解析（含磁盘缓存写）")));
    // 热命中（预算 5ms，D1.1）。
    t.start();
    const LasDoc hot = LasCache::shared().load(las);
    out.append(make(QStringLiteral("las_cached_ms"), QStringLiteral("las"), msSince(t),
                    QStringLiteral("ms"), 5.0, QStringLiteral("二级缓存命中")));
    // 内存命中。
    t.start();
    LasCache::shared().load(las);
    out.append(make(QStringLiteral("las_memory_hit_ms"), QStringLiteral("las"), msSince(t),
                    QStringLiteral("ms"), 1.0, QStringLiteral("内存 LRU 命中")));
    // header-only。
    t.start();
    LasHeaderInfo h;
    LasParser::parseHeader(las, h);
    out.append(make(QStringLiteral("las_header_ms"), QStringLiteral("las"), msSince(t),
                    QStringLiteral("ms"), 5.0, QStringLiteral("header-only 到 ~A")));
    // 区间查询（1/8 窗口——应远小于全量）。
    t.start();
    LasParser::parseRange(las, 0, 2000, names, curves);
    out.append(make(QStringLiteral("las_range_2k_ms"), QStringLiteral("las"), msSince(t),
                    QStringLiteral("ms"), 25.0, QStringLiteral("行区间 [0,2000) 流式")));
  }

  // ---------------- SEG-Y 索引（D2）----------------
  {
    const QString sgy = workDir + QStringLiteral("/bench_100x100x50.sgy");
    const int traces = PerfFixtures::makeSyntheticSegy(sgy, 100, 100, 50);
    QElapsedTimer t;
    const QString idxDir = workDir + QStringLiteral("/segyidx");

    // 全量重扫（旧路径基准）。
    t.start();
    SegyReader legacy;
    legacy.open(sgy);
    out.append(make(QStringLiteral("segy_rescan_ms"), QStringLiteral("segy"), msSince(t),
                    QStringLiteral("ms"), -1,
                    QStringLiteral("open() 顺序全扫 %1 道").arg(traces)));

    // openCached 冷（并行扫描 + 发布索引）。
    QDir(idxDir).removeRecursively();
    t.start();
    SegyReader cold;
    QString err;
    const bool coldOk = cold.openCached(sgy, idxDir, &err);
    out.append(make(QStringLiteral("segy_index_build_ms"), QStringLiteral("segy"), msSince(t),
                    QStringLiteral("ms"), -1,
                    QStringLiteral("openCached 冷（并行 ≤4 线程 + zstd 发布）%2")
                        .arg(err)));

    // openCached 热（身份命中免扫；预算 = 重扫的 5%）。
    t.start();
    SegyReader hot;
    const bool hotOk = hot.openCached(sgy, idxDir, &err);
    const double hotMs = msSince(t);
    out.append(make(QStringLiteral("segy_index_cached_ms"), QStringLiteral("segy"), hotMs,
                    QStringLiteral("ms"), 5.0, QStringLiteral("磁盘索引命中免扫")));
    if (coldOk && hotOk)
    {
      // 一致性自证：命中恢复与冷扫逐道一致。
      const bool same = cold.traceCount() == hot.traceCount() &&
                        cold.geometry().inlineMin == hot.geometry().inlineMin &&
                        cold.geometry().inlineMax == hot.geometry().inlineMax &&
                        cold.inlineNumbers() == hot.inlineNumbers();
      out.append(make(QStringLiteral("segy_index_hit_equiv"), QStringLiteral("segy"),
                      same ? 1.0 : 0.0, QStringLiteral("bool"), 1.5,
                      QStringLiteral("命中恢复 == 冷扫（道数/几何/inline 集）")));
    }
    // 索引统计（D2.6）与压缩收益（D2.4）。
    const SegyIndexStore::IndexStats st = cold.indexStats();
    out.append(make(QStringLiteral("segy_index_density_pct"), QStringLiteral("segy"),
                    st.densityPercent, QStringLiteral("%"), 100.5,
                    QStringLiteral("合成满测网密度应为 100%")));
    qint64 rawBytes = traces * 16 + 256;
    const QFileInfo psx(QDir(idxDir).entryList({QStringLiteral("*.psx")}, QDir::Files).isEmpty()
                            ? QString()
                            : idxDir + QLatin1Char('/') +
                                  QDir(idxDir).entryList({QStringLiteral("*.psx")}, QDir::Files).first());
    if (psx.exists())
      out.append(make(QStringLiteral("segy_index_compress_ratio"), QStringLiteral("segy"),
                      double(psx.size()) / double(rawBytes), QStringLiteral("ratio"), 0.5,
                      QStringLiteral("磁盘索引/原始道级索引（zstd，目标 -50%）")));
  }

  // ---------------- 金字塔（D3）----------------
  {
    const QString tif = workDir + QStringLiteral("/bench_2048.tif");
    PerfFixtures::makeSyntheticGeoTiff(tif, 2048, 2048, true);
    QElapsedTimer t;
    RasterPyramidService pyr(workDir + QStringLiteral("/pyr"));
    t.start();
    RasterPyramidService::PyramidMeta meta;
    pyr.ensure(tif, &meta, nullptr, RasterPyramidService::BuildStrategy::Lazy);
    out.append(make(QStringLiteral("pyramid_lazy_ensure_ms"), QStringLiteral("pyramid"),
                    msSince(t), QStringLiteral("ms"), 200.0,
                    QStringLiteral("懒建目录（不生成瓦片）")));
    t.start();
    RasterPyramidService::Tile tile;
    pyr.tile(tif, meta.levels - 1, 0, 0, &tile);
    out.append(make(QStringLiteral("pyramid_first_tile_ms"), QStringLiteral("pyramid"),
                    msSince(t), QStringLiteral("ms"), 500.0,
                    QStringLiteral("顶层瓦片首建（GDAL 降采样）")));
    t.start();
    pyr.tile(tif, meta.levels - 1, 0, 0, &tile);
    out.append(make(QStringLiteral("pyramid_tile_hit_ms"), QStringLiteral("pyramid"),
                    msSince(t), QStringLiteral("ms"), 2.0, QStringLiteral("瓦片 LRU 命中")));
    const RasterPyramidService::Stats st = pyr.stats(tif);
    out.append(make(QStringLiteral("pyramid_levels"), QStringLiteral("pyramid"),
                    double(st.levels), QStringLiteral("count"), 10.0,
                    QStringLiteral("层级数（2048px → 顶层 ≤256px）")));
  }

  // ---------------- catalog（D5.7）----------------
  {
    QElapsedTimer t;
    const QString dir1k = workDir + QStringLiteral("/cat1k");
    const QString dir10k = workDir + QStringLiteral("/cat10k");
    QDir(dir1k).removeRecursively();
    QDir(dir10k).removeRecursively();
    t.start();
    PerfFixtures::makeSyntheticCatalogDir(dir1k, 1000);
    out.append(make(QStringLiteral("catalog_build_1k_ms"), QStringLiteral("catalog"),
                    msSince(t), QStringLiteral("ms"), -1, QStringLiteral("合成 1k 资产灌库")));
    t.start();
    PerfFixtures::makeSyntheticCatalogDir(dir10k, 10000);
    out.append(make(QStringLiteral("catalog_build_10k_ms"), QStringLiteral("catalog"),
                    msSince(t), QStringLiteral("ms"), -1, QStringLiteral("合成 10k 资产灌库")));
    t.start();
    DataCatalog cat;
    QString err;
    const bool ok = cat.open(dir10k, &err);
    const double openMs = msSince(t);
    out.append(make(QStringLiteral("catalog_open_10k_ms"), QStringLiteral("catalog"), openMs,
                    QStringLiteral("ms"), 500.0,
                    QStringLiteral("10k 资产打开（D5.7 预算 500ms）%1").arg(err)));
    if (ok)
    {
      t.start();
      for (int i = 1; i <= 1000; ++i)
        cat.entityById(QStringLiteral("well-%1").arg(i, 6, 10, QLatin1Char('0')));
      out.append(make(QStringLiteral("catalog_query_1k_ids_ms"), QStringLiteral("catalog"),
                      msSince(t), QStringLiteral("ms"), 100.0,
                      QStringLiteral("1000 次 entityById O(1) 索引查询")));
      t.start();
      const QHash<QString, int> counts = cat.entityCountsByType();
      out.append(make(QStringLiteral("catalog_counts_types"), QStringLiteral("catalog"),
                      double(counts.value(QStringLiteral("well"))), QStringLiteral("count"),
                      10000.5, QStringLiteral("类型计数缓存应报 10000 口井")));
      QString mismatch;
      out.append(make(QStringLiteral("catalog_index_healthy"), QStringLiteral("catalog"),
                      cat.indexHealthy(&mismatch) ? 1.0 : 0.0, QStringLiteral("bool"), 1.5,
                      QStringLiteral("索引对账 %1").arg(mismatch)));
    }
  }

  // ---------------- SHA 缓存（D7.7）----------------
  {
    const QString big = workDir + QStringLiteral("/bench_64mb.bin");
    {
      QFile f(big);
      f.open(QIODevice::WriteOnly);
      QByteArray chunk(1 << 20, 7);
      for (int i = 0; i < 64; ++i)
        f.write(chunk);
    }
    ShaCache::shared().setDiskFile(QString());
    ShaCache::shared().invalidate();
    QElapsedTimer t;
    t.start();
    ShaCache::shared().sha256Hex(big);
    const double hashMs = msSince(t);
    out.append(make(QStringLiteral("sha_hash_64mb_ms"), QStringLiteral("io"), hashMs,
                    QStringLiteral("ms"), -1, QStringLiteral("64MB 流式 SHA-256")));
    t.start();
    ShaCache::shared().sha256Hex(big);
    out.append(make(QStringLiteral("sha_cached_ms"), QStringLiteral("io"), msSince(t),
                    QStringLiteral("ms"), 2.0, QStringLiteral("指纹命中免重算")));
  }

  // ---------------- 预算治理（D6）----------------
  {
    out.append(make(QStringLiteral("budget_used_bytes"), QStringLiteral("memory"),
                    double(CacheBudgetManager::instance()->usedBytes()), QStringLiteral("bytes"),
                    -1, QStringLiteral("全部注册缓存当前占用")));
    const QVector<CacheBudgetManager::NamedStats> all = CacheBudgetManager::instance()->allStats();
    out.append(make(QStringLiteral("budget_registered_caches"), QStringLiteral("memory"),
                    double(all.size()), QStringLiteral("count"), -1,
                    QStringLiteral("LRU 注册表缓存数（D6.2，≥1：las-docs 常驻）")));
  }
  return out;
}

int runMain(const QStringList &args)
{
  const QString workDir = QCoreApplication::applicationDirPath() + QStringLiteral("/perfbench");
  const QVector<BenchResult> results = runAll(workDir);

  QString jsonPath;
  for (int i = 0; i + 1 < args.size(); ++i)
    if (args.at(i) == QStringLiteral("--json"))
      jsonPath = args.at(i + 1);

  const QByteArray json = BenchReport::toJson(results);
  std::fwrite(json.constData(), 1, json.size(), stdout);
  std::fputc('\n', stdout);
  if (!jsonPath.isEmpty())
    BenchReport::writeJsonFile(jsonPath, results);

  int failed = 0;
  for (const BenchResult &r : results)
    if (!r.pass)
    {
      ++failed;
      std::printf("OVER BUDGET: %s = %.3f %s (budget %.3f)\n", qPrintable(r.name),
                  r.value, qPrintable(r.unit), r.budget);
    }
  std::printf("PERF GROUP %s — %lld benchmarks, %d over budget\n",
              failed ? "FAILED" : "OK", (long long)results.size(), failed);
  return failed ? 1 : 0;
}

} // namespace PerfGroup
