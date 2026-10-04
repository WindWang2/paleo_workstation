// 层：测试壳（被测对象为 services 层体传播任务编排）
// goal/horizon-3d 服务面：
//  · 单种子 → 整层位面（线号/TWT 域映射、IL 覆盖、QC 报告）
//  · 取消语义（无半成品发布）/ 顶替语义（前者静默丢弃、后者生效）
//  · 空态如实报因（无体/无种子/线号不在体/内存超限）
//  · 派生资产登记 roundtrip（DERIVED 锚源体版本 + GeoTIFF 上图声明）
//  · 直接成格保留空洞（不 IDW 填洞）
//  · 内存比率门（VmHWM 增量 ≤ 工作集上界倍数，不测绝对值）+
//    计算比率门（传播 ≤ N×(剖面数×切片提取基线)，禁绝对墙钟）
#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "catalog/datacatalog.h"
#include "domain/seismic/sgyvolume.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"

using seismic::SeismicTaskService;
using seismic::SeismicPick;

namespace
{

// 合成倾斜事件体（标准字位 188/192；IL/XL 号从 1000/2000 起——验证线号域
// 映射，非 0 基索引）。峰位 peak(il,xl) = 40 + ilIdx + xlIdx（Ricker 35Hz，
// 波形半宽 ~14 样 ≥ 相关窗；斜率 1+1 保证最大测网角峰 40+191+199=430 <
// 512 采样，波形完整不裁边——全覆盖可期）。
bool writeDippingSegy(const QString &path, int nIl, int nXl, int ns, int dt)
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
      const double peak = 40.0 + i + j;
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
  f.close();
  return true;
}

// VmHWM（KB）——峰值常驻集高水位，单调不减；Linux 专用。
qint64 peakRssKb()
{
  QFile status(QStringLiteral("/proc/self/status"));
  if (!status.open(QIODevice::ReadOnly))
    return -1;
  const QByteArray blob = status.readAll();
  const int at = blob.indexOf("VmHWM:");
  if (at < 0)
    return -1;
  return blob.mid(at + 6).trimmed().split(' ').first().toLongLong();
}

} // namespace

class TestHorizonTrackSvc : public QObject
{
  Q_OBJECT

  QTemporaryDir tempDir_;
  QString smallSgy_;
  QString bigSgy_;   // RSS/比率门夹具（体量 ≫ 滑窗工作集）
  QString slowSgy_;  // 顶替夹具（在途窗口足够宽）
  std::shared_ptr<seismic::SgyVolume> smallVol_;
  std::shared_ptr<seismic::SgyVolume> bigVol_;
  std::shared_ptr<seismic::SgyVolume> slowVol_;

  std::shared_ptr<seismic::SgyVolume> loadVolume(const QString &path)
  {
    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    if (!vol->Load(path.toStdString(), err))
      return nullptr;
    return vol;
  }

  struct RunOutcome
  {
    bool finished = false;
    bool ok = false;
    QList<SeismicPick> picks;
    seismic::SeismicTrackReport report;
    QString error;
    QVector<int> percents;
    PaleoTask *task = nullptr;
  };

  RunOutcome runProp(seismic::SeismicTaskService &svc,
                     std::shared_ptr<const seismic::SgyVolume> volume,
                     const seismic::SeismicTaskService::VolumePropagationRequest &req,
                     int timeoutMs = 120000)
  {
    RunOutcome out;
    PaleoTask *task = svc.startVolumePropagation(
        volume, req,
        [&out](bool ok, const QList<SeismicPick> &picks,
               const seismic::SeismicTrackReport &report, const QString &error) {
          out.finished = true;
          out.ok = ok;
          out.picks = picks;
          out.report = report;
          out.error = error;
        });
    if (!task)
    {
      // 同步拒绝路径：回调已同步发出（finished 由调用方断言）
      out.task = nullptr;
      return out;
    }
    out.task = task;
    QSignalSpy changedSpy(task, &PaleoTask::changed);
    QObject::connect(task, &PaleoTask::changed,
                     [&out, task]() { out.percents.append(task->percent()); });
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    if (!finishedSpy.wait(timeoutMs))
      return out; // 超时：finished=false
    QCoreApplication::processEvents(); // 排队回调投递
    return out;
  }

  static seismic::SeismicTaskService::VolumePropagationRequest seedRequest(
      int seedInline, int seedXline, int seedSample)
  {
    seismic::SeismicTaskService::VolumePropagationRequest req;
    req.seedInline = seedInline;
    req.seeds.append({seedXline, seedSample});
    req.windowSamples = 24;
    req.maxSearchSamples = 12;
    req.correlationThreshold = 0.6;
    req.interpreter = QStringLiteral("svc-test");
    req.horizonName = QStringLiteral("H3D");
    return req;
  }

  // 解析峰：40 + (il-1000) + (xl-2000)
  static int analyticPeak(int il, int xl)
  {
    return 40 + (il - 1000) + (xl - 2000);
  }

private slots:
  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    smallSgy_ = tempDir_.filePath(QStringLiteral("dip_small.sgy"));
    bigSgy_ = tempDir_.filePath(QStringLiteral("dip_big.sgy"));
    slowSgy_ = tempDir_.filePath(QStringLiteral("dip_slow.sgy"));
    QVERIFY(writeDippingSegy(smallSgy_, 8, 24, 256, 2000));
    QVERIFY(writeDippingSegy(bigSgy_, 192, 200, 512, 2000));
    QVERIFY(writeDippingSegy(slowSgy_, 48, 400, 512, 2000));
    smallVol_ = loadVolume(smallSgy_);
    bigVol_ = loadVolume(bigSgy_);
    slowVol_ = loadVolume(slowSgy_);
    QVERIFY(smallVol_ && smallVol_->IsLoaded());
    QVERIFY(bigVol_ && bigVol_->IsLoaded());
    QVERIFY(slowVol_ && slowVol_->IsLoaded());
  }

  // Oracle#1：合成体单种子 → 整层位面（线号/TWT 域映射 + IL 覆盖 + QC）
  void singleSeedFullCoverage()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    auto req = seedRequest(1004, 2012, analyticPeak(1004, 2012));
    req.startTimeMs = 40.0; // 记录延迟入 TWT（#146 域映射）
    const RunOutcome out = runProp(svc, smallVol_, req);
    QVERIFY2(out.finished, "任务未完成");
    QVERIFY2(out.ok, qPrintable(out.error));
    QCOMPARE(int(out.picks.size()), 8 * 24); // 全覆盖
    QCOMPARE(out.report.coveredTraces, 8 * 24);
    QCOMPARE(out.report.totalTraces, 8 * 24);
    QCOMPARE(out.report.ilMin, 1000);
    QCOMPARE(out.report.ilMax, 1007);
    QVERIFY(out.report.meanConfidence >= 0.6f);
    for (const SeismicPick &p : out.picks)
    {
      QVERIFY(p.inlineNo >= 1000 && p.inlineNo <= 1007); // 线号域（非索引）
      QVERIFY(p.xlineNo >= 2000 && p.xlineNo <= 2023);
      const int want = analyticPeak(p.inlineNo, p.xlineNo);
      QVERIFY2(std::abs(p.sampleIndex - want) <= 1,
               qPrintable(QString("il %1 xl %2 picked %3 want %4")
                              .arg(p.inlineNo).arg(p.xlineNo)
                              .arg(p.sampleIndex).arg(want)));
      QCOMPARE(p.twtMs, 40.0 + double(p.sampleIndex) * 2.0);
      QCOMPARE(p.horizonName, QStringLiteral("H3D"));
    }
    QVERIFY(out.percents.size() >= 2); // 进度可见（逐剖面报进度）
  }

  // 空态（Oracle#6）：无体/无种子/线号不在体/内存超限 → 立即如实报因
  void invalidInputsRefusedWithReason()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto req = seedRequest(1004, 2012, analyticPeak(1004, 2012));

    // 无体
    RunOutcome out = runProp(svc, nullptr, req);
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("地震体未加载")));

    // 无种子
    auto noSeed = req;
    noSeed.seeds.clear();
    out = runProp(svc, smallVol_, noSeed);
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("无种子")));

    // 种子剖面线号不在体（无最近线替代——诚实语义）
    out = runProp(svc, smallVol_, seedRequest(999, 2012, 120));
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("不在本体")));

    // 种子 crossline 不在体
    out = runProp(svc, smallVol_, seedRequest(1004, 2099, 120));
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("不在本体")));

    // 种子采样越界
    out = runProp(svc, smallVol_, seedRequest(1004, 2012, 99999));
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("越界")));

    // 内存超限：预算小于滑窗工作集（6×剖面字节）→ 拒绝并报数
    auto tiny = req;
    tiny.workingSetBudgetBytes = 1024;
    out = runProp(svc, smallVol_, tiny);
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("内存超限")));
    QVERIFY(out.picks.isEmpty());

    // 服务未接任务池
    seismic::SeismicTaskService bare;
    out = runProp(bare, smallVol_, req);
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.error.contains(QStringLiteral("PaleoTaskService")));
  }

  // Oracle#3：传播中取消 → 无半成品拾取、无悬挂（任务终态 Cancelled）
  void cancelMidFlightPublishesNothing()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    auto req = seedRequest(1010, 2100, analyticPeak(1010, 2100));

    RunOutcome out;
    PaleoTask *task = svc.startVolumePropagation(
        slowVol_, req,
        [&out](bool ok, const QList<SeismicPick> &picks,
               const seismic::SeismicTrackReport &, const QString &error) {
          out.finished = true;
          out.ok = ok;
          out.picks = picks;
          out.error = error;
        });
    QVERIFY(task);
    QSignalSpy changedSpy(task, &PaleoTask::changed);
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    // 首个进度事件即请求取消（worker 逐剖面协作取消）
    QObject::connect(task, &PaleoTask::changed, [task]() { task->requestCancel(); });
    QVERIFY(finishedSpy.wait(120000));
    QCoreApplication::processEvents();
    QVERIFY(out.finished);
    QVERIFY(!out.ok);
    QVERIFY(out.picks.isEmpty()); // 无半成品
    QVERIFY(out.error.contains(QStringLiteral("取消")));
    QCOMPARE(task->state(), PaleoTask::State::Cancelled);
    QCOMPARE(svc.activeTaskCount(), 0); // 无悬挂
  }

  // Oracle#4：同体顶替——前者静默丢弃（回调不发），后者生效
  void replacementDropsFirstSilently()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    bool firstCalled = false;
    PaleoTask *first = svc.startVolumePropagation(
        slowVol_, seedRequest(1010, 2100, analyticPeak(1010, 2100)),
        [&firstCalled](bool, const QList<SeismicPick> &,
                       const seismic::SeismicTrackReport &, const QString &) {
          firstCalled = true;
        });
    QVERIFY(first);
    // 第二枚种子即刻顶替（前者在途被取消 + 静默）
    RunOutcome second;
    PaleoTask *secondTask = svc.startVolumePropagation(
        slowVol_, seedRequest(1030, 2150, analyticPeak(1030, 2150)),
        [&second](bool ok, const QList<SeismicPick> &picks,
                  const seismic::SeismicTrackReport &report,
                  const QString &error) {
          second.finished = true;
          second.ok = ok;
          second.picks = picks;
          second.report = report;
          second.error = error;
        });
    QVERIFY(secondTask);
    QSignalSpy finishedSpy(secondTask, &PaleoTask::finished);
    QVERIFY(finishedSpy.wait(120000));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents(); // 前者的排队回调（若有）也投递
    QVERIFY2(!firstCalled, "被顶替任务不得发回调（静默丢弃）");
    QCOMPARE(first->state(), PaleoTask::State::Cancelled); // cancelled≠failed
    QVERIFY(second.finished);
    QVERIFY2(second.ok, qPrintable(second.error));
    QCOMPARE(int(second.picks.size()), 48 * 400);
    QCOMPARE(svc.activeTaskCount(), 0);
  }

  // Oracle#1（登记面）：传播结果 → DERIVED 版本（parentVersionIds 锚源体）
  // + GeoTIFF 上图声明（horizon-autotrack 管线复用）
  void derivedVersionAnchorsSourceVolume()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const RunOutcome out = runProp(
        svc, smallVol_, seedRequest(1004, 2012, analyticPeak(1004, 2012)));
    QVERIFY2(out.ok, qPrintable(out.error));

    DataCatalog catalog;
    QString err;
    QVERIFY(catalog.open(tempDir_.path(), &err));
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_hz3d_1");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    seismicAsset.displayName = QStringLiteral("dip_small.sgy");
    QVERIFY(catalog.addAsset(seismicAsset, &err));
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_hz3d_1");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = smallSgy_;
    rawVersion.fileName = QStringLiteral("dip_small.sgy");
    QVERIFY(catalog.addVersion(rawVersion, &err));

    const QString outDir = tempDir_.filePath(QStringLiteral("interpretation"));
    LayerDeclaration decl;
    const QString path = SeismicTaskService::registerPropagatedHorizonAsset(
        &catalog, seismicAsset.id, rawVersion.id, QStringLiteral("H3D"),
        out.picks, outDir, &err, &decl);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(path.endsWith(QStringLiteral("_H3D_horizon.csv")));

    // CSV 行数 = 全覆盖拾取数（规则测网直接落格，无插值行列）
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const int dataLines = int(f.readAll().count('\n')) - 1;
    QCOMPARE(dataLines, int(out.picks.size()));

    // catalog：DERIVED + 父版本锚源体 + origin 标记
    const CatalogVersion derived = catalog.currentVersion(
        QStringLiteral("seis_horizon_seis_hz3d_1_H3D"));
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{rawVersion.id});
    QCOMPARE(derived.extra.value(QStringLiteral("origin")).toString(),
             QStringLiteral("seismic-propagation"));

    // GeoTIFF 上图声明（既有管线）
    QCOMPARE(decl.layerId, QStringLiteral("horizon.H3D"));
    QVERIFY(QFileInfo::exists(decl.source));
    QVERIFY(decl.source.endsWith(QStringLiteral(".tif")));
  }

  // 直接成格保留空洞：缺拾取格点 NaN（不 IDW 填洞——传播覆盖即边界）
  void gridPropagatedKeepsHoles()
  {
    QList<SeismicPick> picks;
    for (int il = 10; il <= 12; ++il)
      for (int xl = 30; xl <= 32; ++xl)
      {
        if (il == 11 && xl == 31)
          continue; // 中心空洞
        SeismicPick p;
        p.inlineNo = il;
        p.xlineNo = xl;
        p.twtMs = 100.0 + il + xl;
        p.confidence = 0.9f;
        picks.append(p);
      }
    const seismic::SeismicHorizonGrid grid =
        SeismicTaskService::gridPropagated(picks);
    QVERIFY(grid.isValid());
    QCOMPARE(grid.inlineCount, 3);
    QCOMPARE(grid.xlineCount, 3);
    QCOMPARE(grid.inlineStep, 1);
    QCOMPARE(grid.xlineStep, 1);
    const auto at = [&](int gi, int gx) {
      return grid.twtMs[std::size_t(gi) * grid.xlineCount + gx];
    };
    QVERIFY(std::isnan(at(1, 1))); // 空洞如实留空
    QCOMPARE(at(0, 0), 140.0);
    QCOMPARE(at(2, 2), 144.0);
  }

  // Oracle#5：内存比率门——体量 ≫ 滑窗工作集的传播，VmHWM 增量有界
  // （192 IL × 200 XL × 512 样 ≈ 75MB 体 vs 6×0.4MB 工作集；比率门不测
  // 绝对值——门 = 工作集的 8 倍，全体驻留 ≈ 192 剖面当量必超门）
  void peakRssBoundedByWorkingSetRatio()
  {
#ifdef Q_OS_LINUX
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    // 预热：索引/单剖面提取的分配先入 HWM（基线含 page cache 之外的常驻）
    seismic::SgySliceImage warm;
    std::string warmErr;
    QVERIFY(bigVol_->ExtractSlice(seismic::SgySliceType::Inline, 1096, warm,
                                  warmErr));
    const qint64 beforeKb = peakRssKb();
    QVERIFY(beforeKb > 0);
    const RunOutcome out = runProp(
        svc, bigVol_, seedRequest(1096, 2100, analyticPeak(1096, 2100)));
    QVERIFY2(out.ok, qPrintable(out.error));
    QCOMPARE(int(out.picks.size()), 192 * 200);
    const qint64 afterKb = peakRssKb();
    QVERIFY(afterKb >= beforeKb);
    const qint64 sectionKb = qint64(200) * 512 * 4 / 1024;
    const qint64 gateKb = 8 * 6 * sectionKb; // 8 ×（6 剖面工作集）
    const qint64 deltaKb = afterKb - beforeKb;
    qInfo("BASELINE hz3d_rss_delta_kb = %lld (gate %lld, volume %lld KB)",
          deltaKb, gateKb, qint64(192) * sectionKb);
    QVERIFY2(deltaKb <= gateKb,
             qPrintable(QString("峰值 RSS 增量 %1 KB 超门 %2 KB（疑似全体驻留）")
                            .arg(deltaKb)
                            .arg(gateKb)));
#else
    QSKIP("VmHWM 比率门为 Linux /proc 专属");
#endif
  }

  // 性能比率门（禁绝对墙钟）：体传播总时 ≤ 8×（剖面数 × 热态切片提取基线）
  void propagationCostRatioGate()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    // 基线：热态单剖面提取最小值
    seismic::SgySliceImage img;
    std::string err;
    QVERIFY(smallVol_->ExtractSlice(seismic::SgySliceType::Inline, 1004, img, err));
    double extractMs = 1e9;
    QElapsedTimer t;
    for (int i = 0; i < 3; ++i)
    {
      t.start();
      QVERIFY(smallVol_->ExtractSlice(seismic::SgySliceType::Inline, 1004, img, err));
      extractMs = std::min(extractMs, t.nsecsElapsed() / 1e6);
    }
    QVERIFY(extractMs > 0.0);
    t.start();
    const RunOutcome out = runProp(
        svc, smallVol_, seedRequest(1004, 2012, analyticPeak(1004, 2012)));
    const double propMs = t.nsecsElapsed() / 1e6;
    QVERIFY2(out.ok, qPrintable(out.error));
    const double gate = 8.0 * extractMs * smallVol_->InlineCount();
    qInfo("BASELINE hz3d_prop_ratio = %.2fms vs %d x %.2fms extract -> %.2fx",
          propMs, smallVol_->InlineCount(), extractMs,
          propMs / (extractMs * smallVol_->InlineCount()));
    QVERIFY2(propMs <= gate,
             qPrintable(QString("体传播 %1ms 超门（基线 %2ms×%3 剖面×8）")
                            .arg(propMs)
                            .arg(extractMs)
                            .arg(smallVol_->InlineCount())));
  }
};

QTEST_MAIN(TestHorizonTrackSvc)
#include "tst_horizontracksvc.moc"
