// 层：数据（测试壳位于 tests/，被测对象为 services 层属性体化扫描编排）
#include <QtTest>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include <gdal.h>

#include "algorithms/seismicattr.h"
#include "catalog/datacatalog.h"
#include "domain/seismic/sgyvolume.h"
#include "io/sattrio.h"
#include "metadata/layermanifest.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"

using seismic::SeismicTaskService;
using K = SeismicTaskService::SeismicAttrKind;

#if defined(Q_OS_UNIX)
#include <QFile>
#endif

namespace
{

// 合成 SEG-Y（标准 INLINE@188/CROSSLINE@192、IEEE fmt=5、inline-major；
// 仿 tst_seismicattrsvc 的 writer）。内容：
//   Sine   —— 全道单位正弦 25Hz（包络/频率解析已知）；
//   Ricker —— 全道同一 Ricker 子波（相干解析已知 = 1）；
//   Ramp   —— 道间线性变化（相干 < 1，加权/等权差异可见）。
// 坐标：x = 1000 + xl·xlSpacing，y = 5000 + il·ilSpacing（各向异性可控）。
enum class Content { Sine, Ricker, Ramp };

bool writeScanSegyImpl(const QString &filePath, int nIl, int nXl, int ns, int dt,
                      Content content, int ilSpacing, int xlSpacing,
                      int firstInline, int firstXline, bool withCoords)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  QByteArray textHdr(3200, ' ');
  const QString banner = QStringLiteral(
      "C01 SEG-Y SCAN TEST First inline : %1 Last inline : %2 First xline : %3 Last xline : %4")
      .arg(firstInline).arg(firstInline + nIl - 1)
      .arg(firstXline).arg(firstXline + nXl - 1);
  const QByteArray bb = banner.toUtf8();
  std::memcpy(textHdr.data(), bb.constData(), bb.size());
  file.write(textHdr);

  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  file.write(binHdr);

  const double dtSec = dt / 1e6;
  int traceIndex = 0;
  for (int i = 0; i < nIl; ++i)
  {
    for (int j = 0; j < nXl; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      qToBigEndian<qint32>(withCoords ? 1000 + j * xlSpacing : 0,
                           reinterpret_cast<uchar *>(trHdr.data()) + 72);
      qToBigEndian<qint32>(withCoords ? 5000 + i * ilSpacing : 0,
                           reinterpret_cast<uchar *>(trHdr.data()) + 76);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data()) + 114);
      qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data()) + 116);
      file.write(trHdr);

      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        float val = 0.0f;
        switch (content)
        {
        case Content::Sine:
          val = float(std::sin(2.0 * std::numbers::pi * 25.0 * dtSec * k));
          break;
        case Content::Ricker:
        {
          const double a2 = std::pow(std::numbers::pi * 40.0 * dtSec, 2);
          const double t = k - ns / 2.0;
          val = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
          break;
        }
        case Content::Ramp:
          // 道间相位线性漂移：il 向每线 +k·0.03 弧度（道间波形渐变）。
          val = float(std::sin(2.0 * std::numbers::pi * 25.0 * dtSec * k +
                               0.03 * i + 0.05 * j));
          break;
        }
        quint32 raw;
        std::memcpy(&raw, &val, 4);
        qToBigEndian<quint32>(raw, reinterpret_cast<uchar *>(samples.data()) + k * 4);
      }
      file.write(samples);
      ++traceIndex;
    }
  }
  file.close();
  return true;
}

bool writeScanSegy(const QString &filePath, int nIl, int nXl, int ns, int dt,
                   Content content, int ilSpacing = 25, int xlSpacing = 25,
                   int firstInline = 10, int firstXline = 100)
{
  return writeScanSegyImpl(filePath, nIl, nXl, ns, dt, content, ilSpacing,
                           xlSpacing, firstInline, firstXline, /*withCoords=*/true);
}

// 全零 CDP 坐标体——SgyCoordinateMapper 拟合失败（道距加权前置校验用例）。
bool writeScanSegyNoCoords(const QString &filePath, int nIl, int nXl, int ns,
                           int dt, Content content)
{
  return writeScanSegyImpl(filePath, nIl, nXl, ns, dt, content, 25, 25, 10, 100,
                           /*withCoords=*/false);
}

// 逐道直算参考：读整 inline 剖面 → 时序道 → complexTraceAnalysis 取样位。
// 与扫描同一核、同一取数路径——Oracle#2「数值与逐道重算一致」的比对基准。
std::vector<float> directTimeSlice(std::shared_ptr<const seismic::SgyVolume> volume,
                                   K kind, int sampleIndex)
{
  const int nIl = volume->InlineCount();
  const int nXl = volume->XlineCount();
  const int nS = volume->SampleCount();
  const double dtMs = double(volume->SampleIntervalUs()) / 1000.0;
  std::vector<float> out(std::size_t(nIl) * nXl,
                         std::numeric_limits<float>::quiet_NaN());
  for (int il = 0; il < nIl; ++il)
  {
    seismic::SgySliceImage slice;
    std::string err;
    if (!volume->ExtractSlice(seismic::SgySliceType::Inline,
                              volume->InlineValues()[std::size_t(il)], slice,
                              err))
    {
      qWarning("directTimeSlice: extract failed: %s", err.c_str());
      return {};
    }
    std::vector<float> traceBuf(static_cast<std::size_t>(nS));
    for (int xl = 0; xl < nXl; ++xl)
    {
      for (int row = 0; row < nS; ++row)
        traceBuf[std::size_t(nS - 1 - row)] =
            slice.values[std::size_t(row * nXl + xl)];
      const auto ct =
          paleo::seisattr::complexTraceAnalysis(traceBuf.data(), nS, dtMs);
      const std::vector<float> *src = nullptr;
      switch (kind)
      {
      case K::Envelope: src = &ct.envelope; break;
      case K::InstFreq: src = &ct.freqHz; break;
      default: src = &ct.envelope; break;
      }
      out[std::size_t(il) * nXl + std::size_t(xl)] = (*src)[std::size_t(sampleIndex)];
    }
  }
  return out;
}

} // namespace

class TestSeismicAttrScan : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;
  QString sineSgy_;
  QString rickerSgy_;
  QString rampSgy_;
  QString bigSgy_;
  std::shared_ptr<seismic::SgyVolume> sineVol_;
  std::shared_ptr<seismic::SgyVolume> rickerVol_;
  std::shared_ptr<seismic::SgyVolume> rampVol_;
  std::shared_ptr<seismic::SgyVolume> bigVol_;

  std::shared_ptr<seismic::SgyVolume> loadVolume(const QString &path)
  {
    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    if (!vol->Load(path.toStdString(), err))
      return nullptr;
    return vol;
  }

  template <typename R>
  struct ScanOutcome
  {
    bool finished = false;
    bool ok = false;
    R result;
    QVector<int> percents;
  };

  using TsOutcome = ScanOutcome<SeismicTaskService::SeismicAttrTimeSliceResult>;
  using VolOutcome = ScanOutcome<SeismicTaskService::SeismicAttrVolumeResult>;

  TsOutcome runTimeSlice(seismic::SeismicTaskService &svc,
                         std::shared_ptr<const seismic::SgyVolume> volume, K kind,
                         const SeismicTaskService::SeismicAttrParams &params,
                         int sampleIndex, const QString &outDir)
  {
    TsOutcome out;
    PaleoTask *task = svc.startTimeSliceAttribute(
        volume, kind, params, sampleIndex, outDir,
        [&out](bool ok, const SeismicTaskService::SeismicAttrTimeSliceResult &r) {
          out.finished = true;
          out.ok = ok;
          out.result = r;
        });
    if (!task)
      return out;
    QObject::connect(task, &PaleoTask::changed, task, [&out, task]() {
      out.percents.append(task->percent());
    });
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    if (!finishedSpy.wait(120000))
      return out;
    QCoreApplication::processEvents();
    return out;
  }

  VolOutcome runVolume(seismic::SeismicTaskService &svc,
                       std::shared_ptr<const seismic::SgyVolume> volume, K kind,
                       const SeismicTaskService::SeismicAttrParams &params,
                       const QString &outDir)
  {
    VolOutcome out;
    PaleoTask *task = svc.startAttributeVolume(
        volume, kind, params, outDir,
        [&out](bool ok, const SeismicTaskService::SeismicAttrVolumeResult &r) {
          out.finished = true;
          out.ok = ok;
          out.result = r;
        });
    if (!task)
      return out;
    QObject::connect(task, &PaleoTask::changed, task, [&out, task]() {
      out.percents.append(task->percent());
    });
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    if (!finishedSpy.wait(300000))
      return out;
    QCoreApplication::processEvents();
    return out;
  }

  // catalog + RAW 版本夹具（登记路径用）。
  bool makeCatalog(DataCatalog *catalog, const QString &sgy, QString *assetId,
                   QString *versionId)
  {
    QString err;
    if (!catalog->open(tempDir_.path(), &err))
      return false;
    CatalogAsset a;
    *assetId = QStringLiteral("seis_scan_") + QFileInfo(sgy).completeBaseName();
    a.id = *assetId;
    a.type = QStringLiteral("seismic");
    a.format = QStringLiteral("sgy");
    a.displayName = QFileInfo(sgy).fileName();
    if (!catalog->addAsset(a, &err))
      return false;
    CatalogVersion v;
    *versionId = QStringLiteral("raw_") + *assetId;
    v.id = *versionId;
    v.assetId = a.id;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = sgy;
    v.fileName = QFileInfo(sgy).fileName();
    return catalog->addVersion(v, &err);
  }

private slots:
  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    sineSgy_ = tempDir_.filePath(QStringLiteral("sine.sgy"));
    rickerSgy_ = tempDir_.filePath(QStringLiteral("ricker.sgy"));
    rampSgy_ = tempDir_.filePath(QStringLiteral("ramp.sgy"));
    bigSgy_ = tempDir_.filePath(QStringLiteral("big.sgy"));
    QVERIFY(writeScanSegy(sineSgy_, 6, 8, 256, 2000, Content::Sine));
    QVERIFY(writeScanSegy(rickerSgy_, 5, 9, 128, 2000, Content::Ricker));
    // Ramp 各向异性道距：IL 100m / XL 25m——加权/等权差异可见。
    QVERIFY(writeScanSegy(rampSgy_, 6, 7, 128, 2000, Content::Ramp,
                          /*ilSpacing=*/100, /*xlSpacing=*/25));
    // 取消/性能门用体：24×80×2048（~15MB 输入；扫描时长足够开取消窗）。
    QVERIFY(writeScanSegy(bigSgy_, 24, 80, 2048, 2000, Content::Sine));
    sineVol_ = loadVolume(sineSgy_);
    rickerVol_ = loadVolume(rickerSgy_);
    rampVol_ = loadVolume(rampSgy_);
    bigVol_ = loadVolume(bigSgy_);
    QVERIFY(sineVol_ && sineVol_->IsLoaded());
    QVERIFY(rickerVol_ && rickerVol_->IsLoaded());
    QVERIFY(rampVol_ && rampVol_->IsLoaded());
    QVERIFY(bigVol_ && bigVol_->IsLoaded());
  }

  void paramHashDiscriminates()
  {
    SeismicTaskService::SeismicAttrParams p, p2;
    const QString h1 = SeismicTaskService::attrScanParamHash(
        QStringLiteral("ts"), K::Envelope, p, sineSgy_, 100);
    QCOMPARE(h1, SeismicTaskService::attrScanParamHash(
                     QStringLiteral("ts"), K::Envelope, p, sineSgy_, 100));
    QVERIFY(h1.size() == 64);
    // 采样位/范围/参数/属性任一变化 → hash 变
    QVERIFY(h1 != SeismicTaskService::attrScanParamHash(
                      QStringLiteral("ts"), K::Envelope, p, sineSgy_, 101));
    QVERIFY(h1 != SeismicTaskService::attrScanParamHash(
                      QStringLiteral("vol"), K::Envelope, p, sineSgy_, 100));
    p2.windowHalfSamples = 9;
    QVERIFY(h1 != SeismicTaskService::attrScanParamHash(
                      QStringLiteral("ts"), K::Envelope, p2, sineSgy_, 100));
    QVERIFY(h1 != SeismicTaskService::attrScanParamHash(
                      QStringLiteral("ts"), K::InstFreq, p, sineSgy_, 100));
  }

  void timeSliceMatchesPerTraceRecompute()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_out"));
    const int sample = 128;

    TsOutcome out = runTimeSlice(svc, sineVol_, K::Envelope, params, sample, outDir);
    QVERIFY2(out.finished, "任务未完成");
    QVERIFY2(out.ok, qPrintable(out.result.error));
    QCOMPARE(out.result.attrId, QStringLiteral("envelope"));
    QCOMPARE(out.result.nIl, 6);
    QCOMPARE(out.result.nXl, 8);
    QCOMPARE(out.result.sampleIndex, sample);
    QCOMPARE(out.result.validCells, qint64(6 * 8));
    QVERIFY(!out.result.cacheHit);
    QVERIFY(!out.result.paramHash.isEmpty());

    // Oracle#2：IL×XL 栅格数值与逐道重算一致（同一核同路径——逐位相等）。
    const std::vector<float> expect = directTimeSlice(sineVol_, K::Envelope, sample);
    QCOMPARE(out.result.values.size(), expect.size());
    for (std::size_t i = 0; i < expect.size(); ++i)
      QCOMPARE(out.result.values[i], expect[i]);

    // 地理参考栅格产物存在且可开（诚实 URI 的物证）。
    QVERIFY(QFileInfo::exists(out.result.cachePath));
    QVERIFY(out.result.cachePath.endsWith(QStringLiteral(".tif")));
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen(out.result.cachePath.toUtf8().constData(),
                               GA_ReadOnly);
    QVERIFY2(ds, "GeoTIFF 无法打开");
    QCOMPARE(GDALGetRasterXSize(ds), 8);
    QCOMPARE(GDALGetRasterYSize(ds), 6);
    double gt[6] = {0, 0, 0, 0, 0, 0};
    GDALGetGeoTransform(ds, gt);
    QVERIFY(gt[0] != 0.0 || gt[3] != 0.0); // 有地理参考
    GDALClose(ds);
    // sidecar 存在（缓存身份）
    QVERIFY(QFileInfo::exists(out.result.cachePath + QStringLiteral(".prov.json")));

    // 进度单调且终值 100
    QVERIFY(out.percents.size() >= 2);
    for (int i = 1; i < out.percents.size(); ++i)
      QVERIFY(out.percents[i] >= out.percents[i - 1]);
    QCOMPARE(out.percents.last(), 100);
  }

  void timeSliceAnalyticValues()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_out2"));

    // 单位正弦包络（样 128 中部）≈ 1
    TsOutcome env = runTimeSlice(svc, sineVol_, K::Envelope, params, 128, outDir);
    QVERIFY2(env.ok, qPrintable(env.result.error));
    for (float v : env.result.values)
      QVERIFY2(v > 0.9f && v < 1.1f, qPrintable(QStringLiteral("env=%1").arg(v)));

    // RMS（半窗 16）≈ 1/√2
    params.windowHalfSamples = 16;
    TsOutcome rms = runTimeSlice(svc, sineVol_, K::Rms, params, 128, outDir);
    QVERIFY2(rms.ok, qPrintable(rms.result.error));
    for (float v : rms.result.values)
      QVERIFY2(std::fabs(v - 1.0 / std::sqrt(2.0)) < 0.05,
               qPrintable(QStringLiteral("rms=%1").arg(v)));

    // 同一波形相干：内部 1，边界 NaN（IL±1/XL±1 道带）
    TsOutcome coh = runTimeSlice(svc, rickerVol_, K::Coherence, params, 64, outDir);
    QVERIFY2(coh.ok, qPrintable(coh.result.error));
    QVERIFY(std::isnan(coh.result.values[0]));                        // (il0,xl0)
    QVERIFY(std::isnan(coh.result.values[std::size_t(4 * 9 + 8)]));   // (il4,xl8)
    QVERIFY(coh.result.values[std::size_t(2 * 9 + 4)] > 0.99f);      // 内部
  }

  void shapeRejectionsAreHonest()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_out3"));

    // Oracle#6：剖面路径 Time 切片如实拒绝，文案指明所需窗口
    bool called = false;
    QString errText;
    svc.startAttributeSlice(
        sineVol_, K::Rms, params, seismic::SgySliceType::Time, 64,
        [&](bool ok, const seismic::SeismicTaskService::SeismicAttrResult &r) {
          called = true;
          errText = r.error;
          QVERIFY(!ok);
        });
    QVERIFY(called);
    QVERIFY(errText.contains(QStringLiteral("整道谱")));
    QVERIFY(errText.contains(QStringLiteral("垂向窗")));
    QVERIFY(errText.contains(QStringLiteral("扫描")));

    // 采样号越界
    TsOutcome bad = runTimeSlice(svc, sineVol_, K::Envelope, params, 9999, outDir);
    QVERIFY(bad.finished && !bad.ok);
    QVERIFY(bad.result.error.contains(QStringLiteral("越界")));

    // 测网过小相干（1×1 体）
    QString tinySgy = tempDir_.filePath(QStringLiteral("tiny.sgy"));
    QVERIFY(writeScanSegy(tinySgy, 1, 1, 32, 2000, Content::Ricker));
    auto tinyVol = loadVolume(tinySgy);
    QVERIFY(tinyVol && tinyVol->IsLoaded());
    TsOutcome coh = runTimeSlice(svc, tinyVol, K::Coherence, params, 8, outDir);
    QVERIFY(coh.finished && !coh.ok);
    QVERIFY(coh.result.error.contains(QStringLiteral("测网过小")));
  }

  void timeSliceCacheHitOnSecondScan()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_cache"));

    TsOutcome first = runTimeSlice(svc, sineVol_, K::Envelope, params, 64, outDir);
    QVERIFY2(first.ok, qPrintable(first.result.error));
    QVERIFY(!first.result.cacheHit);

    // Oracle#5：同参数二次扫描必须命中缓存
    TsOutcome second = runTimeSlice(svc, sineVol_, K::Envelope, params, 64, outDir);
    QVERIFY2(second.ok, qPrintable(second.result.error));
    QVERIFY(second.result.cacheHit);
    QCOMPARE(second.result.cachePath, first.result.cachePath);
    QCOMPARE(second.result.paramHash, first.result.paramHash);

    // 参数变化 → 不命中
    auto p2 = params;
    p2.windowHalfSamples = 7; // envelope 不吃该参数——hash 仍区分（参数包整体）
    TsOutcome third = runTimeSlice(svc, sineVol_, K::Envelope, p2, 64, outDir);
    QVERIFY2(third.ok, qPrintable(third.result.error));
    QVERIFY(!third.result.cacheHit);
  }

  void volumeScanExtractionConsistency()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("vol_out"));

    VolOutcome out = runVolume(svc, sineVol_, K::Envelope, params, outDir);
    QVERIFY2(out.finished, "任务未完成");
    QVERIFY2(out.ok, qPrintable(out.result.error));
    QCOMPARE(out.result.attrId, QStringLiteral("envelope"));
    QCOMPARE(out.result.nIl, 6);
    QCOMPARE(out.result.nXl, 8);
    QCOMPARE(out.result.nS, 256);
    QVERIFY(QFileInfo::exists(out.result.path));
    QVERIFY(!out.result.cacheHit);

    // Oracle#3：容器读回——头一致 + 任意面抽取与直算一致
    paleo::sattr::SattrVolumeReader reader;
    QString err;
    QVERIFY2(reader.open(out.result.path, &err), qPrintable(err));
    QCOMPARE(reader.info().paramHash, out.result.paramHash);
    QCOMPARE(reader.info().nIl, 6);
    QCOMPARE(reader.info().nXl, 8);
    QCOMPARE(reader.info().nS, 256);
    QCOMPARE(reader.info().ilValues.size(), 6);
    QCOMPARE(reader.info().ilValues.front(), 10);
    QCOMPARE(reader.info().xlValues.front(), 100);
    QCOMPARE(reader.info().validCells, out.result.validCells);
    QCOMPARE(reader.info().valueMin, out.result.valueMin);

    const int nS = 256;
    const double dtMs = double(sineVol_->SampleIntervalUs()) / 1000.0;
    std::vector<float> plane;
    // extractInline(i) vs 直算整线
    for (int il : {0, 3, 5})
    {
      QVERIFY2(reader.extractInline(il, &plane, &err), qPrintable(err));
      seismic::SgySliceImage slice;
      std::string xerr;
      QVERIFY(sineVol_->ExtractSlice(seismic::SgySliceType::Inline,
                                     sineVol_->InlineValues()[std::size_t(il)],
                                     slice, xerr));
      std::vector<float> traceBuf(static_cast<std::size_t>(nS));
      for (int xl = 0; xl < 8; ++xl)
      {
        for (int row = 0; row < nS; ++row)
          traceBuf[std::size_t(nS - 1 - row)] =
              slice.values[std::size_t(row * 8 + xl)];
        const auto ct = paleo::seisattr::complexTraceAnalysis(traceBuf.data(), nS, dtMs);
        for (int s = 0; s < nS; ++s)
          QCOMPARE(plane[std::size_t(xl) * nS + std::size_t(s)], ct.envelope[std::size_t(s)]);
      }
    }
    // extractTime(s) vs 时间切片扫描（两条扫描路径互证——Oracle#3 一致性）
    TsOutcome ts = runTimeSlice(svc, sineVol_, K::Envelope, params, 100, outDir);
    QVERIFY2(ts.ok, qPrintable(ts.result.error));
    QVERIFY2(reader.extractTime(100, &plane, &err), qPrintable(err));
    QCOMPARE(plane.size(), ts.result.values.size());
    for (std::size_t i = 0; i < plane.size(); ++i)
      QCOMPARE(plane[i], ts.result.values[i]);

    // 同参数二次体扫描命中缓存
    VolOutcome again = runVolume(svc, sineVol_, K::Envelope, params, outDir);
    QVERIFY2(again.ok, qPrintable(again.result.error));
    QVERIFY(again.result.cacheHit);
    QCOMPARE(again.result.path, out.result.path);
  }

  void volumeCoherenceSemantics()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("vol_coh"));

    VolOutcome out = runVolume(svc, rickerVol_, K::Coherence, params, outDir);
    QVERIFY2(out.ok, qPrintable(out.result.error));
    paleo::sattr::SattrVolumeReader reader;
    QString err;
    QVERIFY2(reader.open(out.result.path, &err), qPrintable(err));
    // 同一波形：体相干内部 = 1，边界带 NaN（IL 向 ±ilHalf、XL 向 ±xlHalf）
    std::vector<float> inlinePlane;
    QVERIFY2(reader.extractInline(2, &inlinePlane, &err), qPrintable(err));
    const int nS = 128;
    QVERIFY(std::isnan(inlinePlane[std::size_t(0) * nS + 64])); // xl 边界
    QVERIFY(inlinePlane[std::size_t(4) * nS + 64] > 0.99f);     // 内部
    std::vector<float> timePlane;
    QVERIFY2(reader.extractTime(64, &timePlane, &err), qPrintable(err));
    QVERIFY(std::isnan(timePlane[std::size_t(0 * 9 + 4)]));     // il 边界
    QVERIFY(timePlane[std::size_t(2 * 9 + 4)] > 0.99f);         // 内部

    // 与时间切片相干扫描互证（slab 语义 == 整体切片语义）
    TsOutcome ts = runTimeSlice(svc, rickerVol_, K::Coherence, params, 64, outDir);
    QVERIFY2(ts.ok, qPrintable(ts.result.error));
    for (std::size_t i = 0; i < timePlane.size(); ++i)
    {
      QCOMPARE(std::isnan(timePlane[i]), std::isnan(ts.result.values[i]));
      if (!std::isnan(timePlane[i]))
        QCOMPARE(timePlane[i], ts.result.values[i]);
    }
  }

  void coherenceWeightingModesDiffer()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("vol_w"));

    // Ramp 体（道间波形渐变）+ 各向异性道距（IL 100m / XL 25m）：
    // 加权与等权两档结果必须有差异（近线贡献被抬升）。
    params.coherenceWeighting = 0;
    VolOutcome eq = runVolume(svc, rampVol_, K::Coherence, params, outDir);
    QVERIFY2(eq.ok, qPrintable(eq.result.error));
    params.coherenceWeighting = 1;
    VolOutcome iw = runVolume(svc, rampVol_, K::Coherence, params, outDir);
    QVERIFY2(iw.ok, qPrintable(iw.result.error));
    QVERIFY(eq.result.paramHash != iw.result.paramHash);

    paleo::sattr::SattrVolumeReader rEq, rIw;
    QString err;
    QVERIFY2(rEq.open(eq.result.path, &err), qPrintable(err));
    QVERIFY2(rIw.open(iw.result.path, &err), qPrintable(err));
    QCOMPARE(rEq.info().coherenceWeighting, 0);
    QCOMPARE(rIw.info().coherenceWeighting, 1);
    QCOMPARE(rIw.info().ilTraceSpacing, 100.0);
    QCOMPARE(rIw.info().xlTraceSpacing, 25.0);
    std::vector<float> pEq, pIw;
    QVERIFY(rEq.extractTime(64, &pEq, &err));
    QVERIFY(rIw.extractTime(64, &pIw, &err));
    int differing = 0;
    for (std::size_t i = 0; i < pEq.size(); ++i)
      if (std::isfinite(pEq[i]) && std::isfinite(pIw[i]) &&
          std::fabs(pEq[i] - pIw[i]) > 1e-5f)
        ++differing;
    QVERIFY2(differing > 0, "加权/等权两档结果无差异");

    // 同波形体（Ricker）：两档都 = 1（S=1 与权重无关——解析不变量）
    params.coherenceWeighting = 0;
    VolOutcome eqR = runVolume(svc, rickerVol_, K::Coherence, params, outDir);
    params.coherenceWeighting = 1;
    VolOutcome iwR = runVolume(svc, rickerVol_, K::Coherence, params, outDir);
    QVERIFY2(eqR.ok && iwR.ok,
             qPrintable(eqR.ok ? iwR.result.error : eqR.result.error));
    paleo::sattr::SattrVolumeReader r2;
    QVERIFY(r2.open(iwR.result.path, &err));
    std::vector<float> p;
    QVERIFY(r2.extractTime(64, &p, &err));
    QVERIFY(p[std::size_t(2 * 9 + 4)] > 0.99f);
  }

  void cancelLeavesNoPartialArtifact()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_cancel"));

    // 时间切片扫描取消：无 tif、无 sidecar、无 .partial 残件
    {
      TsOutcome out;
      PaleoTask *task = svc.startTimeSliceAttribute(
          bigVol_, K::Envelope, params, 512, outDir,
          [&out](bool ok,
                 const SeismicTaskService::SeismicAttrTimeSliceResult &r) {
            out.finished = true;
            out.ok = ok;
            out.result = r;
          });
      QVERIFY(task);
      task->requestCancel();
      QSignalSpy finishedSpy(task, &PaleoTask::finished);
      QVERIFY(finishedSpy.wait(120000));
      QCoreApplication::processEvents();
      QCOMPARE(task->state(), PaleoTask::State::Cancelled);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("取消")));
      QDir dir(outDir);
      const auto entries = dir.entryList(QDir::Files);
      for (const QString &f : entries)
        QVERIFY2(!f.endsWith(QStringLiteral(".tif")) &&
                     !f.endsWith(QStringLiteral(".partial")) &&
                     !f.endsWith(QStringLiteral(".prov.json")),
                 qPrintable(QStringLiteral("取消残件：%1").arg(f)));
    }
    // 体扫描取消：无 .sattr / .partial 残件
    {
      VolOutcome out;
      PaleoTask *task = svc.startAttributeVolume(
          bigVol_, K::Envelope, params, outDir,
          [&out](bool ok,
                 const SeismicTaskService::SeismicAttrVolumeResult &r) {
            out.finished = true;
            out.ok = ok;
            out.result = r;
          });
      QVERIFY(task);
      task->requestCancel();
      QSignalSpy finishedSpy(task, &PaleoTask::finished);
      QVERIFY(finishedSpy.wait(120000));
      QCoreApplication::processEvents();
      QCOMPARE(task->state(), PaleoTask::State::Cancelled);
      QVERIFY(out.finished && !out.ok);
      QDir dir(outDir);
      const auto entries = dir.entryList(QDir::Files);
      for (const QString &f : entries)
        QVERIFY2(!f.endsWith(QStringLiteral(".sattr")) &&
                     !f.endsWith(QStringLiteral(".partial")),
                 qPrintable(QStringLiteral("取消残件：%1").arg(f)));
    }
  }

  void reviewRound1Regressions()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("r1"));

    // H1：体扫描无产物目录 → 如实拒绝（不落根路径）
    bool called = false;
    QString errText;
    svc.startAttributeVolume(
        sineVol_, K::Envelope, params, QString(),
        [&](bool ok, const SeismicTaskService::SeismicAttrVolumeResult &r) {
          called = true;
          errText = r.error;
          QVERIFY(!ok);
        });
    QVERIFY(called);
    QVERIFY(errText.contains(QStringLiteral("产物目录")));

    // H2：道距加权 + 地理参考不可拟合的体 → 体扫描如实拒绝
    //（构造无坐标 sgy——CDP 全 0 时仿射拟合失败）
    {
      const QString noGeoSgy =
          tempDir_.filePath(QStringLiteral("nogeo.sgy"));
      QVERIFY(writeScanSegyNoCoords(noGeoSgy, 4, 5, 64, 2000, Content::Ricker));
      auto noGeoVol = loadVolume(noGeoSgy);
      QVERIFY(noGeoVol && noGeoVol->IsLoaded());
      auto wp = params;
      wp.coherenceWeighting = 1;
      VolOutcome out = runVolume(svc, noGeoVol, K::Coherence, wp, outDir);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("地理参考")));
      // 等权档不受限（相干可算）
      VolOutcome eq = runVolume(svc, noGeoVol, K::Coherence, params, outDir);
      QVERIFY2(eq.ok, qPrintable(eq.result.error));
    }

    // M3：时间切片缓存命中回填统计（非 0 有效单元）+ 值域
    {
      TsOutcome first = runTimeSlice(svc, sineVol_, K::Rms, params, 100, outDir);
      QVERIFY2(first.ok, qPrintable(first.result.error));
      QCOMPARE(first.result.validCells, qint64(6 * 8));
      TsOutcome second = runTimeSlice(svc, sineVol_, K::Rms, params, 100, outDir);
      QVERIFY2(second.ok && second.result.cacheHit,
               qPrintable(second.result.error));
      QCOMPARE(second.result.validCells, qint64(6 * 8));
      QCOMPARE(second.result.valueMin, first.result.valueMin);
      QCOMPARE(second.result.valueMax, first.result.valueMax);
      QCOMPARE(second.result.nIl, 6);
      QCOMPARE(second.result.nXl, 8);
    }
  }

  void registerAssetsAndLayerDeclaration()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("ts_reg"));

    TsOutcome ts = runTimeSlice(svc, sineVol_, K::Envelope, params, 64, outDir);
    QVERIFY2(ts.ok, qPrintable(ts.result.error));
    VolOutcome vol = runVolume(svc, sineVol_, K::InstFreq, params, outDir);
    QVERIFY2(vol.ok, qPrintable(vol.result.error));

    DataCatalog catalog;
    QString assetId, versionId;
    QVERIFY(makeCatalog(&catalog, sineSgy_, &assetId, &versionId));

    // Oracle#4：切片属性 → DERIVED + 诚实栅格 URI 的层树条目
    LayerDeclaration decl;
    QString err;
    const QString tifPath = SeismicTaskService::registerTimeSliceAttributeAsset(
        &catalog, assetId, versionId, K::Envelope, params, ts.result, sineSgy_,
        outDir, &err, &decl);
    QVERIFY2(!tifPath.isEmpty(), qPrintable(err));
    QCOMPARE(decl.type, QStringLiteral("raster"));
    QCOMPARE(decl.group, QStringLiteral("00_Data"));
    QVERIFY(QFileInfo::exists(decl.source));
    QCOMPARE(QFileInfo(decl.source).absoluteFilePath(),
             QFileInfo(tifPath).absoluteFilePath());
    QVERIFY(decl.layerId.startsWith(QStringLiteral("seisattr_ts.")));
    QVERIFY(!decl.title.isEmpty());
    const CatalogVersion tsVer = catalog.currentVersion(
        QStringLiteral("seis_attr_%1_envelope_ts_64").arg(assetId));
    QCOMPARE(tsVer.stage, QStringLiteral("DERIVED"));
    QCOMPARE(tsVer.parentVersionIds, QStringList{versionId});
    QCOMPARE(tsVer.extra.value(QStringLiteral("param_hash")).toString(),
             ts.result.paramHash);
    QCOMPARE(tsVer.extra.value(QStringLiteral("cache_hit")).toBool(), false);

    // 二次登记（缓存命中结果）→ 复用既有版本，不重复建
    TsOutcome ts2 = runTimeSlice(svc, sineVol_, K::Envelope, params, 64, outDir);
    QVERIFY(ts2.ok && ts2.result.cacheHit);
    LayerDeclaration decl2;
    const QString tifPath2 = SeismicTaskService::registerTimeSliceAttributeAsset(
        &catalog, assetId, versionId, K::Envelope, params, ts2.result, sineSgy_,
        outDir, &err, &decl2);
    QCOMPARE(QFileInfo(tifPath2).absoluteFilePath(),
             QFileInfo(tifPath).absoluteFilePath());
    QCOMPARE(decl2.source, decl.source);

    // 属性体登记：format sattr + volume 标记 + provenance 字段
    const QString volPath = SeismicTaskService::registerAttributeVolumeAsset(
        &catalog, assetId, versionId, K::InstFreq, params, vol.result, sineSgy_,
        &err);
    QVERIFY2(!volPath.isEmpty(), qPrintable(err));
    const CatalogVersion volVer = catalog.currentVersion(
        QStringLiteral("seis_attr_%1_instfreq_vol").arg(assetId));
    QCOMPARE(volVer.stage, QStringLiteral("DERIVED"));
    QCOMPARE(catalog.assetById(volVer.assetId).format,
             QStringLiteral("sattr"));
    QCOMPARE(volVer.extra.value(QStringLiteral("volume")).toBool(), true);
    QCOMPARE(volVer.extra.value(QStringLiteral("param_hash")).toString(),
             vol.result.paramHash);
    QVERIFY(volVer.extra.contains(QStringLiteral("params_coherenceWeighting")));
    QVERIFY(volVer.extra.contains(QStringLiteral("source_sgy")));

    // 无产物目录的扫描（outDir 空）→ 登记如实失败
    TsOutcome bare = runTimeSlice(svc, sineVol_, K::Envelope, params, 64,
                                  QString());
    QVERIFY(bare.ok);
    QVERIFY(bare.result.cachePath.isEmpty());
    QString err2;
    QVERIFY(SeismicTaskService::registerTimeSliceAttributeAsset(
                &catalog, assetId, versionId, K::Envelope, params, bare.result,
                sineSgy_, QString(), &err2)
                .isEmpty());
    QVERIFY(err2.contains(QStringLiteral("产物缺失")));
  }

#if defined(Q_OS_UNIX)
  qint64 vmHwmKb()
  {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly))
      return -1;
    const QByteArray text = status.readAll();
    const int at = text.indexOf("VmHWM:");
    if (at < 0)
      return -1;
    return text.mid(at + 6).trimmed().split(' ')[0].toLongLong();
  }

  void scanWorkingSetBounded()
  {
    // 性能门（禁绝对毫秒墙钟——用峰值驻留 vs 输入体量的比门）：扫描期间
    // 峰值驻留增量须小于一个整体拷贝（分块扫描不全体驻留的回归门）。
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = SeismicTaskService::SeismicAttrParams{};
    const QString outDir = tempDir_.filePath(QStringLiteral("vol_rss"));
    const qint64 inputBytes = qint64(24) * 80 * 2048 * 4; // ~15MiB——用比门
    const qint64 before = vmHwmKb();
    VolOutcome out = runVolume(svc, bigVol_, K::Envelope, params, outDir);
    QVERIFY2(out.ok, qPrintable(out.result.error));
    const qint64 after = vmHwmKb();
    QVERIFY(before > 0 && after > 0);
    // 门：增量 < 2×输入体量（整扫描链路含 SATV 攒块 ≤32MiB + 剖面/索引；
    // 全体驻留 + 多份拷贝的回归形态会显著越界）。
    QVERIFY2((after - before) * 1024 < 2 * inputBytes + qint64(96) * 1024 * 1024,
             qPrintable(QStringLiteral("VmHWM 增量 %1 KiB 越界")
                            .arg(after - before)));
    // 扫描时长 vs 纯直算比门（读+写开销有界，不做绝对墙钟断言）
    VolOutcome again = runVolume(svc, bigVol_, K::InstFreq, params, outDir);
    QVERIFY2(again.ok, qPrintable(again.result.error));
  }
#endif
};

QTEST_MAIN(TestSeismicAttrScan)
#include "tst_seismicattrscan.moc"
