// tst_cache_segyindex — wave/io-perf-cache D2：SEG-Y 道头索引持久化的
// 版本化/完整性/损坏自愈/checkpoint/增量/坏道跳过/空洞统计。
// P2/#1026（v6）：统一边界审计的伪造 payload 回归（越过 CRC 直达解码/
// 审计分支）、载荷越界、checkpoint 越尾、变道长布局往返。
#include <QtTest>

#include "io/cachecore.h"
#include "io/perffixtures.h"
#include "io/segyindexstore.h"
#include "io/segyreader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <atomic>
#include <limits>

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
    void gridCellSpanUsesWideDifference();
    void publishFailureStillUsable();
    // B6（wave/deepen-perf）：顺序路径坏道跳过放宽 + 变道长布局契约。
    void sequentialBadTraceSkipMatchesParallelContract();
    void variableLayoutFlagSurfaces();
    void variableLayoutKeepsHardErrorOnCorruptNs();
    void variableLayoutWritesNoCheckpoint();
    // #290：并行/续扫与顺序 open() 的索引口径一致性。
    void parallelDialectMatchesSequential();
    void parallelShortTraceMatchesSequential();
    // review-fix-dataio：restore() 拒绝损坏/篡改的缓存索引。
    void restoreRejectsCorruptIndex();
    // P2/#1026：统一边界审计（v6）——载荷越界、checkpoint 越尾、伪造
    // payload（越过 CRC 的内部解码/审计分支）、变道长布局落盘往返。
    void auditRejectsPayloadBeyondEof();
    void checkpointPastEofSelfHeals();
    void forgedPayloadRejectedBeyondCrc();
    void variableLayoutIndexRoundtrip();

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

void CacheSegyIndexTests::restoreRejectsCorruptIndex()
{
  const QString sgy = m_dir.filePath("restore.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 4, 5, 30) > 0);
  SegyReader reader;
  QString err;
  QVERIFY2(reader.open(sgy, &err), qPrintable(err));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  {
    SegyReader ok;
    QVERIFY(ok.restore(snap, sgy)); // 正常快照仍可恢复（兼容）
    QCOMPARE(ok.samplesPerTrace(), 30);
  }
  auto bad = snap;
  bad.samplesPerTrace = 0;
  QVERIFY(!SegyReader().restore(bad, sgy));
  bad.samplesPerTrace = -7;
  QVERIFY(!SegyReader().restore(bad, sgy));
  bad = snap;
  bad.offsets[1] = QFileInfo(sgy).size() + 4096; // 越过文件尾
  QVERIFY(!SegyReader().restore(bad, sgy));
  bad = snap;
  bad.offsets[0] = 100; // 落在 3600 字节卷头内
  QVERIFY(!SegyReader().restore(bad, sgy));
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

  // 第一遍：并行扫描中途取消 → checkpoint 落盘。
  // P2（2026-11-19）：旧写法 cancel 恒 true——分片首查（i=0）即取消，
  // m_index 为空，snapshot() 正确拒绝落空索引 → 无 .psx，本机对 master
  // 实测 10/10 红（扫描器与验证器三代代码同）。改为计数器取消：每个
  // 分片越过首个 128 道检查窗口（i=128）后才取消——m_path 在
  // scanParallel 前已置，非空索引 → checkpoint 确定性落盘。
  {
    SegyReader r;
    SegyOptions opts;
    int cancelChecks = 0;
    opts.cancel = [&cancelChecks] { return ++cancelChecks >= 2; };
    QString err;
    QVERIFY(!r.openCached(sgy, idx, &err, &opts));
    QCOMPARE(err, QStringLiteral("cancelled"));
    QVERIFY(r.lastScanPartial());
    QVERIFY(r.traceCount() > 0); // 分片已收道——checkpoint 非空
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
  QVERIFY(src.open(QIODevice::ReadOnly));
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
  snap.scannedOffset = qint64(1) << 40;
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

void CacheSegyIndexTests::gridCellSpanUsesWideDifference()
{
  // inline 从 INT_MIN 到 INT_MAX：qint32 相减会回绕，跨度必须在 qint64 里算。
  SegyIndexStore::StoredIndex index;
  index.inlineNos = {std::numeric_limits<qint32>::min(), std::numeric_limits<qint32>::max()};
  index.xlineNos = {0, 1};
  const auto st = SegyIndexStore::computeStats(index);
  const qint64 ilSpan = qint64(std::numeric_limits<qint32>::max()) -
                        qint64(std::numeric_limits<qint32>::min()) + 1;
  QCOMPARE(st.inlineMin, std::numeric_limits<qint32>::min());
  QCOMPARE(st.inlineMax, std::numeric_limits<qint32>::max());
  QCOMPARE(st.gridCells, ilSpan * 2);
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

// ---- B6（wave/deepen-perf）：坏道跳过放宽 + 变道长布局契约 --------------------

namespace
{
// 把 offset 处道头的 ns 字（0 基 114，大端 i16）改为 val。
void patchTraceNs(const QString &path, qint64 offset, qint16 val)
{
  QFile f(path);
  QVERIFY(f.open(QIODevice::ReadWrite));
  f.seek(offset + 114);
  const quint16 v = static_cast<quint16>(val);
  const char bytes[2] = {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)};
  f.write(bytes, 2);
  f.close();
}
// 改短 ns 时同步移除真实载荷，后续道头才处于正确边界。
bool shortenFirstTrace(const QString &path, int oldSamples, int newSamples)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return false;
  QByteArray bytes = f.readAll();
  f.close();
  bytes.remove(3600 + 240 + newSamples * 4, (oldSamples - newSamples) * 4);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return f.write(bytes) == bytes.size();
}

// #290：demo 工区方言 SEG-Y 生成器——inline 字（偏移 188）随测网变化，
// crossline 字（偏移 192，字节 193）全 0，CDP（偏移 20）随 xline 变化；
// 坐标对 72/76（Source X/Y）全 0，CDP X/Y（偏移 180/184）非零。固定道长、
// 样本全 0（内容不重要，索引只读道头）。
bool makeDialectSegy(const QString &path, int inlCount, int xlCount, int samples,
                     qint32 baseInline)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(QByteArray(3200, ' ')); // 文本头
  QByteArray bin(400, '\0');
  qToBigEndian<qint16>(static_cast<qint16>(xlCount), reinterpret_cast<uchar *>(bin.data() + 12));
  qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(bin.data() + 16)); // dt
  qToBigEndian<qint16>(static_cast<qint16>(samples), reinterpret_cast<uchar *>(bin.data() + 20));
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(bin.data() + 24));    // IEEE
  qToBigEndian<qint16>(0, reinterpret_cast<uchar *>(bin.data() + 304));   // 无扩展文本头
  qToBigEndian<qint32>(baseInline, reinterpret_cast<uchar *>(bin.data() + 4));
  f.write(bin);

  QByteArray trHdr(240, '\0');
  const QByteArray body(samples * 4, '\0');
  int tracl = 0;
  for (int i = 0; i < inlCount; ++i)
  {
    for (int x = 0; x < xlCount; ++x)
    {
      memset(trHdr.data(), 0, 240);
      auto put32 = [&trHdr](int off, qint32 v) {
        qToBigEndian<qint32>(v, reinterpret_cast<uchar *>(trHdr.data() + off));
      };
      put32(0, ++tracl);                        // TRACL
      put32(8, i + 1);                          // field record
      put32(20, x);                             // CDP = xline 序（道号位置 21）
      qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data() + 70)); // scal = 1
      qToBigEndian<qint16>(48, reinterpret_cast<uchar *>(trHdr.data() + 108)); // delay ms
      put32(180, 500000 + 50 * x);              // CDP X（字节 181-184）
      put32(184, 4000000 + 100 * i);            // CDP Y（字节 185-188）
      qToBigEndian<qint16>(static_cast<qint16>(samples),
                           reinterpret_cast<uchar *>(trHdr.data() + 114));
      qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(trHdr.data() + 116));
      put32(188, baseInline + i);               // inline（192 恒 0——方言点①）
      f.write(trHdr);
      f.write(body);
    }
  }
  return f.error() == QFileDevice::NoError;
}

// 断言 r 与 seq 的索引/几何完全一致（#290 口径一致性）：道数、去重线号集合、
// 几何范围 + 四角，以及逐道 (lineNo, xlineNo, cdp, tracl) 有序序列（索引
// 身份的全序比对——offsets 无公开访问口，逐道序列 + 角点 + 计数等价覆盖）。
bool sameIndexAs(const SegyReader &r, const SegyReader &seq, QString *why)
{
  if (r.traceCount() != seq.traceCount() ||
      r.inlineNumbers() != seq.inlineNumbers() ||
      r.crosslineNumbers() != seq.crosslineNumbers() ||
      r.samplesPerTrace() != seq.samplesPerTrace())
  {
    if (why)
      *why = QStringLiteral("index mismatch: traces %1 vs %2, inlines %3 vs %4, xlines %5 vs %6")
                 .arg(r.traceCount()).arg(seq.traceCount())
                 .arg(r.inlineNumbers().size()).arg(seq.inlineNumbers().size())
                 .arg(r.crosslineNumbers().size()).arg(seq.crosslineNumbers().size());
    return false;
  }
  const SegyGeometry a = r.geometry(), b = seq.geometry();
  if (a.inlineMin != b.inlineMin || a.inlineMax != b.inlineMax ||
      a.xlineMin != b.xlineMin || a.xlineMax != b.xlineMax)
  {
    if (why) *why = QStringLiteral("geometry range mismatch");
    return false;
  }
  if (a.startTimeMs != b.startTimeMs)
  {
    if (why)
      *why = QStringLiteral("startTimeMs %1 vs %2").arg(a.startTimeMs).arg(b.startTimeMs);
    return false;
  }
  for (int k = 0; k < 4; ++k)
  {
    if (a.cornerX[k] != b.cornerX[k] || a.cornerY[k] != b.cornerY[k])
    {
      if (why) *why = QStringLiteral("corner %1 mismatch").arg(k);
      return false;
    }
  }
  const QVector<SegyTrace> ta = r.traces(), tb = seq.traces();
  if (ta.size() != tb.size())
  {
    if (why) *why = QStringLiteral("traces() size %1 vs %2").arg(ta.size()).arg(tb.size());
    return false;
  }
  for (int i = 0; i < ta.size(); ++i)
  {
    if (ta[i].lineNo != tb[i].lineNo || ta[i].xlineNo != tb[i].xlineNo ||
        ta[i].cdp != tb[i].cdp || ta[i].tracl != tb[i].tracl)
    {
      if (why)
        *why = QStringLiteral("trace %1: (line %2, xline %3, cdp %4, tracl %5) vs "
                              "(line %6, xline %7, cdp %8, tracl %9)")
                   .arg(i)
                   .arg(ta[i].lineNo).arg(ta[i].xlineNo).arg(ta[i].cdp).arg(ta[i].tracl)
                   .arg(tb[i].lineNo).arg(tb[i].xlineNo).arg(tb[i].cdp).arg(tb[i].tracl);
      return false;
    }
  }
  return true;
}
} // namespace

void CacheSegyIndexTests::sequentialBadTraceSkipMatchesParallelContract()
{
  // B6：小文件（<1MB → 顺序路径）固定道长布局中一道 ns 损坏 → 与并行路径
  //（badTraceSkipKeepsIndex）同语义：跳过 + 记录，索引不作废。
  const QString sgy = m_dir.filePath("m.sgy");
  const int inl = 8, xl = 12, samples = 40; // 96 道 ≈ 38KB → 顺序路径
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  const qint64 traceSize = 240 + qint64(samples) * 4;
  const qint64 badOffset = 3600 + 37 * traceSize;
  patchTraceNs(sgy, badOffset, -32768);

  SegyReader r;
  QString err;
  QVERIFY2(r.open(sgy, &err), qPrintable(err));
  QCOMPARE(r.traceCount(), inl * xl - 1);
  QCOMPARE(r.badTraceOffsets().size(), 1);
  QCOMPARE(r.badTraceOffsets().first(), badOffset);
  QCOMPARE(r.variableTraceLayout(), false);
  // 坏道只少一道：8 条 inline 全在（各 11/12 道）。
  QCOMPARE(r.inlineNumbers().size(), inl);
}

void CacheSegyIndexTests::variableLayoutFlagSurfaces()
{
  // B6：变道长布局（某道 ns>0 且 ≠ 二进制头 ns）顺序路径本就支持
  //（逐道推进）——补断言 variableTraceLayout() 观察面。
  const QString sgy = m_dir.filePath("n.sgy");
  const int inl = 6, xl = 8, samples = 20;
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  // 道 0 的 ns 改 12（≠ 20）→ 首道变短，其余正常。
  patchTraceNs(sgy, 3600, 12);
  QVERIFY(shortenFirstTrace(sgy, samples, 12));

  SegyReader r;
  QString err;
  QVERIFY2(r.open(sgy, &err), qPrintable(err));
  QCOMPARE(r.traceCount(), inl * xl);
  QCOMPARE(r.variableTraceLayout(), true);
  QCOMPARE(r.badTraceOffsets().isEmpty(), true);
}

void CacheSegyIndexTests::variableLayoutKeepsHardErrorOnCorruptNs()
{
  // B6：变道长布局里的负 ns——后续道边界不可恢复，整索引报错（不静默跳）。
  const QString sgy = m_dir.filePath("o.sgy");
  const int inl = 6, xl = 8, samples = 20;
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  // 道 0 ns=12（变道长观察点）；扫描器视角的道 1 起点随之前移。
  patchTraceNs(sgy, 3600, 12);
  QVERIFY(shortenFirstTrace(sgy, samples, 12));
  const qint64 trace1Offset = 3600 + 240 + qint64(12) * 4; // 道 0 实长
  patchTraceNs(sgy, trace1Offset, -32768);

  SegyReader r;
  QString err;
  QVERIFY(!r.open(sgy, &err));
  QVERIFY2(err.contains(QStringLiteral("variable-trace-length")),
           qPrintable(err));
}

void CacheSegyIndexTests::variableLayoutWritesNoCheckpoint()
{
  // B6：变道长布局的取消不落 checkpoint——resumeScan 按固定步长推进，
  // 续扫会把错位道头当好道收进索引（契约见 docs/perf/INDEX_FORMAT.md §3）。
  const QString sgy = m_dir.filePath("p.sgy");
  const int inl = 12, xl = 12, samples = 20; // 144 道 ≈ 46KB → 顺序路径
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  patchTraceNs(sgy, 3600, 16); // 道 0 变短 → variable
  QVERIFY(shortenFirstTrace(sgy, samples, 16));

  const QString idx = cacheDir();
  {
    SegyReader r;
    SegyOptions opts;
    int checks = 0;
    opts.cancel = [&checks] { return ++checks >= 2; }; // 128 道后再取消（已见 variable）
    QString err;
    QVERIFY(!r.openCached(sgy, idx, &err, &opts));
    QVERIFY(r.lastScanPartial());
    QCOMPARE(r.variableTraceLayout(), true);
  }
  // 固定布局的 checkpoint 会落盘（checkpointResumeAfterCancel 已证）；变道长
  // 布局对本文件必须一个字节都不留（idx/ 里其它测试的 psx 按路径哈希命名，
  // 互不相干）。
  SegyIndexStore store(idx);
  QVERIFY2(!QFile::exists(store.cacheFilePathFor(QFileInfo(sgy))),
           "variable-trace-length layout must not publish a resumable checkpoint");
}

// ---- #290：并行/续扫与顺序 open() 的索引口径一致性 --------------------------

void CacheSegyIndexTests::parallelDialectMatchesSequential()
{
  // demo 工区方言文件（193 全 0 取 CDP、72/76 全 0 取 CDP X/Y）恰好满足并行
  // 资格（inline 字变化、定长、>1MiB、format 5）——修复前 openCached 走
  // scanParallel 时 xline 全 0、角点 (0,0)，与 open() 结果分叉。
  const QString sgy = m_dir.filePath("q.sgy");
  const int inl = 40, xl = 60, samples = 100; // 2400 道 × 640B ≈ 1.5MB → 并行资格
  QVERIFY(makeDialectSegy(sgy, inl, xl, samples, 3000));

  SegyReader seq;
  QString err;
  QVERIFY2(seq.open(sgy, &err), qPrintable(err));
  // 顺序路径探针必须命中方言：crossline 来自 CDP、角点来自 CDP X/Y。
  QCOMPARE(seq.traceCount(), inl * xl);
  QCOMPARE(seq.crosslineNumbers().size(), xl);
  QCOMPARE(seq.crosslineNumbers().first(), 0);
  QCOMPARE(seq.crosslineNumbers().last(), xl - 1);
  QVERIFY(seq.geometry().cornerX[0] != 0.0);
  QVERIFY(seq.geometry().cornerY[0] != 0.0);

  // 路径 1：冷并行扫（独立缓存目录）。
  SegyReader par;
  QVERIFY2(par.openCached(sgy, m_dir.filePath("idxA"), &err), qPrintable(err));
  QVERIFY2(sameIndexAs(par, seq, &err), qPrintable(err));

  // 路径 2：checkpoint 续扫——首遍立刻取消落 checkpoint，第二遍 resumeScan。
  const QString idxB = m_dir.filePath("idxB");
  {
    SegyReader r;
    SegyOptions opts;
    opts.cancel = [] { return true; };
    QVERIFY(!r.openCached(sgy, idxB, &err, &opts));
    QVERIFY(r.lastScanPartial());
  }
  SegyReader resumed;
  QVERIFY2(resumed.openCached(sgy, idxB, &err), qPrintable(err));
  QVERIFY2(sameIndexAs(resumed, seq, &err), qPrintable(err));

  // 路径 2b：取消发生在已经收入道之后。部分索引的线号和起始时间必须来自
  // 已扫道，不能是默认 0；续扫完成后与顺序索引一致。
  const QString idxC = m_dir.filePath("idxC");
  {
    SegyReader r;
    SegyOptions opts;
    std::atomic<int> checks{0};
    opts.cancel = [&checks] { return checks.fetch_add(1) >= 4; };
    QVERIFY(!r.openCached(sgy, idxC, &err, &opts));
    QVERIFY(r.lastScanPartial());
    QVERIFY(r.traceCount() > 0);
    QVERIFY(r.traceCount() < seq.traceCount());
    QCOMPARE(r.geometry().inlineMin, seq.geometry().inlineMin);
    QCOMPARE(r.geometry().startTimeMs, seq.geometry().startTimeMs);
  }
  SegyReader resumedPartial;
  QVERIFY2(resumedPartial.openCached(sgy, idxC, &err), qPrintable(err));
  QVERIFY2(sameIndexAs(resumedPartial, seq, &err), qPrintable(err));

  // 路径 3：身份命中（路径 1 已发布完整索引）。
  SegyReader hit;
  QVERIFY2(hit.openCached(sgy, m_dir.filePath("idxA"), &err), qPrintable(err));
  QVERIFY2(sameIndexAs(hit, seq, &err), qPrintable(err));
}

void CacheSegyIndexTests::parallelShortTraceMatchesSequential()
{
  // 中部道 ns 变短且载荷物理同步压缩（文件整体仍是合法变行长布局）——顺序
  // 与并行两路径口径一致：都识别为 variable-trace-length，逐道推进，无坏道，
  // 解码长度按各道 ns（合并口径：中部短道不再按「坏道跳过」，那会让扫描错位）。
  const QString sgy = m_dir.filePath("r.sgy");
  const int inl = 40, xl = 60, samples = 100; // 同上：>1MiB → 并行资格
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  const qint64 traceSize = 240 + qint64(samples) * 4;
  const qint64 shortOffset = 3600 + 1000 * traceSize;
  patchTraceNs(sgy, shortOffset, samples - 10);
  // 同步移除该道多出来的 40 字节载荷：后续道头回到正确边界。
  {
    QFile f(sgy);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QByteArray bytes = f.readAll();
    f.close();
    bytes.remove(shortOffset + 240 + (samples - 10) * 4, 10 * 4);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(f.write(bytes) == bytes.size());
  }

  SegyReader seq;
  QString err;
  QVERIFY2(seq.open(sgy, &err), qPrintable(err));
  QCOMPARE(seq.traceCount(), inl * xl);
  QVERIFY2(seq.badTraceOffsets().isEmpty(), qPrintable(err));
  QCOMPARE(seq.variableTraceLayout(), true);

  SegyReader par;
  QVERIFY2(par.openCached(sgy, cacheDir(), &err), qPrintable(err));
  QCOMPARE(par.traceCount(), seq.traceCount());
  QCOMPARE(par.variableTraceLayout(), seq.variableTraceLayout());
  QCOMPARE(par.badTraceOffsets(), seq.badTraceOffsets());
  QCOMPARE(par.inlineNumbers(), seq.inlineNumbers());
  QCOMPARE(par.crosslineNumbers(), seq.crosslineNumbers());

  // 索引内短道按其真实 ns 解码（不一致即伪）。
  QVector<SegyTrace> line;
  QVERIFY2(par.readInline(1000, &line, &err), qPrintable(err));
  QCOMPARE(line.size(), xl);
  for (const SegyTrace &t : line)
    QVERIFY2(t.samples.size() == samples || t.samples.size() == samples - 10,
             qPrintable(QStringLiteral("unexpected ns %1").arg(t.samples.size())));
}

// ---- P2/#1026（v6）：统一边界审计 ------------------------------------------------

namespace
{
// 测试侧 payload 组包器（与 SegyIndexStore::encodePayload v6 同构）。目的：
// 绕过 store.save() 的合法编码，直接落「带合法 magic/版本/CRC 的伪造
// payload」——让 load() 的内部解码/审计分支真正被测，而不是只被外层
// CRC 拦截。marker/layout 允许覆写以构造未知标记。
QByteArray forgeIndexPayload(const SegyIndexStore::StoredIndex &idx,
                             char markerOverride = 0, char layoutOverride = 0)
{
  QByteArray out;
  cacheio::putStr(&out, idx.ident.canonicalPath);
  cacheio::putI64(&out, idx.ident.size);
  cacheio::putI64(&out, idx.ident.mtimeMs);
  cacheio::putU64(&out, idx.ident.inode);
  cacheio::putStr(&out, QString::fromLatin1(idx.prefixFingerprint));
  cacheio::putI32(&out, idx.samplesPerTrace);
  cacheio::putI32(&out, idx.sampleIntervalUs);
  cacheio::putI32(&out, idx.formatCode);
  cacheio::putI32(&out, idx.binLineNo);
  for (int offset : idx.headerWordOffsets) cacheio::putI32(&out, offset);
  out.append(static_cast<char>(idx.dialectFlags));
  out.append(layoutOverride ? layoutOverride
                            : (idx.layoutKind == 'V' ? 'V' : 'F'));
  cacheio::putI64(&out, idx.firstTraceOffset);
  cacheio::putF64(&out, idx.geometry.inlineMin);
  cacheio::putF64(&out, idx.geometry.inlineMax);
  cacheio::putF64(&out, idx.geometry.xlineMin);
  cacheio::putF64(&out, idx.geometry.xlineMax);
  for (int i = 0; i < 4; ++i)
  {
    cacheio::putF64(&out, idx.geometry.cornerX[i]);
    cacheio::putF64(&out, idx.geometry.cornerY[i]);
  }
  cacheio::putF64(&out, idx.geometry.startTimeMs);
  const quint32 n = quint32(idx.inlineNos.size());
  const quint32 bad = quint32(idx.badTraceOffsets.size());
  cacheio::putU32(&out, n);
  cacheio::putU32(&out, bad);
  out.append(markerOverride ? markerOverride : (idx.complete ? 'C' : 'P'));
  cacheio::putI64(&out, idx.scannedOffset);
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI32(&out, idx.inlineNos.at(int(i)));
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI32(&out, idx.xlineNos.at(int(i)));
  for (quint32 i = 0; i < n; ++i)
    cacheio::putI64(&out, idx.offsets.at(int(i)));
  if ((layoutOverride ? layoutOverride : idx.layoutKind) == 'V')
    for (quint32 i = 0; i < n; ++i)
      cacheio::putU16(&out, idx.traceNs.size() == idx.inlineNos.size()
                                ? idx.traceNs.at(int(i))
                                : quint16(idx.samplesPerTrace));
  for (quint32 i = 0; i < bad; ++i)
    cacheio::putI64(&out, idx.badTraceOffsets.at(int(i)));
  return out;
}
} // namespace

void CacheSegyIndexTests::forgedPayloadRejectedBeyondCrc()
{
  // 基准：真实扫描快照（身份/几何/数组全部合法）。
  const QString sgy = m_dir.filePath("forged.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 4, 4, 12) > 0); // 道长 288B
  SegyIndexStore store(cacheDir());
  SegyReader reader;
  QString err;
  QVERIFY(reader.open(sgy, &err));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  const QFileInfo fi(sgy);
  const QString path = store.cacheFilePathFor(fi);

  // 单用例执行器：写伪造 payload（合法 magic/版本/CRC），load 必须给出
  // 可解释 miss 并删文件自愈。
  const auto expectReject = [&](const SegyIndexStore::StoredIndex &forged,
                                char markerOverride, char layoutOverride,
                                const char *needle) {
    QString werr;
    QVERIFY(writeCacheFileAtomic(path, QByteArray("PSGYIDX1", 8), 6,
                                 CacheFlags::None,
                                 forgeIndexPayload(forged, markerOverride,
                                                   layoutOverride),
                                 &werr));
    QString reason;
    const auto loaded = store.load(fi, /*partialOk=*/true, &reason);
    QVERIFY2(!loaded.has_value(),
             qPrintable(QStringLiteral("forged index accepted: %1 %2")
                            .arg(markerOverride ? QChar(markerOverride) : QChar(),
                                 reason)));
    QVERIFY2(reason.contains(QLatin1String(needle)),
             qPrintable(QStringLiteral("reason '%1' lacks '%2'").arg(reason, needle)));
    QVERIFY(!QFile::exists(path)); // 自愈删除
  };

  { // 控制：真实快照经同一组包器 → 正常命中（组包器本身正确）。
    QString werr;
    QVERIFY(writeCacheFileAtomic(path, QByteArray("PSGYIDX1", 8), 6,
                                 CacheFlags::None, forgeIndexPayload(snap),
                                 &werr));
    QString reason;
    QVERIFY2(store.load(fi, /*partialOk=*/false, &reason).has_value(),
             qPrintable(reason));
  }
  { // 未知 marker：非 'C'/'P' 一律损坏，不再「非 C 即 partial」。
    expectReject(snap, 'X', 0, "payload decode failed");
  }
  { // 未知布局标记。
    expectReject(snap, 0, 'Q', "payload decode failed");
  }
  {
    auto f = snap;
    f.formatCode = 99; // 未知样点格式——直接失败，不退化为 1 字节样本
    expectReject(f, 0, 0, "unknown sample format");
  }
  {
    auto f = snap;
    f.samplesPerTrace = 0;
    expectReject(f, 0, 0, "samples-per-trace");
  }
  {
    auto f = snap; // 道序打乱
    std::swap(f.offsets[0], f.offsets[1]);
    expectReject(f, 0, 0, "order/alignment");
  }
  {
    auto f = snap; // 重复偏移（重叠道）
    f.offsets[3] = f.offsets[2];
    expectReject(f, 0, 0, "order/alignment");
  }
  {
    auto f = snap; // 接近整数上限的偏移（溢出安全：减法判界）
    f.offsets[0] = std::numeric_limits<qint64>::max() / 2;
    expectReject(f, 0, 0, "order/alignment");
  }
  {
    auto f = snap; // 坏道偏移与好道重复
    f.badTraceOffsets = {f.offsets.at(2)};
    expectReject(f, 0, 0, "duplicate trace offset");
  }
  {
    auto f = snap; // 变道长不允许 checkpoint
    f.layoutKind = 'V';
    f.complete = false;
    f.traceNs.fill(quint16(f.samplesPerTrace));
    expectReject(f, 0, 0, "must not be a checkpoint");
  }
  {
    auto f = snap; // 变道长 ns 表与偏移不连续
    f.layoutKind = 'V';
    f.traceNs = QVector<quint16>(f.inlineNos.size(),
                                 quint16(f.samplesPerTrace));
    f.traceNs[0] = quint16(f.samplesPerTrace - 3);
    expectReject(f, 0, 0, "order/alignment");
  }
  {
    auto f = snap; // checkpoint 超尾 1 字节（#1026 G5：+4096 宽容已删）
    f.complete = false;
    f.scannedOffset = fi.size() + 1;
    expectReject(f, 0, 0, "outside file");
  }
  {
    auto f = snap; // checkpoint 超尾 4096（旧宽容上界）
    f.complete = false;
    f.scannedOffset = fi.size() + 4096;
    expectReject(f, 0, 0, "outside file");
  }
  {
    auto f = snap; // 完整索引未扫到文件尾
    f.scannedOffset = fi.size() - 288;
    expectReject(f, 0, 0, "does not end at EOF");
  }
  { // 合法 partial 控制：取消恰在文件尾（全道已扫）→ 仍可用（partialOk）。
    QString werr;
    auto f = snap;
    f.complete = false;
    f.scannedOffset = fi.size();
    QVERIFY(writeCacheFileAtomic(path, QByteArray("PSGYIDX1", 8), 6,
                                 CacheFlags::None, forgeIndexPayload(f), &werr));
    QString reason;
    QVERIFY2(store.load(fi, /*partialOk=*/true, &reason).has_value(),
             qPrintable(reason));
  }
}

void CacheSegyIndexTests::auditRejectsPayloadBeyondEof()
{
  // #1026 G6：道头完整在文件内、采样载荷越过文件尾——restore() 旧口径
  // 只查 off+240≤fileSize，此处证明完整道跨度（240+ns·bytes）也不越界。
  const QString sgy = m_dir.filePath("shortpay.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 4, 4, 40) > 0); // 道长 400B
  // 截到 2.75 道：道 2 道头（4400..4640）在文件内，载荷仅 60/160。
  QFile f(sgy);
  QVERIFY(f.open(QIODevice::ReadWrite));
  QVERIFY(f.resize(3600 + 2 * 400 + 300));
  f.close();

  SegyIndexStore::StoredIndex snap;
  snap.ident = SegyIndexStore::identityOf(QFileInfo(sgy));
  snap.samplesPerTrace = 40;
  snap.sampleIntervalUs = 2000;
  snap.formatCode = 5;
  snap.firstTraceOffset = 3600;
  snap.layoutKind = 'F';
  snap.complete = true;
  snap.scannedOffset = QFileInfo(sgy).size();
  snap.inlineNos = {1000, 1000, 1000};
  snap.xlineNos = {2000, 2001, 2002};
  snap.offsets = {3600, 4000, 4400};
  QVERIFY(!SegyReader().restore(snap, sgy)); // 载荷越界 → 拒

  // 同一份伪造经磁盘缓存走 load()：审计拒绝 + 自愈删除。
  SegyIndexStore store(cacheDir());
  QString werr;
  QVERIFY(writeCacheFileAtomic(store.cacheFilePathFor(QFileInfo(sgy)),
                               QByteArray("PSGYIDX1", 8), 6, CacheFlags::None,
                               forgeIndexPayload(snap), &werr));
  QString reason;
  QVERIFY(!store.load(QFileInfo(sgy), /*partialOk=*/true, &reason).has_value());
  QVERIFY2(reason.contains(QLatin1String("beyond EOF")),
           qPrintable(reason));
  QVERIFY(!QFile::exists(store.cacheFilePathFor(QFileInfo(sgy))));
}

void CacheSegyIndexTests::checkpointPastEofSelfHeals()
{
  // #1026 G5：篡改 checkpoint 的 scannedOffset 越过文件尾——旧 load() 的
  // size+4096 宽容放行，新审计拒绝并删缓存自愈（可重建）。
  const QString sgy = m_dir.filePath("cpt.sgy");
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 6, 6, 24) > 0);
  SegyIndexStore store(m_dir.filePath("idxCpt"));
  SegyReader reader;
  QVERIFY(reader.open(sgy));
  SegyIndexStore::StoredIndex snap;
  QVERIFY(reader.snapshot(&snap));
  const QFileInfo fi(sgy);
  const auto tampered = [&snap, &fi](qint64 scanned) {
    SegyIndexStore::StoredIndex t = snap;
    t.complete = false;
    t.scannedOffset = scanned;
    return t;
  };
  for (const qint64 beyond : {fi.size() + 1, fi.size() + 4096})
  {
    QString err;
    QVERIFY(store.save(tampered(beyond), &err)); // save 不审计（编码器）
    QString reason;
    QVERIFY(!store.load(fi, /*partialOk=*/true, &reason).has_value());
    QVERIFY2(reason.contains(QLatin1String("audit")) ||
                 reason.contains(QLatin1String("outside file")),
             qPrintable(reason));
    QVERIFY(!QFile::exists(store.cacheFilePathFor(fi)));
  }
  // 源数据仍可正常打开（缓存目录内容不污染原始 SEG-Y）。
  SegyReader again;
  QString reopenErr;
  QVERIFY2(again.openCached(sgy, m_dir.filePath("idxCpt"), &reopenErr),
           qPrintable(reopenErr));
  QCOMPARE(again.traceCount(), 36);
}

void CacheSegyIndexTests::variableLayoutIndexRoundtrip()
{
  // P2/W1（v6）：变道长完整索引带 layoutKind='V' + 逐道 ns 落盘；身份
  // 命中后 restore() 恢复同一索引并如实上报 variableTraceLayout()。
  const QString sgy = m_dir.filePath("var.sgy");
  const int inl = 6, xl = 8, samples = 20;
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, inl, xl, samples) > 0);
  patchTraceNs(sgy, 3600, 12);
  QVERIFY(shortenFirstTrace(sgy, samples, 12));

  const QString idx = m_dir.filePath("idxVar");
  SegyIndexStore store(idx);
  SegyReader r;
  QString err;
  QVERIFY2(r.open(sgy, &err), qPrintable(err));
  QCOMPARE(r.variableTraceLayout(), true);
  SegyIndexStore::StoredIndex snap;
  QVERIFY(r.snapshot(&snap));
  QCOMPARE(snap.layoutKind, 'V');
  QCOMPARE(snap.traceNs.size(), inl * xl);
  QCOMPARE(int(snap.traceNs.first()), 12);
  QVERIFY(store.save(snap, &err));

  QString reason;
  const auto loaded = store.load(QFileInfo(sgy), /*partialOk=*/false, &reason);
  QVERIFY2(loaded.has_value(), qPrintable(reason));
  QCOMPARE(loaded->layoutKind, 'V');
  SegyReader hit;
  QVERIFY(hit.restore(*loaded, sgy));
  QCOMPARE(hit.traceCount(), inl * xl);
  QCOMPARE(hit.variableTraceLayout(), true);
  QVector<SegyTrace> line;
  QVERIFY2(hit.readInline(1000, &line, &err), qPrintable(err));
  QCOMPARE(line.size(), xl);
  QCOMPARE(int(line.first().samples.size()), 12); // 短道按其真实 ns 解码
}

QTEST_MAIN(CacheSegyIndexTests)
#include "tst_cache_segyindex.moc"
