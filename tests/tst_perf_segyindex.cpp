// tst_perf_segyindex — wave/io-perf-cache D2.9/D2.10：并行扫描等价性与
// 缓存命中性能预算。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/segyindexstore.h"
#include "io/segyreader.h"

#include <QDir>
#include <QTemporaryDir>

class PerfSegyIndexTests : public QObject
{
    Q_OBJECT

  private slots:
    void parallelScanMatchesSequential();
    void cacheHitWithinBudget();
    void coldBuildWithinBudget();
    void progressReachesHundredOnHit();
    void statsSummaryText();
    void cacheInvalidatedByRemove();
    void parallelBadTraceTolerated();
    void identityHelpers();

  private:
    QTemporaryDir m_dir;
};

void PerfSegyIndexTests::parallelScanMatchesSequential()
{
  // D2.9：并行分片扫描与顺序 open() 的索引逐道一致。
  const QString sgy = m_dir.filePath("par.sgy");
  const int inl = 150, xl = 150, samples = 40; // ~7MB → 并行路径
  QCOMPARE(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples), inl * xl);

  SegyReader seq;
  QString err;
  QVERIFY(seq.open(sgy, &err));
  SegyReader par;
  const QString idx = m_dir.filePath("idx-par");
  QVERIFY(par.openCached(sgy, idx, &err));

  QCOMPARE(par.traceCount(), seq.traceCount());
  QCOMPARE(par.inlineNumbers(), seq.inlineNumbers());
  QCOMPARE(par.crosslineNumbers(), seq.crosslineNumbers());
  QCOMPARE(par.geometry().inlineMin, seq.geometry().inlineMin);
  QCOMPARE(par.geometry().inlineMax, seq.geometry().inlineMax);
  QCOMPARE(par.geometry().xlineMin, seq.geometry().xlineMin);
  QCOMPARE(par.geometry().xlineMax, seq.geometry().xlineMax);
  QCOMPARE(par.geometry().startTimeMs, seq.geometry().startTimeMs);
  QCOMPARE(par.sampleIntervalUs(), seq.sampleIntervalUs());
}

void PerfSegyIndexTests::cacheHitWithinBudget()
{
  const QString sgy = m_dir.filePath("hit.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 100, 100, 50) > 0);
  const QString idx = m_dir.filePath("idx-hit");
  SegyReader warmup;
  QString err;
  QVERIFY(warmup.openCached(sgy, idx, &err));

  QElapsedTimer t;
  t.start();
  SegyReader hit;
  QVERIFY(hit.openCached(sgy, idx, &err));
  const double ms = t.nsecsElapsed() / 1.0e6;
  // goal/perf-systematize 簇3：比率化——同夹具冷重建为在测参照（实测命中
  // ≈0.8ms/冷建 ≈6ms，ratios.json 基线 segy_cached_vs_rebuild_max=0.35；
  // 门取基线×1.2=0.42，缓存退化时比率→1 必红）。sanity 只拦挂死。
  const QString coldIdx = m_dir.filePath(QStringLiteral("idx-coldref"));
  t.restart();
  SegyReader coldRef;
  QVERIFY(coldRef.openCached(sgy, coldIdx, &err));
  const double coldMs = double(t.nsecsElapsed()) / 1.0e6;
  qInfo("index hit %.2fms vs cold rebuild %.1fms (ratio %.3f)", ms, coldMs,
        coldMs > 0 ? ms / coldMs : -1.0);
  QVERIFY2(coldMs > 0 && ms < 0.42 * coldMs,
           qPrintable(QStringLiteral("index hit %1ms >= 0.42×冷建 %2ms（索引缓存未生效）")
                          .arg(ms, 0, 'f', 2)
                          .arg(coldMs, 0, 'f', 1)));
  QVERIFY2(ms < 500.0,
           qPrintable(QStringLiteral("index hit %1ms >= 500ms（sanity）").arg(ms)));
  QCOMPARE(hit.traceCount(), 10000);
}

void PerfSegyIndexTests::coldBuildWithinBudget()
{
  const QString sgy = m_dir.filePath("cold.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 100, 100, 50) > 0);
  const QString idx = m_dir.filePath("idx-cold"); // 空 → 冷建
  QElapsedTimer t;
  t.start();
  SegyReader r;
  QString err;
  QVERIFY(r.openCached(sgy, idx, &err));
  const double ms = t.nsecsElapsed() / 1.0e6;
  // 冷建（扫描 10000 道 + zstd 发布）预算：重扫实测 ~6ms，预算 50ms 留余量。
  QVERIFY2(ms < 50.0, qPrintable(QStringLiteral("cold build %1ms >= 50ms").arg(ms)));
}

void PerfSegyIndexTests::progressReachesHundredOnHit()
{
  // D2.10：命中路径发一次 (size,size)——百分比立刻收敛。
  const QString sgy = m_dir.filePath("prog.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 40, 40, 30) > 0);
  const QString idx = m_dir.filePath("idx-prog");
  SegyReader warm;
  QString err;
  QVERIFY(warm.openCached(sgy, idx, &err));

  qint64 lastDone = -1, lastTotal = -1;
  int events = 0;
  SegyOptions opts;
  opts.progress = [&](qint64 done, qint64 total) {
    lastDone = done;
    lastTotal = total;
    ++events;
  };
  SegyReader hit;
  QVERIFY(hit.openCached(sgy, idx, &err, &opts));
  QVERIFY(events >= 1);
  QCOMPARE(lastDone, QFileInfo(sgy).size());
  QCOMPARE(lastTotal, QFileInfo(sgy).size());
}

void PerfSegyIndexTests::statsSummaryText()
{
  const QString sgy = m_dir.filePath("sum.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 20, 20, 10) > 0);
  SegyReader r;
  QString err;
  QVERIFY(r.open(sgy, &err));
  const QString text = SegyIndexStore::statsSummary(r.indexStats());
  QVERIFY(text.contains(QStringLiteral("traces=400")));
  QVERIFY(text.contains(QStringLiteral("inline=[1000..1019]")));
  QVERIFY(text.contains(QStringLiteral("xline=[2000..2019]")));
  QVERIFY(text.contains(QStringLiteral("missing=0")));
}

void PerfSegyIndexTests::cacheInvalidatedByRemove()
{
  // store.remove 后 load 不命中（重新走扫描路径仍成功）。
  const QString sgy = m_dir.filePath("rm.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 30, 30, 20) > 0);
  const QString idx = m_dir.filePath("idx-rm");
  SegyReader warm;
  QString err;
  QVERIFY(warm.openCached(sgy, idx, &err));
  SegyIndexStore store(idx);
  QVERIFY(store.remove(QFileInfo(sgy)));
  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), false, &reason).has_value());
  SegyReader again;
  QVERIFY(again.openCached(sgy, idx, &err)); // 重建
  QCOMPARE(again.traceCount(), 900);
}

void PerfSegyIndexTests::parallelBadTraceTolerated()
{
  // 并行路径 + 探测段外坏道：等价性与坏道记录（D2.7 x D2.9 组合）。
  const QString sgy = m_dir.filePath("pbad.sgy");
  const int inl = 110, xl = 110, samples = 40;
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  const qint64 traceSize = 240 + qint64(samples) * 4;
  const qint64 badOff = 3600 + 6000 * traceSize;
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(badOff + 114);
  const char bad[2] = {static_cast<char>(0x80), 0x00};
  f.write(bad, 2);
  f.close();

  const QString idx = m_dir.filePath("idx-pbad");
  SegyReader r;
  QString err;
  QVERIFY(r.openCached(sgy, idx, &err));
  QCOMPARE(r.traceCount(), inl * xl - 1);
  QCOMPARE(r.badTraceOffsets(), QVector<qint64>{badOff});
  // 命中恢复后坏道表同样在。
  SegyReader hit;
  QVERIFY(hit.openCached(sgy, idx, &err));
  QCOMPARE(hit.badTraceOffsets().size(), 1);
}

void PerfSegyIndexTests::identityHelpers()
{
  const QString sgy = m_dir.filePath("id.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 5, 5, 10) > 0);
  const QFileInfo fi(sgy);
  const SegyIndexStore::Identity a = SegyIndexStore::identityOf(fi);
  const SegyIndexStore::Identity b = SegyIndexStore::identityOf(fi);
  QVERIFY(a.matches(b));
  QVERIFY(a.size > 0);
  QVERIFY(!a.canonicalPath.isEmpty());
  const QByteArray fp = SegyIndexStore::prefixFingerprintOf(sgy, fi.size());
  QCOMPARE(fp.size(), 64); // sha256 hex
  QVERIFY(!SegyIndexStore::prefixFingerprintOf(sgy, 0).isEmpty() == false);
}

QTEST_MAIN(PerfSegyIndexTests)
#include "tst_perf_segyindex.moc"
