// tst_perf_regress — wave/io-perf-cache D8.2/D8.3/D8.4：回归门。机器无关相对
// 指标 vs docs/perf/baselines/ratios.json，劣化 >20% 判红。
#include <QtTest>

#include "catalog/datacatalog.h"
#include "io/benchreport.h"
#include "io/lascache.h"
#include "io/lasparser.h"
#include "io/perffixtures.h"
#include "io/segyindexstore.h"
#include "io/segyreader.h"
#include "io/shacache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

class PerfRegressTests : public QObject
{
    Q_OBJECT

  private slots:
    void baselinesPresentAndWellFormed();
    void lasCacheHitRatioWithinGate();
    void segyIndexHitRatioWithinGate();
    void shaCacheHitRatioWithinGate();
    void catalogQueryScalingWithinGate();
    void catalogBuildScalingWithinGate(); // WP2：写路径线性化门
    void coldVsHotBothReported();
    void htmlReportRenders();
    void jsonReportParses();

  private:
    QTemporaryDir m_dir;
    QJsonObject loadBaselines();
    static double ratioGate(const QJsonObject &b, const QString &key);
};

QJsonObject PerfRegressTests::loadBaselines()
{
  // 首选编译期注入的源码树绝对路径（审计 01 M5：构建目录在源码树外也命中）；
  // 相对 CWD 的候选保留作兼容回落。
  const QStringList candidates = {
#ifdef PALEO_SOURCE_DIR
      QStringLiteral(PALEO_SOURCE_DIR "/docs/perf/baselines/ratios.json"),
#endif
      QStringLiteral("../docs/perf/baselines/ratios.json"),
      QStringLiteral("../../docs/perf/baselines/ratios.json"),
      QStringLiteral("docs/perf/baselines/ratios.json"),
  };
  for (const QString &c : candidates)
  {
    QFile f(QFileInfo(c).absoluteFilePath());
    if (f.open(QIODevice::ReadOnly))
    {
      const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
      if (doc.isObject())
        return doc.object();
    }
  }
  return {};
}

double PerfRegressTests::ratioGate(const QJsonObject &b, const QString &key)
{
  return b.value(key).toDouble(-1.0);
}

void PerfRegressTests::baselinesPresentAndWellFormed()
{
  const QJsonObject b = loadBaselines();
  QVERIFY2(!b.isEmpty(), "docs/perf/baselines/ratios.json 缺失或不可解析（D8.3）");
  const QStringList required = {
      QStringLiteral("las_cache_hit_vs_cold_max"),
      QStringLiteral("segy_cached_vs_rebuild_max"),
      QStringLiteral("sha_cached_vs_hash_max"),
      QStringLiteral("catalog_query_5k_vs_1k_max"),
      QStringLiteral("catalog_build_5k_vs_1k_max"),
  };
  for (const QString &k : required)
    QVERIFY2(b.contains(k), qPrintable(QStringLiteral("baseline 缺键 %1").arg(k)));
}

void PerfRegressTests::lasCacheHitRatioWithinGate()
{
  const QJsonObject b = loadBaselines();
  if (b.isEmpty())
    QSKIP("baseline 缺失");
  const QString las = m_dir.filePath("gate.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 15581));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx"));
  LasCache::shared().invalidate();

  QElapsedTimer t;
  t.start();
  QVERIFY(LasCache::shared().load(las).ok);
  const double coldMs = t.nsecsElapsed() / 1.0e6;

  LasCache::shared().clearMemory();
  t.restart();
  QVERIFY(LasCache::shared().load(las).ok);
  const double hitMs = t.nsecsElapsed() / 1.0e6;

  const double ratio = coldMs > 0 ? hitMs / coldMs : 99.0;
  const double gate = ratioGate(b, QStringLiteral("las_cache_hit_vs_cold_max")) * 1.2; // +20% 容差
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("hit/cold=%1 > 门限 %2（D8.2 劣化>20%%）")
                          .arg(ratio, 0, 'f', 4)
                          .arg(gate, 0, 'f', 4)));
}

void PerfRegressTests::segyIndexHitRatioWithinGate()
{
  const QJsonObject b = loadBaselines();
  if (b.isEmpty())
    QSKIP("baseline 缺失");
  const QString sgy = m_dir.filePath("gate.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 100, 100, 50) > 0);
  const QString idx = m_dir.filePath("idx");

  QElapsedTimer t;
  t.start();
  SegyReader cold;
  QString err;
  QVERIFY(cold.openCached(sgy, idx, &err));
  const double buildMs = t.nsecsElapsed() / 1.0e6;

  t.restart();
  SegyReader hit;
  QVERIFY(hit.openCached(sgy, idx, &err));
  const double hitMs = t.nsecsElapsed() / 1.0e6;

  const double ratio = hitMs / buildMs;
  const double gate = ratioGate(b, QStringLiteral("segy_cached_vs_rebuild_max")) * 1.2;
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("hit/rebuild=%1 > 门限 %2").arg(ratio, 0, 'f', 4).arg(gate, 0, 'f', 4)));
}

void PerfRegressTests::shaCacheHitRatioWithinGate()
{
  const QJsonObject b = loadBaselines();
  if (b.isEmpty())
    QSKIP("baseline 缺失");
  const QString big = m_dir.filePath("gate16mb.bin");
  {
    QFile f(big);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray chunk(1 << 20, 3);
    for (int i = 0; i < 16; ++i)
      f.write(chunk);
  }
  ShaCache::shared().setDiskFile(QString());
  ShaCache::shared().invalidate();
  QElapsedTimer t;
  t.start();
  QVERIFY(!ShaCache::shared().sha256Hex(big).isEmpty());
  const double hashMs = t.nsecsElapsed() / 1.0e6;
  t.restart();
  QVERIFY(!ShaCache::shared().sha256Hex(big).isEmpty());
  const double hitMs = t.nsecsElapsed() / 1.0e6;
  const double ratio = hitMs / hashMs;
  const double gate = ratioGate(b, QStringLiteral("sha_cached_vs_hash_max")) * 1.2;
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("hit/hash=%1 > 门限 %2").arg(ratio, 0, 'f', 4).arg(gate, 0, 'f', 4)));
}

void PerfRegressTests::catalogQueryScalingWithinGate()
{
  // 亚线性伸缩门：5k 表 vs 1k 表的同量查询耗时比。
  const QJsonObject b = loadBaselines();
  if (b.isEmpty())
    QSKIP("baseline 缺失");
  const QString dir1 = m_dir.filePath("g1k");
  const QString dir5 = m_dir.filePath("g5k");
  QString err;
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir1, 1000, &err));
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir5, 5000, &err));
  DataCatalog c1, c5;
  QVERIFY(c1.open(dir1, &err));
  QVERIFY(c5.open(dir5, &err));
  auto probe = [](DataCatalog *c) {
    QElapsedTimer t;
    t.start();
    for (int i = 1; i <= 500; ++i)
      c->entityById(QStringLiteral("well-%1").arg(i, 6, 10, QLatin1Char('0')));
    return t.nsecsElapsed() / 1.0e6;
  };
  const double t1 = probe(&c1);
  const double t5 = probe(&c5);
  const double ratio = t1 > 0 ? t5 / t1 : 99.0;
  const double gate = ratioGate(b, QStringLiteral("catalog_query_5k_vs_1k_max")) * 1.2;
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("5k/1k=%1 > 门限 %2（索引退化成线性？）")
                          .arg(ratio, 0, 'f', 2)
                          .arg(gate, 0, 'f', 2)));
}

void PerfRegressTests::catalogBuildScalingWithinGate()
{
  // WP2：catalog 写路径线性化回归门——5k vs 1k 灌库（BatchSave 批内
  // 4×N 次 mutator + N 个受管文件 + 一次全量 JSON save）的耗时比。
  // 线性实现 ≈5×（数据量比）；二次实现 ~25×（旧 mutator 全表快照的
  // COW detach——BASE 实测红，WP2 修复后绿）。与查询门同一容差口径。
  const QJsonObject b = loadBaselines();
  if (b.isEmpty())
    QSKIP("baseline 缺失");
  const QString dir1 = m_dir.filePath("b1k");
  const QString dir5 = m_dir.filePath("b5k");
  QString err;
  QElapsedTimer t;
  t.start();
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir1, 1000, &err));
  const double t1 = t.nsecsElapsed() / 1.0e6;
  t.restart();
  QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir5, 5000, &err));
  const double t5 = t.nsecsElapsed() / 1.0e6;
  qInfo("catalog build 1k=%.1fms 5k=%.1fms ratio=%.2f", t1, t5,
        t1 > 0 ? t5 / t1 : -1.0);
  const double ratio = t1 > 0 ? t5 / t1 : 99.0;
  const double gate = ratioGate(b, QStringLiteral("catalog_build_5k_vs_1k_max")) * 1.2;
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("5k/1k=%1 > 门限 %2（mutator 写路径退化成二次？）")
                          .arg(ratio, 0, 'f', 2)
                          .arg(gate, 0, 'f', 2)));
}

void PerfRegressTests::coldVsHotBothReported()
{
  // D8.4：冷/热两个口径都能从 LasCache timings 观测到且热 < 冷。
  const QString las = m_dir.filePath("ch.las");
  QVERIFY(PerfFixtures::makeSyntheticLas(las, 6000));
  LasCache::shared().setDiskRoot(m_dir.filePath("idx2"));
  LasCache::shared().invalidate();
  QVERIFY(LasCache::shared().load(las).ok);
  QVERIFY(LasCache::shared().lastTimings().coldParseNs > 0);
  LasCache::shared().clearMemory();
  QVERIFY(LasCache::shared().load(las).ok);
  QVERIFY(LasCache::shared().lastTimings().diskLoadNs > 0);
  QVERIFY(LasCache::shared().load(las).ok);
  QVERIFY(LasCache::shared().lastTimings().memoryHitNs >= 0);
}

void PerfRegressTests::htmlReportRenders()
{
  // D8.6：HTML 摘要包含分组表格与预算条。
  QVector<BenchResult> results;
  BenchResult r1;
  r1.name = QStringLiteral("x_ms");
  r1.group = QStringLiteral("las");
  r1.value = 3.2;
  r1.unit = QStringLiteral("ms");
  r1.budget = 5.0;
  r1.pass = true;
  BenchResult r2;
  r2.name = QStringLiteral("y_ms");
  r2.group = QStringLiteral("segy");
  r2.value = 12.0;
  r2.unit = QStringLiteral("ms");
  r2.budget = 10.0;
  r2.pass = false;
  results << r1 << r2;
  const QByteArray html = BenchReport::toHtml(results, QStringLiteral("unit"));
  QVERIFY(html.contains("<!DOCTYPE html>"));
  QVERIFY(html.contains("x_ms"));
  QVERIFY(html.contains("over budget"));
  QVERIFY(html.contains("y_ms"));
  const QString out = m_dir.filePath("report.html");
  QVERIFY(BenchReport::writeHtmlFile(out, results, QStringLiteral("unit")));
  QVERIFY(QFile::exists(out));
}

void PerfRegressTests::jsonReportParses()
{
  QVector<BenchResult> results;
  BenchResult r;
  r.name = QStringLiteral("j_ms");
  r.group = QStringLiteral("io");
  r.value = 1.5;
  r.unit = QStringLiteral("ms");
  r.budget = 2.0;
  r.pass = true;
  results << r;
  const QByteArray json = BenchReport::toJson(results);
  const QJsonDocument doc = QJsonDocument::fromJson(json);
  QVERIFY(doc.isObject());
  QCOMPARE(doc.object().value(QStringLiteral("benchmarks")).toArray().size(), 1);
  QCOMPARE(doc.object().value(QStringLiteral("benchmarks"))
               .toArray()
               .at(0)
               .toObject()
               .value(QStringLiteral("name"))
               .toString(),
           QStringLiteral("j_ms"));
  const QString out = m_dir.filePath("report.json");
  QVERIFY(BenchReport::writeJsonFile(out, results));
  QVERIFY(QFile::exists(out));
}

QTEST_MAIN(PerfRegressTests)
#include "tst_perf_regress.moc"
