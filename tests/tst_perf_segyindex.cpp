// tst_perf_segyindex — wave/io-perf-cache D2.9/D2.10：并行扫描等价性与
// 缓存命中性能预算。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/segyindexstore.h"
#include "io/segyreader.h"
#include "perfbudget_relax.h"
#include "segydiskcachebudget.h"

#include <QDir>
#include <QTemporaryDir>
#include <algorithm>
#include <array>

class PerfSegyIndexTests : public QObject
{
    Q_OBJECT

  private slots:
    void parallelScanMatchesSequential_data();
    void parallelScanMatchesSequential();
    void cacheHitWithinBudget();
    void coldBuildWithinBudget();
    void progressReachesHundredOnHit();
    void statsSummaryText();
    void cacheInvalidatedByRemove();
    void parallelBadTraceTolerated_data();
    void parallelBadTraceTolerated();
    void identityHelpers();

  private:
    QTemporaryDir m_dir;
};

void PerfSegyIndexTests::parallelScanMatchesSequential_data()
{
  QTest::addColumn<int>("side");
  QTest::newRow("compact") << 100;
  QTest::newRow("parallel") << 150;
}

void PerfSegyIndexTests::parallelScanMatchesSequential()
{
  QFETCH(int, side);
  // D2.9：并行分片扫描与顺序 open() 的索引逐道一致。
  const QString sgy = m_dir.filePath(QStringLiteral("par-%1.sgy").arg(side));
  const int inl = side, xl = side, samples = 40;
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
  SegyStoredIndex cached, reference;
  QVERIFY(par.snapshot(&cached) && seq.snapshot(&reference));
  QCOMPARE(cached.inlineNos, reference.inlineNos);
  QCOMPARE(cached.xlineNos, reference.xlineNos);
  QCOMPARE(cached.offsets, reference.offsets);
  for (int corner = 0; corner < 4; ++corner) {
    QCOMPARE(cached.geometry.cornerX[corner], reference.geometry.cornerX[corner]);
    QCOMPARE(cached.geometry.cornerY[corner], reference.geometry.cornerY[corner]);
  }
}

void PerfSegyIndexTests::cacheHitWithinBudget()
{
  paleo::test::verifySegyDiskCacheBudget(m_dir, 0.42);
}

void PerfSegyIndexTests::coldBuildWithinBudget()
{
  std::array<double, 5> times{};
  QStringList sampleMs;
  for (size_t i = 0; i < times.size(); ++i) {
    const QString sgy = m_dir.filePath(QStringLiteral("cold-%1.sgy").arg(i));
    const QString dir = m_dir.filePath(QStringLiteral("idx-cold-%1").arg(i));
    QCOMPARE(PerfFixtures::makeSyntheticSegy(sgy, 100, 100, 50), 10000);
    const QString index = SegyIndexStore(dir).cacheFilePathFor(QFileInfo(sgy));
    QVERIFY(!QFileInfo::exists(index));
    SegyReader reader;
    QString error;
    QElapsedTimer t;
    t.start();
    QVERIFY2(reader.openCached(sgy, dir, &error), qPrintable(error));
    times[i] = t.nsecsElapsed() / 1.0e6;
    QCOMPARE(reader.traceCount(), 10000);
    QVERIFY(QFileInfo(index).size() > 0); // openCached may succeed after a failed cache write
    sampleMs.append(QString::number(times[i], 'f', 2));
  }
  std::sort(times.begin(), times.end());
  const double median = times[times.size() / 2];
  qInfo().noquote() << "SEG-Y cold build ms:" << sampleMs.join(", ") << "median" << median << "max" << times.back();
  QVERIFY2(median < paleo::perfbudget::relaxedBudgetMs(50),
           qPrintable(QStringLiteral("cold build median %1ms >= 50ms").arg(median)));
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

void PerfSegyIndexTests::parallelBadTraceTolerated_data()
{
  parallelScanMatchesSequential_data();
}

void PerfSegyIndexTests::parallelBadTraceTolerated()
{
  QFETCH(int, side);
  // 并行路径 + 探测段外坏道：等价性与坏道记录（D2.7 x D2.9 组合）。
  const QString sgy = m_dir.filePath(QStringLiteral("pbad-%1.sgy").arg(side));
  const int inl = side, xl = side, samples = 40;
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
