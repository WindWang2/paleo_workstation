// 层：测试壳
// tst_dataperf_index — goal/data-perf：SEG-Y 索引缓存发布修复的回归面。
//
// 被修的 bug：扫描 checkpoint（一个纯可恢复性优化）发布失败时，旧实现把它当
// 致命扫描错误 → 整卷扫描作废、索引不产出 → 缓存永不落盘 → 每次重启全量重扫。
//
// 这里用「把 checkpoint 目标预置成目录」这一平台无关的手法强制 rename 失败
//（POSIX 是 EISDIR/ENOTEMPTY，Windows 是 ERROR_ACCESS_DENIED），断言：
//   · 扫描照常完成、索引完整；
//   · 缓存照常发布（这才治了「每次重启全量重扫」）；
//   · 失败原因不再被吞掉（outcome.checkpointNote 非空且点名 rename）；
//   · 二次加载走缓存命中，且显著快于冷扫（比率门，不写死绝对毫秒）。
#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <filesystem>

#include "../src/io/perffixtures.h"
#include "domain/seismic/sgyindexcache.h"
#include "Data/Sgy/SgyIndexService.h"

namespace
{
  seismic::SgyIndexBuildRequest baseRequest(const std::filesystem::path &sgy,
                                            const std::filesystem::path &checkpoint)
  {
    seismic::SgyIndexBuildRequest request;
    request.path = sgy;
    request.useCache = true;
    request.saveCache = true;
    request.verbose = false;
    request.checkpointPath = checkpoint;
    // 1 字节节奏 = 每个窗口都落一次 checkpoint，小样本也能打到发布路径。
    request.checkpointIntervalBytes = 1;
    return request;
  }
} // namespace

class DataPerfIndexTests : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(m_dir.isValid());
    const QString cacheDir = m_dir.filePath(QStringLiteral("index-cache"));
    QVERIFY(QDir().mkpath(cacheDir));
    qputenv("SEISMIC_INDEX_CACHE_DIR", QFile::encodeName(cacheDir));
    QCOMPARE(QString::fromStdU16String(seismic::SgyIndexCache::CacheDirectory().u16string()),
             cacheDir);

    m_sgy = m_dir.filePath(QStringLiteral("dataperf_grid.sgy"));
    // 40 inline × 40 crossline = 1600 道，每道 64 采样（IEEE fp32）。
    QVERIFY2(PerfFixtures::makeSyntheticSegy(m_sgy, 40, 40, 64) == 1600,
             "synthetic SEG-Y fixture failed");
    m_sgyStd = std::filesystem::path(m_sgy.toStdString());
  }

  void cleanupTestCase()
  {
    std::string err;
    seismic::SgyIndexCache::Remove(m_sgyStd, err);
  }

  // 主回归：checkpoint 发布失败不得作废整卷索引，缓存照常落盘。
  void checkpointPublishFailureDoesNotKillTheScan();

  // 完整扫描发布缓存后，二次加载走命中且显著更快（比率门）。
  void secondLoadHitsTheCacheAndIsMuchFaster();

  // 完整扫描不留自己的 checkpoint（可恢复性状态用毕即弃）。
  void completedScanRemovesItsOwnCheckpoint();

private:
  QTemporaryDir m_dir;
  QString m_sgy;
  std::filesystem::path m_sgyStd;
};

void DataPerfIndexTests::checkpointPublishFailureDoesNotKillTheScan()
{
  std::string removeErr;
  QVERIFY2(seismic::SgyIndexCache::Remove(m_sgyStd, removeErr), qPrintable(QString::fromStdString(removeErr)));

  // 把 checkpoint 目标预置成一个**目录**：tmp 写得进去，rename/replace 到目录
  // 必失败——这就是真工区日志里那一步的可复现形态。
  const std::filesystem::path blocked = seismic::SgyIndexCache::CacheDirectory() /
                                        "scan-checkpoints" / "dataperf_grid.sgy.ckpt";
  std::error_code ec;
  std::filesystem::remove_all(blocked, ec);
  std::filesystem::create_directories(blocked, ec);
  QVERIFY2(std::filesystem::is_directory(blocked, ec), "checkpoint blocker was not created");

  seismic::SgyIndexBuildRequest request = baseRequest(m_sgyStd, blocked);
  const seismic::SgyIndexBuildOutcome outcome = seismic::BuildSgyIndexAuto(request);

  QVERIFY2(outcome.index != nullptr,
           qPrintable(QString::fromStdString(
               "scan was discarded by a checkpoint failure: " + outcome.message)));
  QCOMPARE(outcome.strategy, std::string("sequential"));
  QVERIFY2(outcome.index->complete, "the published index must be complete");
  QCOMPARE(outcome.index->traceCount, 1600);

  // 失败原因必须可见（不再是一句裸的 "rename failed" 被吞掉）。
  QVERIFY2(!outcome.checkpointNote.empty(),
           "a failed checkpoint publish must be reported, not swallowed");
  qInfo("dataperf: checkpoint note = %s", outcome.checkpointNote.c_str());
  QVERIFY2(outcome.checkpointNote.find("rename") != std::string::npos ||
               outcome.checkpointNote.find("MoveFileEx") != std::string::npos,
           qPrintable(QString::fromStdString(outcome.checkpointNote)));

  // 缓存照常落盘——这才是「下次不再全量重扫」的实质。
  QCOMPARE(outcome.cacheNote, std::string("index cache published"));
  const std::filesystem::path cachePath = seismic::SgyIndexCache::CachePathFor(m_sgyStd);
  std::error_code exists;
  QVERIFY2(std::filesystem::exists(cachePath, exists), "the index cache was not published");
  QVERIFY2(std::filesystem::file_size(cachePath, exists) > 0, "the index cache is empty");

  std::filesystem::remove_all(blocked, ec);
}

void DataPerfIndexTests::secondLoadHitsTheCacheAndIsMuchFaster()
{
  std::string removeErr;
  seismic::SgyIndexCache::Remove(m_sgyStd, removeErr);
  const std::filesystem::path cachePath = seismic::SgyIndexCache::CachePathFor(m_sgyStd);
  std::error_code ec;
  QVERIFY2(!std::filesystem::exists(cachePath, ec), "cache must start cold");

  // 冷：整卷扫描 + 落盘。
  seismic::SgyIndexBuildRequest cold = baseRequest(m_sgyStd, std::filesystem::path());
  const seismic::SgyIndexBuildOutcome coldOutcome = seismic::BuildSgyIndexAuto(cold);
  QVERIFY2(coldOutcome.index != nullptr, qPrintable(QString::fromStdString(coldOutcome.message)));
  QCOMPARE(coldOutcome.fromCache, false);
  QVERIFY2(std::filesystem::exists(cachePath, ec), "cold run did not publish a cache");

  // 热：命中缓存（不再全量重扫）。
  seismic::SgyIndexBuildRequest warm = baseRequest(m_sgyStd, std::filesystem::path());
  const seismic::SgyIndexBuildOutcome warmOutcome = seismic::BuildSgyIndexAuto(warm);
  QVERIFY2(warmOutcome.index != nullptr, qPrintable(QString::fromStdString(warmOutcome.message)));
  QCOMPARE(warmOutcome.fromCache, true);
  QCOMPARE(warmOutcome.strategy, std::string("cache"));
  QCOMPARE(warmOutcome.index->traceCount, coldOutcome.index->traceCount);

  qInfo("dataperf: cold=%.1fms warm=%.1fms ratio=%.3f", coldOutcome.wallMs, warmOutcome.wallMs,
        coldOutcome.wallMs > 0 ? warmOutcome.wallMs / coldOutcome.wallMs : -1.0);
  // 比率门：命中路径的耗时要远低于冷扫（真工区口径：< 冷扫 20%）。
  QVERIFY2(coldOutcome.wallMs > 0.0 && warmOutcome.wallMs < 0.5 * coldOutcome.wallMs,
           qPrintable(QStringLiteral("warm load %1ms is not much faster than the cold scan %2ms")
                          .arg(warmOutcome.wallMs)
                          .arg(coldOutcome.wallMs)));
}

void DataPerfIndexTests::completedScanRemovesItsOwnCheckpoint()
{
  std::string removeErr;
  seismic::SgyIndexCache::Remove(m_sgyStd, removeErr);

  const std::filesystem::path checkpoint = seismic::SgyIndexCache::CacheDirectory() /
                                           "scan-checkpoints" / "dataperf_grid.sgy.ckpt";
  std::error_code ec;
  std::filesystem::remove_all(checkpoint, ec);

  seismic::SgyIndexBuildRequest request = baseRequest(m_sgyStd, checkpoint);
  const seismic::SgyIndexBuildOutcome outcome = seismic::BuildSgyIndexAuto(request);
  QVERIFY2(outcome.index != nullptr, qPrintable(QString::fromStdString(outcome.message)));
  QCOMPARE(outcome.strategy, std::string("sequential"));
  // 正常路径：checkpoint 写得出，也就不必降级。
  QVERIFY2(outcome.checkpointNote.empty(),
           qPrintable(QString::fromStdString(outcome.checkpointNote)));

  // 完整扫描成功后自己的 checkpoint 会被清掉（可恢复状态用毕即弃）。
  std::error_code stale;
  QVERIFY2(!std::filesystem::exists(checkpoint, stale),
           "a completed scan must not leave its checkpoint behind");
  std::filesystem::remove_all(checkpoint, ec);
}

QTEST_MAIN(DataPerfIndexTests)
#include "tst_dataperf_index.moc"
