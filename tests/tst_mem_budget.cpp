// 层：测试壳
// goal/perf-systematize 簇5：真工区内存预算门（966MiB 体）。
//   门1 RSS 上限：打开 + inline/time/crossline 三切片后 VmRSS ≤
//      kMaxFileSizeMultiple × 体字节（结构性上限——「整读进内存×2」类回归
//      必红；直读后端正常应远低于此）。绝对值同步 qInfo 供 BASELINE 誊录。
//   门2 泄漏嗅探：开→切片→释放 ×kCycles 轮，末轮相对首轮 RSS 增长 ≤
//      max(kGrowthFloorBytes, 体字节 × kGrowthShare)（每轮增量小且收敛）。
// env 门控同 tst_seismic_realarea（PALEO_SEISMIC_REAL_SGY /
// PALEO_REAL_PROJECT_AREA 未设整套 QSKIP——CI 无本地数据不红）。
// 采样纪律：malloc_trim(0) 后读 /proc/self/status VmRSS——把分配器保留的
// 空闲堆还给 OS 再量（真实泄漏是可达内存，trim 不动它——降噪不掩漏）。
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <filesystem>
#if defined(__linux__)
#include <malloc.h>
#endif

#include "Engine/Sdk.h"

using namespace seismic; // engine::/sdk:: 均为其嵌套命名空间（同 tst_seismic_realarea）

namespace {
QString resolveRealSgy()
{
  const QString direct = qEnvironmentVariable("PALEO_SEISMIC_REAL_SGY");
  if (!direct.isEmpty())
    return direct;
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    return QString();
  return area + QStringLiteral("/地震体/200P_seismic.sgy");
}

// /proc/self/status VmRSS（字节）；读不到 -1（非 Linux 测试面 QSKIP）。
qint64 currentRssBytes()
{
  QFile f(QStringLiteral("/proc/self/status"));
  if (!f.open(QIODevice::ReadOnly))
    return -1;
  for (const QByteArray &line : f.readAll().split('\n'))
    if (line.startsWith("VmRSS:"))
    {
      // "VmRSS:\t 123456 kB"
      const QByteArray kb = line.mid(6).trimmed().split(' ').value(0);
      bool ok = false;
      const qint64 v = kb.toLongLong(&ok);
      return ok ? v * 1024 : qint64(-1);
    }
  return -1;
}

qint64 sampleRssAfterTrim()
{
#if defined(__linux__)
  malloc_trim(0);
#endif
  return currentRssBytes();
}

// 门参数（2026-10-01 本机实测钉死，改动须附实测依据——docs/perf/BASELINE.md §7）：
// 直读后端 966MiB 体 + 3 切片后 RSS 实测 151MiB（0.16×体）——直读不整载。
// 上限取 0.5×体字节（≈483MiB 门）：实测留 3.1× 噪声裕度，「整读进内存」
// （≥1.0×）与「重复缓存整份」（≥2×）类回归必红。
constexpr double kMaxFileSizeMultiple = 0.5;
// 泄漏增长率：6 轮实测净增 16KiB（近零）。门 = max(32MiB, 0.5%×体)：
// 实测留 2000× 噪声裕度，仍能抓住「每轮漏一份切片/索引」级（≥5MiB/轮）回归。
constexpr double kGrowthShare = 0.005;
constexpr qint64 kGrowthFloorBytes = 32LL * 1024 * 1024;
constexpr int kCycles = 6;
} // namespace

class TestMemBudget : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    realSgy_ = resolveRealSgy();
    if (realSgy_.isEmpty() || !QFile::exists(realSgy_))
      QSKIP("PALEO_SEISMIC_REAL_SGY / PALEO_REAL_PROJECT_AREA not set — memory budget skipped");
    fileBytes_ = QFileInfo(realSgy_).size();
    QVERIFY2(fileBytes_ > 500LL * 1024 * 1024,
             "夹具非大体（<500MB）——本门针对 966MB 级真工区体");
    QVERIFY2(sampleRssBytes() > 0, "/proc/self/status VmRSS 不可读（非 Linux？）");
  }

  // 门1：开 + 3 切片后的结构性 RSS 上限（+绝对值誊录）。
  void rssCeilingAfterOpenAndThreeSlices()
  {
    namespace sdk = seismic::sdk;
    engine::Status st;
    auto ds = sdk::Dataset::Open(std::filesystem::path(realSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    QVERIFY2(ds != nullptr && st.ok(), st.message.c_str());
    const engine::DatasetMetadata &meta = ds->Metadata();
    const int midIl = meta.inlineAxis.ValueAt(meta.inlineAxis.count / 2);
    const int midXl = meta.xlineAxis.ValueAt(meta.xlineAxis.count / 2);
    const int midSample = meta.sampleCount / 2;

    engine::Slice2D slice;
    QVERIFY2(ds->ReadInline(midIl, slice).ok(), "inline slice");
    QVERIFY2(ds->ReadTimeSlice(midSample, slice).ok(), "time slice");
    QVERIFY2(ds->ReadCrossline(midXl, slice).ok(), "crossline slice");

    const qint64 rss = sampleRssBytes();
    qInfo("BASELINE mem_rss_after_open_3slices_bytes = %lld (%.0f MiB, file=%.0f MiB, ratio=%.2f)",
          static_cast<long long>(rss), rss / 1048576.0, fileBytes_ / 1048576.0,
          double(rss) / double(fileBytes_));
    const qint64 ceiling = qint64(kMaxFileSizeMultiple * double(fileBytes_));
    QVERIFY2(rss <= ceiling,
             qPrintable(QStringLiteral("RSS %1 MiB > 门 %2 MiB（%3×体大小）——"
                                       "疑似整读/重复缓存回归")
                            .arg(rss / 1048576)
                            .arg(ceiling / 1048576)
                            .arg(kMaxFileSizeMultiple, 0, 'f', 1)));
  }

  // 门2：开→切片→释放循环的 RSS 增长率（泄漏嗅探）。
  void noRssGrowthAcrossReopenCycles()
  {
    namespace sdk = seismic::sdk;
    engine::Status st;
    const std::filesystem::path path(realSgy_.toStdString());
    qint64 rss1 = -1, rssN = -1;
    for (int round = 0; round < kCycles; ++round)
    {
      {
        auto ds = sdk::Dataset::Open(path, sdk::OpenOptions{}, st);
        QVERIFY2(ds != nullptr && st.ok(), st.message.c_str());
        engine::Slice2D slice;
        const engine::DatasetMetadata &meta = ds->Metadata();
        QVERIFY2(ds->ReadInline(meta.inlineAxis.ValueAt(meta.inlineAxis.count / 2), slice).ok(),
                 "cycle inline");
        QVERIFY2(ds->ReadTimeSlice(meta.sampleCount / 2, slice).ok(), "cycle time slice");
        ds.reset(); // 释放本轮数据集（集中式进程内缓存放给门判——增长门只认净增）
      }
      const qint64 rss = sampleRssBytes();
      qInfo("BASELINE mem_rss_cycle_%lld_bytes = %lld",
            static_cast<long long>(round + 1), static_cast<long long>(rss));
      if (round == 0)
        rss1 = rss;
      rssN = rss;
    }
    const qint64 growth = rssN - rss1;
    const qint64 gate = qMax<qint64>(kGrowthFloorBytes,
                                     qint64(kGrowthShare * double(fileBytes_)));
    qInfo("BASELINE mem_rss_cycle_growth_bytes = %lld (gate=%lld)",
          static_cast<long long>(growth), static_cast<long long>(gate));
    QVERIFY2(growth <= gate,
             qPrintable(QStringLiteral("循环 %1 轮 RSS 净增 %2 MiB > 门 %3 MiB——"
                                       "疑似泄漏（每轮 open/close 未回收）")
                            .arg(kCycles)
                            .arg(growth / 1048576)
                            .arg(gate / 1048576)));
  }

private:
  QString realSgy_;
  qint64 fileBytes_ = 0;

  qint64 sampleRssBytes() const { return sampleRssAfterTrim(); }
};

QTEST_MAIN(TestMemBudget)
#include "tst_mem_budget.moc"
