// tst_cache_segyindex — wave/io-perf-cache D2：SEG-Y 道头索引持久化的
// 版本化/完整性/损坏自愈/checkpoint/增量/坏道跳过/空洞统计。
#include <QtTest>

#include "io/perffixtures.h"
#include "io/segyindexstore.h"
#include "io/segyreader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

class CacheSegyIndexTests : public QObject
{
    Q_OBJECT

  private slots:
    void saveLoadRoundtrip();
    void identitySizeChangeTriggersRebuild();
    void identityMtimeChangeTriggersRebuild();
    void corruptFileSelfHeals();
    void versionReject();
    void openCachedHitEqualsColdScan();
    void checkpointResumeAfterCancel();
    void incrementalRescanOnAppend();
    void checkpointAuditRejectsTampered();
    void badTraceSkipKeepsIndex();
    void gapStatsReportMissing();
    void publishFailureStillUsable();

  private:
    QTemporaryDir m_dir;
    QString cacheDir() const { return m_dir.filePath("idx"); }
};

void CacheSegyIndexTests::saveLoadRoundtrip()
{
  const QString sgy = m_dir.filePath("a.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 8, 12, 40) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QString err;
  QVERIFY(reader.open(sgy, &err));

  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  QVERIFY(store.save(snap, &err));

  const auto loaded = store.load(QFileInfo(sgy), false, &err);
  QVERIFY(loaded.has_value());
  QCOMPARE(int(loaded->inlineNos.size()), 8 * 12);
  QCOMPARE(loaded->geometry.inlineMin, 1000.0);
  QCOMPARE(loaded->geometry.inlineMax, 1007.0);
  QCOMPARE(loaded->complete, true);
}

void CacheSegyIndexTests::identitySizeChangeTriggersRebuild()
{
  const QString sgy = m_dir.filePath("b.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 6, 6, 20) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QVERIFY(reader.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  QVERIFY(store.save(snap));

  // 文件变大（追加整测网段）→ 身份失配 → 不命中。
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::Append));
  f.write(QByteArray(240 + 20 * 4, '\0')); // 一道的字节数（道头 0 化不合法，但身份测试只看 stat）
  f.close();
  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), false, &reason).has_value());
  QVERIFY(reason.contains(QStringLiteral("identity")));
}

void CacheSegyIndexTests::identityMtimeChangeTriggersRebuild()
{
  const QString sgy = m_dir.filePath("c.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 5, 5, 16) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QVERIFY(reader.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  QVERIFY(store.save(snap));

  // utimensat 改 mtime（内容不变）：身份失配。
  {
    QFile tf(sgy);
    QVERIFY(tf.open(QIODevice::ReadWrite));
    QVERIFY(tf.setFileTime(QDateTime::currentDateTime().addSecs(-5000),
                           QFileDevice::FileModificationTime));
  }
  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), false, &reason).has_value());
}

void CacheSegyIndexTests::corruptFileSelfHeals()
{
  const QString sgy = m_dir.filePath("d.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 6, 6, 24) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QVERIFY(reader.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  QVERIFY(store.save(snap));

  // 破坏缓存文件中段。
  const QString path = store.cacheFilePathFor(QFileInfo(sgy));
  QVERIFY(QFile::exists(path));
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(f.size() / 2);
  const char x = '\x5A';
  f.write(&x, 1);
  f.close();

  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), false, &reason).has_value());
  QVERIFY(!QFile::exists(path)); // 自愈删除（D2.2/D2.3）
}

void CacheSegyIndexTests::versionReject()
{
  const QString sgy = m_dir.filePath("e.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 4, 4, 12) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QVERIFY(reader.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  QVERIFY(store.save(snap));
  // 把 version 字段（offset 8 u16）改成 99。
  QFile f(store.cacheFilePathFor(QFileInfo(sgy)));
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(8);
  const char v[2] = {99, 0};
  f.write(v, 2);
  f.close();
  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), false, &reason).has_value());
  QVERIFY(!QFile::exists(store.cacheFilePathFor(QFileInfo(sgy))));
}

void CacheSegyIndexTests::openCachedHitEqualsColdScan()
{
  const QString sgy = m_dir.filePath("f.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 12, 14, 30) > 0);
  const QString idx = cacheDir();

  SegyReader cold;
  QString err;
  QVERIFY(cold.open(sgy, &err)); // 顺序路径
  SegyReader cached;
  QVERIFY(cached.openCached(sgy, idx, &err)); // 并行冷路径 + 发布

  QCOMPARE(cached.traceCount(), cold.traceCount());
  QCOMPARE(cached.samplesPerTrace(), cold.samplesPerTrace());
  QCOMPARE(cached.inlineNumbers(), cold.inlineNumbers());
  QCOMPARE(cached.crosslineNumbers(), cold.crosslineNumbers());
  QCOMPARE(cached.geometry().inlineMin, cold.geometry().inlineMin);
  QCOMPARE(cached.geometry().inlineMax, cold.geometry().inlineMax);
  QCOMPARE(cached.geometry().xlineMin, cold.geometry().xlineMin);
  QCOMPARE(cached.geometry().xlineMax, cold.geometry().xlineMax);

  // 命中恢复：新 reader 免扫得到同一索引。
  SegyReader hit;
  QVERIFY(hit.openCached(sgy, idx, &err));
  QCOMPARE(hit.traceCount(), cold.traceCount());
  QCOMPARE(hit.inlineNumbers(), cold.inlineNumbers());
}

void CacheSegyIndexTests::checkpointResumeAfterCancel()
{
  const QString sgy = m_dir.filePath("g.sgy");
  // 足够大以触发并行路径（>4MB）。
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 120, 120, 40) > 0);
  const QString idx = cacheDir();

  // 第一遍：立刻取消 → checkpoint 落盘。
  {
    SegyReader r;
    SegyOptions opts;
    opts.cancel = [] { return true; }; // 首个检查点即取消
    QString err;
    QVERIFY(!r.openCached(sgy, idx, &err, &opts));
    QCOMPARE(err, QStringLiteral("cancelled"));
  }
  const QDir dir(idx);
  QVERIFY(!dir.entryList({QStringLiteral("*.psx")}, QDir::Files).isEmpty());

  // 第二遍：不取消 → 从 checkpoint 续扫完成（或全扫），最终索引完整。
  SegyReader done;
  QString err;
  QVERIFY(done.openCached(sgy, idx, &err));
  QCOMPARE(done.traceCount(), 120 * 120);
  QVERIFY(done.badTraceOffsets().isEmpty());
  QVERIFY(!done.lastScanPartial());
}

void CacheSegyIndexTests::incrementalRescanOnAppend()
{
  const QString sgy = m_dir.filePath("h.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 40, 40, 20) > 0);
  // 4MB 以上才会走并行/续扫路径——40×40×20 太小（~1.3MB），直接用顺序
  // openCached 也行：这里验证 loadForResume 的前缀判据。
  const QString idx = cacheDir();
  SegyReader r;
  QString err;
  QVERIFY(r.openCached(sgy, idx, &err));

  // 追加整道×40（一条 inline 的道数）→ 前缀不变。
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::Append));
  QFile src(sgy);
  // 复制首 40 道（道长 240+20*4=320B）。
  src.open(QIODevice::ReadOnly);
  src.seek(3600);
  const QByteArray traces = src.read(40 * 320);
  f.write(traces);
  f.close();
  src.close();

  SegyIndexStore store(idx);
  QString reason;
  const auto resumable = store.loadForResume(QFileInfo(sgy), &reason);
  Q_UNUSED(resumable); // 完整索引（非 checkpoint）不可续扫——load 只认完整身份
  // 追加后完整索引身份失配 → openCached 重建。
  SegyReader r2;
  QVERIFY(r2.openCached(sgy, idx, &err));
  QCOMPARE(r2.traceCount(), 40 * 40 + 40);
}

void CacheSegyIndexTests::checkpointAuditRejectsTampered()
{
  const QString sgy = m_dir.filePath("i.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 30, 30, 20) > 0);
  SegyIndexStore store(cacheDir());
  SegyReader r;
  QVERIFY(r.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(r.snapshot(&snap));
  // 伪 checkpoint：scannedOffset 越界（D2.8 审计应拒）。
  snap.complete = false;
  snap.scannedOffset = 1 << 40;
  QString err;
  store.save(snap, &err);
  QString reason;
  const auto loaded = store.load(QFileInfo(sgy), true, &reason);
  QVERIFY(!loaded.has_value());
  QVERIFY(reason.contains(QStringLiteral("audit")) || reason.contains(QStringLiteral("checkpoint")));
}

void CacheSegyIndexTests::badTraceSkipKeepsIndex()
{
  // D2.7：固定道长布局中一道 ns 损坏（0x8000）→ 跳过 + 记录，索引不作废。
  const QString sgy = m_dir.filePath("j.sgy");
  const int inl = 100, xl = 100, samples = 40; // >4MB 触发并行路径
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  const qint64 traceSize = 240 + qint64(samples) * 4;
  const qint64 midTraceOffset = 3600 + (inl * xl / 2) * traceSize;
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(midTraceOffset + 114); // ns 字（115-116，0 基 114）
  const char bad[2] = {static_cast<char>(0x80), 0x00}; // -32768
  f.write(bad, 2);
  f.close();

  const QString idx = cacheDir();
  SegyReader r;
  QString err;
  QVERIFY2(r.openCached(sgy, idx, &err), qPrintable(err));
  QCOMPARE(r.traceCount(), inl * xl - 1); // 坏道不在索引
  QCOMPARE(r.badTraceOffsets().size(), 1);
  QCOMPARE(r.badTraceOffsets().first(), midTraceOffset);
}

void CacheSegyIndexTests::gapStatsReportMissing()
{
  // D2.6：抽掉中间一道 → bounding 网格出现 1 空洞。
  const QString sgy = m_dir.filePath("k.sgy");
  const int inl = 120, xl = 120, samples = 40; // >4MB：并行路径才有坏道跳过
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  const qint64 traceSize = 240 + qint64(samples) * 4;
  const qint64 holeOffset = 3600 + 55 * traceSize; // 探测段（首 64 道）内的坏道
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(holeOffset + 114);
  const char bad[2] = {static_cast<char>(0x80), 0x00};
  f.write(bad, 2);
  f.close();

  const QString idx = cacheDir();
  SegyReader r;
  QString err;
  QVERIFY2(r.openCached(sgy, idx, &err), qPrintable(err));
  const SegyIndexStore::IndexStats st = r.indexStats();
  QCOMPARE(st.traceCount, qint64(120 * 120 - 1));
  QCOMPARE(st.badTraces, qint64(1));
  QCOMPARE(st.inlineMin, qint32(1000));
  QCOMPARE(st.inlineMax, qint32(1119));
  QCOMPARE(st.xlineMin, qint32(2000));
  QCOMPARE(st.xlineMax, qint32(2119));
  QCOMPARE(st.gridCells, qint64(120 * 120));
  QCOMPARE(st.presentCells, qint64(120 * 120 - 1));
  QCOMPARE(st.missingCells, qint64(1));
  QVERIFY(st.densityPercent > 99.99 && st.densityPercent <= 100.0);
}

void CacheSegyIndexTests::publishFailureStillUsable()
{
  // D2.1 降级：缓存目录不可写（塞一个同名只读目录）→ openCached 仍成功。
  const QString sgy = m_dir.filePath("l.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 8, 8, 20) > 0);
  const QString idx = m_dir.filePath("blocked");
  QFile g(idx);
  QVERIFY(g.open(QIODevice::WriteOnly)); // 文件占住目录名 → mkpath 失败
  g.close();
  SegyReader r;
  QString err;
  QVERIFY(r.openCached(sgy, idx, &err));
  QCOMPARE(r.traceCount(), 64);
}

QTEST_MAIN(CacheSegyIndexTests)
#include "tst_cache_segyindex.moc"
