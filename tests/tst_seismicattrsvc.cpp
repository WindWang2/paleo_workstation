// 层：数据（测试壳位于 tests/，被测对象为 services 层属性任务编排）
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

namespace
{

// 合成 SEG-Y（标准 INLINE@188/CROSSLINE@192 字位、IEEE fmt=5、inline-major）。
// 两种内容：Sine=全道单位正弦（25Hz，包络/频率解析已知）；Ricker=全道同一
// Ricker 子波（相干解析已知 =1）。仿 tests/tst_seismic_core.cpp 的 writer。
enum class Content { Sine, Ricker };

bool writeAttributeSegy(const QString &filePath, int nIl, int nXl, int ns, int dt,
                        Content content, int firstInline = 10,
                        int firstXline = 100)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  QByteArray textHdr(3200, ' ');
  const QString banner = QStringLiteral(
      "C01 SEG-Y ATTR TEST First inline : %1 Last inline : %2 First xline : %3 Last xline : %4")
      .arg(firstInline).arg(firstInline + nIl - 1)
      .arg(firstXline).arg(firstXline + nXl - 1);
  const QByteArray bb = banner.toUtf8();
  std::memcpy(textHdr.data(), bb.constData(), bb.size());
  file.write(textHdr);

  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  qToBigEndian<qint16>(0x0100, reinterpret_cast<uchar *>(binHdr.data()) + 300);
  file.write(binHdr);

  const double dtSec = dt / 1e6;
  int traceIndex = 0;
  for (int i = 0; i < nIl; ++i)
  {
    for (int j = 0; j < nXl; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(traceIndex + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 8);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 20);
      qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 70);
      qToBigEndian<qint32>(1000 + j * 25, reinterpret_cast<uchar *>(trHdr.data()) + 72);
      qToBigEndian<qint32>(5000 + i * 25, reinterpret_cast<uchar *>(trHdr.data()) + 76);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data()) + 114);
      qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data()) + 116);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      file.write(trHdr);

      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        float val = 0.0f;
        if (content == Content::Sine)
        {
          // 单位正弦 25Hz（dt=2000us 时 0.05 cyc/样，Nyquist 安全）
          val = float(std::sin(2.0 * std::numbers::pi * 25.0 * dtSec * k));
        }
        else
        {
          // Ricker 40Hz，峰位 ns/2；所有道相同 → 相干=1
          const double a2 = std::pow(std::numbers::pi * 40.0 * dtSec, 2);
          const double t = k - ns / 2.0;
          val = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
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

} // namespace

class TestSeismicAttrSvc : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;
  QString sineSgy_;
  QString rickerSgy_;
  QString cancelSgy_;
  std::shared_ptr<seismic::SgyVolume> sineVol_;
  std::shared_ptr<seismic::SgyVolume> rickerVol_;
  std::shared_ptr<seismic::SgyVolume> cancelVol_;

  std::shared_ptr<seismic::SgyVolume> loadVolume(const QString &path)
  {
    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    if (!vol->Load(path.toStdString(), err))
      return nullptr;
    return vol;
  }

  // 跑一个属性任务到终态，回调结果入成员，changed() 采样 percent 序列。
  struct RunOutcome
  {
    bool finished = false;
    bool ok = false;
    seismic::SeismicTaskService::SeismicAttrResult result;
    QVector<int> percents; // 每次 changed() 的 percent() 采样
  };

  RunOutcome runAttr(seismic::SeismicTaskService &svc,
                     std::shared_ptr<seismic::SgyVolume> volume,
                     seismic::SeismicTaskService::SeismicAttrKind kind,
                     const seismic::SeismicTaskService::SeismicAttrParams &params,
                     seismic::SgySliceType type, int index)
  {
    RunOutcome out;
    PaleoTask *task = svc.startAttributeSlice(
        volume, kind, params, type, index,
        [&out](bool ok, const seismic::SeismicTaskService::SeismicAttrResult &r)
        {
          out.finished = true;
          out.ok = ok;
          out.result = r;
        });
    if (!task)
      return out;
    QSignalSpy changedSpy(task, &PaleoTask::changed);
    QObject::connect(task, &PaleoTask::changed, [&out, task]()
                     { out.percents.append(task->percent()); });
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    if (!finishedSpy.wait(60000))
      return out; // 超时：finished=false，调用方断言失败
    QCoreApplication::processEvents(); // 排队的 onFinished 投递
    return out;
  }

private slots:
  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    sineSgy_ = tempDir_.filePath(QStringLiteral("sine.sgy"));
    rickerSgy_ = tempDir_.filePath(QStringLiteral("ricker.sgy"));
    cancelSgy_ = tempDir_.filePath(QStringLiteral("cancel.sgy"));
    QVERIFY(writeAttributeSegy(sineSgy_, 6, 8, 256, 2000, Content::Sine));
    QVERIFY(writeAttributeSegy(rickerSgy_, 5, 9, 128, 2000, Content::Ricker));
    // 取消用体：600 道 × 2048 样（包络 FFT 全程 ~百毫秒，取消窗足够宽）
    QVERIFY(writeAttributeSegy(cancelSgy_, 4, 600, 2048, 2000, Content::Ricker));
    sineVol_ = loadVolume(sineSgy_);
    rickerVol_ = loadVolume(rickerSgy_);
    cancelVol_ = loadVolume(cancelSgy_);
    QVERIFY(sineVol_ && sineVol_->IsLoaded());
    QVERIFY(rickerVol_ && rickerVol_->IsLoaded());
    QVERIFY(cancelVol_ && cancelVol_->IsLoaded());
  }

  void attrIdVocabulary()
  {
    using K = seismic::SeismicTaskService::SeismicAttrKind;
    QCOMPARE(SeismicTaskService::seismicAttrId(K::Envelope), QStringLiteral("envelope"));
    QCOMPARE(SeismicTaskService::seismicAttrId(K::InstFreq), QStringLiteral("instfreq"));
    QCOMPARE(SeismicTaskService::seismicAttrId(K::Coherence), QStringLiteral("coherence"));
    QCOMPARE(SeismicTaskService::seismicAttrId(K::Sweetness), QStringLiteral("sweetness"));
    QVERIFY(!SeismicTaskService::seismicAttrDisplayName(K::Rms).isEmpty());
    QVERIFY(SeismicTaskService::seismicAttrNeedsNeighbors(K::Coherence));
    QVERIFY(!SeismicTaskService::seismicAttrNeedsNeighbors(K::Envelope));
  }

  void envelopeSliceGeometryAndValues()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = seismic::SeismicTaskService::SeismicAttrParams{};

    RunOutcome out = runAttr(svc, sineVol_,
                             seismic::SeismicTaskService::SeismicAttrKind::Envelope,
                             params, seismic::SgySliceType::Inline, 11);
    QVERIFY2(out.finished, "任务未在超时内完成");
    QVERIFY2(out.ok, qPrintable(out.result.error));
    QVERIFY(out.result.image);
    QCOMPARE(out.result.attrId, QStringLiteral("envelope"));
    QCOMPARE(out.result.sectionIndex, 11);
    QCOMPARE(out.result.image->width, 8);
    QCOMPARE(out.result.image->height, 256);
    QCOMPARE(out.result.traceCount, 8);
    QCOMPARE(out.result.validTraceCount, 8);
    QVERIFY(out.result.readMs >= 0.0 && out.result.computeMs >= 0.0);

    // 单位正弦包络：中部样点 ≈ 1（镜像折点尾 ~2e-2 量级，门 ±0.1 宽裕）
    const auto &img = *out.result.image;
    for (int x : {1, 4, 6})
    {
      const float env = img.Value(x, 128);
      QVERIFY2(env > 0.9f && env < 1.1f,
               qPrintable(QStringLiteral("env@(%1,128)=%2").arg(x).arg(env)));
    }
    // 折点过冲：迹缘包络实测 ~1.19（轮2），门 <1.3
    QVERIFY(img.valueMax > 0.9f && img.valueMax < 1.3f);

    // 进度单调且终值 100（Oracle#3）
    QVERIFY(out.percents.size() >= 2);
    for (int i = 1; i < out.percents.size(); ++i)
      QVERIFY2(out.percents[i] >= out.percents[i - 1],
               qPrintable(QStringLiteral("percent 回退 %1<%2")
                            .arg(out.percents[i]).arg(out.percents[i - 1])));
    QCOMPARE(out.percents.last(), 100);
  }

  void instFreqAndRmsAnalytic()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    auto params = seismic::SeismicTaskService::SeismicAttrParams{};

    // 瞬时频率：中部 ≈ 25Hz（折点尾经差分放大，护栏取中部样点，门 ±5Hz）
    RunOutcome freq = runAttr(svc, sineVol_,
                              seismic::SeismicTaskService::SeismicAttrKind::InstFreq,
                              params, seismic::SgySliceType::Inline, 10);
    QVERIFY2(freq.ok, qPrintable(freq.result.error));
    QVERIFY(freq.result.image);
    const float f = freq.result.image->Value(4, 128);
    QVERIFY2(f > 20.0f && f < 30.0f,
             qPrintable(QStringLiteral("instfreq@(4,128)=%1").arg(f)));

    // RMS（半窗 16）：单位正弦的 RMS ≈ 1/√2 ≈ 0.7071（窗跨整数半周期，
    // 边缘缩窗偏差可忽略，门 ±0.05）
    params.windowHalfSamples = 16;
    RunOutcome rms = runAttr(svc, sineVol_,
                             seismic::SeismicTaskService::SeismicAttrKind::Rms,
                             params, seismic::SgySliceType::Xline, 102);
    QVERIFY2(rms.ok, qPrintable(rms.result.error));
    QCOMPARE(rms.result.image->width, 6); // XL 剖 → 宽 = inline 数
    const float r = rms.result.image->Value(3, 128);
    QVERIFY2(std::fabs(r - 1.0 / std::sqrt(2.0)) < 0.05,
             qPrintable(QStringLiteral("rms@(3,128)=%1").arg(r)));
  }

  void coherenceIdenticalTracesServiceLevel()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = seismic::SeismicTaskService::SeismicAttrParams{};

    RunOutcome out = runAttr(svc, rickerVol_,
                             seismic::SeismicTaskService::SeismicAttrKind::Coherence,
                             params, seismic::SgySliceType::Inline, 12);
    QVERIFY2(out.ok, qPrintable(out.result.error));
    QVERIFY(out.result.image);
    QCOMPARE(out.result.image->width, 9);
    QCOMPARE(out.result.image->height, 128);
    const auto &img = *out.result.image;
    // 同一波形：内部 XL 列高相干（子波主体行）；边界 XL NaN
    for (int x = 2; x <= 6; ++x)
      QVERIFY2(img.Value(x, 64) > 0.99f,
               qPrintable(QStringLiteral("coh@(%1,64)=%2").arg(x).arg(img.Value(x, 64))));
    QVERIFY(std::isnan(img.Value(0, 64)));
    QVERIFY(std::isnan(img.Value(8, 64)));
    QVERIFY(img.valueMax <= 1.0f + 1e-6f);
  }

  void rejectInvalidRequests()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = seismic::SeismicTaskService::SeismicAttrParams{};

    // 缺线（无最近线替代）
    {
      RunOutcome out = runAttr(svc, sineVol_,
                               seismic::SeismicTaskService::SeismicAttrKind::Envelope,
                               params, seismic::SgySliceType::Inline, 9999);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("不在本体")));
    }
    // 测网边缘线相干（无双侧邻线）
    {
      RunOutcome out = runAttr(svc, rickerVol_,
                               seismic::SeismicTaskService::SeismicAttrKind::Coherence,
                               params, seismic::SgySliceType::Inline, 10);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("边缘")));
    }
    // 时间切片（暂不支持，如实失败）
    {
      RunOutcome out = runAttr(svc, sineVol_,
                               seismic::SeismicTaskService::SeismicAttrKind::Envelope,
                               params, seismic::SgySliceType::Time, 64);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("时间切片")));
    }
    // 未加载体
    {
      RunOutcome out = runAttr(svc, nullptr,
                               seismic::SeismicTaskService::SeismicAttrKind::Envelope,
                               params, seismic::SgySliceType::Inline, 10);
      QVERIFY(out.finished && !out.ok);
      QVERIFY(out.result.error.contains(QStringLiteral("未加载")));
    }
  }

  void immediateCancel()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = seismic::SeismicTaskService::SeismicAttrParams{};

    RunOutcome out;
    PaleoTask *task = svc.startAttributeSlice(
        cancelVol_, seismic::SeismicTaskService::SeismicAttrKind::Envelope,
        params, seismic::SgySliceType::Inline, 10,
        [&out](bool ok, const seismic::SeismicTaskService::SeismicAttrResult &r)
        {
          out.finished = true;
          out.ok = ok;
          out.result = r;
        });
    QVERIFY(task);
    // 入池即取消：worker 入口/读进度回调/算循环三处协作检查点之一命中
    task->requestCancel();
    QSignalSpy finishedSpy(task, &PaleoTask::finished);
    QVERIFY(finishedSpy.wait(60000));
    QCoreApplication::processEvents();
    QCOMPARE(task->state(), PaleoTask::State::Cancelled);
    QVERIFY(out.finished && !out.ok);
    QVERIFY(out.result.error.contains(QStringLiteral("取消")));
  }

  void registerSattrAssetRoundtrip()
  {
    seismic::SeismicTaskService svc;
    svc.setTaskService(new PaleoTaskService(nullptr, &svc));
    const auto params = seismic::SeismicTaskService::SeismicAttrParams{};
    RunOutcome out = runAttr(svc, sineVol_,
                             seismic::SeismicTaskService::SeismicAttrKind::Envelope,
                             params, seismic::SgySliceType::Inline, 11);
    QVERIFY2(out.ok, qPrintable(out.result.error));

    DataCatalog catalog;
    QString err;
    QVERIFY(catalog.open(tempDir_.path(), &err));
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_attrsvc_1");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    seismicAsset.displayName = QStringLiteral("sine.sgy");
    QVERIFY(catalog.addAsset(seismicAsset, &err));
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_attrsvc_1");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = sineSgy_;
    rawVersion.fileName = QStringLiteral("sine.sgy");
    QVERIFY(catalog.addVersion(rawVersion, &err));

    const QString outDir = tempDir_.filePath(QStringLiteral("attributes"));
    const QString path = SeismicTaskService::registerAttributeSliceAsset(
        &catalog, seismicAsset.id, rawVersion.id, out.result, params, sineSgy_,
        outDir, &err);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(path.endsWith(QStringLiteral("envelope_il_11.sattr")));

    // SATR 容器：魔数 + 尺寸自洽（头 20B = 魔数4+版本4+w4+h4+jsonLen4，
    // 随后 JSON 与 w*h*4 小端 f32 值块）
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray blob = f.readAll();
    QVERIFY(blob.size() > 20);
    QVERIFY(blob.startsWith("SATR"));
    const quint32 jsonLen = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(blob.constData()) + 16);
    const qint64 expectSize = 20 + qint64(jsonLen) +
                              qint64(out.result.image->width) *
                                  out.result.image->height * 4;
    QCOMPARE(qint64(blob.size()), expectSize);

    // catalog 断言：DERIVED、父版本回指 RAW、extra 带 attrId
    const CatalogVersion derived = catalog.currentVersion(
        QStringLiteral("seis_attr_seis_attrsvc_1_envelope_il_11"));
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{rawVersion.id});
    QCOMPARE(derived.managed, false);
    QVERIFY(!derived.sha256.isEmpty());
    QCOMPARE(derived.extra.value(QStringLiteral("attrId")).toString(),
             QStringLiteral("envelope"));
  }
};

QTEST_MAIN(TestSeismicAttrSvc)
#include "tst_seismicattrsvc.moc"
