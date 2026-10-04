// 层：测试壳
// goal/horizon-autotrack 性能面：
//  · 比率门（常跑）：合成体上多种子追踪总耗时 ≤ 8× 同体切片提取基线
//    （禁绝对墙钟；同进程同盘，机器无关）。
//  · 966MB 生产形状实测：411 IL × 641 XL × 901 样 @2ms（= 965.9 MiB，与
//    真工区同构）逐道追踪速率（拾取点数/秒），输出 BASELINE 行誊
//    docs/progress/horizon-autotrack.md。夹具 ramp 波形（道间相关恒 1）——
//    度量的是满候选搜索的计算吞吐（每道 FLOPs 与事件数据相同）。
//    夹具一次性生成缓存在构建目录，不入库。
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <vector>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/seismictaskservice.h"

using seismic::SeismicTaskService;

namespace
{

// 合成事件体（标准字位 188/192；峰位随道缓移的 ricker 事件——追踪全程可追）
bool writeEventSegy(const QString &path, int nIl, int nXl, int ns, int dt)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  f.write(QByteArray(3200, ' '));
  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  f.write(binHdr);
  const double dtSec = dt / 1e6;
  const double a2 = std::pow(std::numbers::pi * 35.0 * dtSec, 2);
  for (int i = 0; i < nIl; ++i)
    for (int j = 0; j < nXl; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(i * nXl + j + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
      qToBigEndian<qint32>(1000 + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(2000 + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data()) + 114);
      qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data()) + 116);
      f.write(trHdr);
      QByteArray samples(ns * 4, 0);
      const double peak = ns / 2.0 + (i * 0.5 + j * 0.25);
      for (int k = 0; k < ns; ++k)
      {
        const double t = k - peak;
        const float v = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
        quint32 raw;
        std::memcpy(&raw, &v, 4);
        qToBigEndian<quint32>(raw, reinterpret_cast<uchar *>(samples.data()) + k * 4);
      }
      f.write(samples);
    }
  return true;
}

} // namespace

class TestHorizonTrackPerf : public QObject
{
  Q_OBJECT

  QString bigSgy_;

private slots:
  void initTestCase()
  {
    const QString perfDir = QStringLiteral(PALEO_SEISMIC_PERF_DIR);
    QVERIFY(QDir().mkpath(perfDir));
    // 966MB 生产形状：411×641×901@2ms = 965.9 MiB（与真工区同构）
    bigSgy_ = perfDir + QStringLiteral("/autotrack_big.sgy");
    const qint64 wantBytes =
        3600 + qint64(411) * 641 * (240 + qint64(901) * 4);
    if (QFileInfo(bigSgy_).size() != wantBytes)
    {
      QElapsedTimer gen;
      gen.start();
      const int rc = QProcess::execute(
          QStringLiteral(PALEO_PYTHON3),
          {QStringLiteral(PALEO_SEGY_FIXTURE_TOOL),
           QStringLiteral("--out"), bigSgy_,
           QStringLiteral("--inlines"), QStringLiteral("411"),
           QStringLiteral("--xlines"), QStringLiteral("641"),
           QStringLiteral("--samples"), QStringLiteral("901"),
           QStringLiteral("--dt"), QStringLiteral("2000")});
      QVERIFY2(rc == 0, "make_segy_fixture.py 966MB shape failed");
      qInfo("fixture generated in %.1f s", gen.elapsed() / 1000.0);
    }
    QCOMPARE(QFileInfo(bigSgy_).size(), wantBytes);
    qInfo("perf fixture: %s (%.1f MiB)", qPrintable(bigSgy_),
          QFileInfo(bigSgy_).size() / 1048576.0);
  }

  // ---- 比率门：多种子追踪 ≤ 8× 切片提取基线（防数量级回归）----
  void trackingCostRatioGate()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("perf_ratio.sgy"));
    QVERIFY(writeEventSegy(sgy, 8, 400, 1024, 2000));

    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));

    seismic::SgySliceImage img;
    QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, 1004, img, err));
    QCOMPARE(img.width, 400);

    // 基线：热态取 3 次最小
    double extractMs = 1e9;
    for (int i = 0; i < 3; ++i)
    {
      QElapsedTimer t;
      t.start();
      QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, 1004, img, err));
      extractMs = std::min(extractMs, t.nsecsElapsed() / 1e6);
    }
    QVERIFY(extractMs > 0.0);

    // 三种子追踪（转置 + 核 + 合并在内），热态最小
    double trackMs = 1e9;
    seismic::SeismicTrackReport report;
    QList<seismic::SeismicPick> picks;
    for (int i = 0; i < 3; ++i)
    {
      QElapsedTimer t;
      t.start();
      picks = SeismicTaskService::trackHorizonMultiSeeds(
          img, seismic::SgySliceType::Inline, 1004, 2000, 2399,
          {{50, 512}, {200, 512}, {350, 512}}, {24, 12, 0.6},
          QStringLiteral("perf"), QStringLiteral("H1"), 2.0f, &report);
      trackMs = std::min(trackMs, t.nsecsElapsed() / 1e6);
    }
    QCOMPARE(picks.size(), 400); // 事件全程可追
    qInfo("BASELINE track_ratio = %.2fms vs extract %.2fms -> %.2fx "
          "(%.0f picks/s)",
          trackMs, extractMs, trackMs / extractMs,
          picks.size() / (trackMs / 1000.0));
    QVERIFY2(trackMs <= 8.0 * extractMs,
             qPrintable(QStringLiteral("追踪 %1ms 超门（基线 %2ms×8）")
                            .arg(trackMs)
                            .arg(extractMs)));
  }

  // ---- 966MB 生产形状实测：IL 剖面满宽度追踪速率 ----
  void bigVolumeTrackRate()
  {
    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    QElapsedTimer clock;
    clock.start();
    QVERIFY2(vol->Load(bigSgy_.toStdString(), err),
             qPrintable(QString::fromStdString(err)));
    qInfo("BASELINE autotrack_966mb_open_ms = %.0f", double(clock.elapsed()));
    qInfo("BASELINE autotrack_966mb_geometry = il %d..%d xl %d..%d ns %d dt %dus",
          vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax(),
          vol->SampleCount(), vol->SampleIntervalUs());

    const int midInline = (vol->InlineMin() + vol->InlineMax()) / 2;
    seismic::SgySliceImage img;
    clock.restart();
    QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, midInline, img, err));
    qInfo("BASELINE autotrack_966mb_il_extract_ms = %.1f", double(clock.elapsed()));
    QCOMPARE(img.width, vol->XlineMax() - vol->XlineMin() + 1);

    // 中列种子（ramp 道相关恒 1 → 全程可追；度量满候选搜索吞吐）
    clock.restart();
    seismic::SeismicTrackReport report;
    const QList<seismic::SeismicPick> picks = SeismicTaskService::trackHorizonMultiSeeds(
        img, seismic::SgySliceType::Inline, midInline, vol->XlineMin(),
        vol->XlineMax(), {{img.width / 2, vol->SampleCount() / 2}}, {24, 12, 0.6},
        QStringLiteral("perf"), QStringLiteral("H1"), 2.0f, &report);
    const double trackMs = double(clock.elapsed());
    const double picksPerSec = picks.size() / (trackMs / 1000.0);
    qInfo("BASELINE autotrack_966mb_track_ms = %.1f for %d picks "
          "(covered %d/%d, stop=%s)",
          trackMs, int(picks.size()), int(report.coveredTraces), int(report.totalTraces),
          qPrintable(report.stopSummary));
    qInfo("BASELINE autotrack_966mb_picks_per_sec = %.0f", picksPerSec);
    QVERIFY(picks.size() >= report.totalTraces * 9 / 10); // ramp 全程可追
    QVERIFY(trackMs < 5000.0); // 防数量级回归（正常亚百 ms 级）
  }
};

QTEST_GUILESS_MAIN(TestHorizonTrackPerf)
#include "tst_horizontrackperf.moc"
