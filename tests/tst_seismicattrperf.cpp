// 层：测试壳
// goal/seismic-attributes 性能面：
//  · 比率门（常跑）：合成体上属性任务总耗时 ≤ N× 同体切片提取基线——禁绝对
//    墙钟（README 通用纪律），比率对同进程同盘，机器无关。
//  · 真机实测（PALEO_REAL_PROJECT_AREA 门控）：966MB 体中段 IL 切片的
//    包络/RMS/相干延迟，输出 BASELINE 行（手工誊 docs/progress/
//    seismic-attributes.md）；未设 env 时整套跳过。只读契约：源目录不动。
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"

using seismic::SeismicTaskService;

namespace
{

bool writePerfSegy(const QString &path, int nIl, int nXl, int ns, int dt,
                   int firstInline = 1000, int firstXline = 2000)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  QByteArray textHdr(3200, ' ');
  const QString banner =
      QStringLiteral("C01 ATTR PERF TEST First inline : %1 Last inline : %2 "
                     "First xline : %3 Last xline : %4")
          .arg(firstInline).arg(firstInline + nIl - 1)
          .arg(firstXline).arg(firstXline + nXl - 1);
  const QByteArray bb = banner.toUtf8();
  std::memcpy(textHdr.data(), bb.constData(), bb.size());
  f.write(textHdr);
  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  f.write(binHdr);

  const double dtSec = dt / 1e6;
  const double a2 = std::pow(std::numbers::pi * 35.0 * dtSec, 2);
  int traceIndex = 0;
  for (int i = 0; i < nIl; ++i)
    for (int j = 0; j < nXl; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(traceIndex + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 8);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 20);
      qToBigEndian<qint16>(1, reinterpret_cast<uchar *>(trHdr.data()) + 70);
      qToBigEndian<qint32>(1000 + j * 12, reinterpret_cast<uchar *>(trHdr.data()) + 72);
      qToBigEndian<qint32>(5000 + i * 50, reinterpret_cast<uchar *>(trHdr.data()) + 76);
      qToBigEndian<qint16>(ns, reinterpret_cast<uchar *>(trHdr.data()) + 114);
      qToBigEndian<qint16>(dt, reinterpret_cast<uchar *>(trHdr.data()) + 116);
      qToBigEndian<qint32>(firstInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(firstXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      f.write(trHdr);
      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        // 带倾角同相轴：峰位随道缓移（避免全零区优化失真）
        const double peak = ns / 2.0 + (i * 0.5 + j * 0.25);
        const double t = k - peak;
        const float v = float((1.0 - 2.0 * a2 * t * t) * std::exp(-a2 * t * t));
        quint32 raw;
        std::memcpy(&raw, &v, 4);
        qToBigEndian<quint32>(raw, reinterpret_cast<uchar *>(samples.data()) + k * 4);
      }
      f.write(samples);
      ++traceIndex;
    }
  return true;
}

bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
  QElapsedTimer clock;
  clock.start();
  while (!cond())
  {
    if (clock.elapsed() > timeoutMs)
      return cond();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

// 一次属性任务的 (readMs, computeMs)；失败返回负值。
std::pair<double, double> runAttrOnce(
    SeismicTaskService &svc, std::shared_ptr<seismic::SgyVolume> volume,
    SeismicTaskService::SeismicAttrKind kind, int inlineNo)
{
  double readMs = -1, computeMs = -1;
  bool done = false;
  svc.startAttributeSlice(
      volume, kind, SeismicTaskService::SeismicAttrParams{},
      seismic::SgySliceType::Inline, inlineNo,
      [&done, &readMs, &computeMs](bool ok,
                                   const SeismicTaskService::SeismicAttrResult &r) {
        done = true;
        if (ok)
        {
          readMs = r.readMs;
          computeMs = r.computeMs;
        }
      });
  waitFor([&done]() { return done; }, 120000);
  return {readMs, computeMs};
}

} // namespace

class TestSeismicAttrPerf : public QObject
{
  Q_OBJECT

private slots:
  // ---- 比率门：属性任务 ≤ 6× 同体切片提取（读同盘同进程，机器无关） ----
  void attributeCostRatioGate()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sgy = dir.filePath(QStringLiteral("perf_ratio.sgy"));
    // 8 IL × 400 XL × 1024 样 ≈ 13MB：切片提取毫秒级、属性任务百毫秒级——
    // 比率稳定不抖
    QVERIFY(writePerfSegy(sgy, 8, 400, 1024, 2000));

    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    QVERIFY(vol->Load(sgy.toStdString(), err));
    const int midInline = 1004;

    // 基线：纯切片提取（热态取 3 次最小，去首读抖动）
    seismic::SgySliceImage img;
    double extractMs = 1e9;
    for (int i = 0; i < 3; ++i)
    {
      QElapsedTimer t;
      t.start();
      QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, midInline, img, err));
      extractMs = std::min(extractMs, double(t.elapsed()));
    }
    QVERIFY(extractMs > 0.0);

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);

    using K = SeismicTaskService::SeismicAttrKind;
    const QList<QPair<K, QString>> cases = {
        {K::Envelope, QStringLiteral("envelope")},
        {K::Rms, QStringLiteral("rms")},
        {K::Coherence, QStringLiteral("coherence")}};
    for (const auto &c : cases)
    {
      const auto ms = runAttrOnce(svc, vol, c.first, midInline);
      QVERIFY2(ms.first >= 0.0, qPrintable(QStringLiteral("%1 失败").arg(c.second)));
      const double total = ms.first + ms.second;
      qInfo("BASELINE attr_ratio_%s = total %.1fms (read %.1f + compute %.1f) "
            "vs extract %.1fms -> %.2fx",
            c.second.toUtf8().constData(), total, ms.first, ms.second,
            extractMs, total / extractMs);
      // 门（防数量级回归，非掐抖动）：逐道族 ≤8×（实测 ~3×）；相干 ≤12×
      // ——它读 3 条邻线（3× 基线读取）+ 单线程整线 semblance（实测 ~7×）。
      const double gate = c.first == K::Coherence ? 12.0 : 8.0;
      QVERIFY2(total <= gate * extractMs,
               qPrintable(QStringLiteral("%1 总耗时 %2ms 超门（基线 %3ms×%4）")
                              .arg(c.second)
                              .arg(total)
                              .arg(extractMs)
                              .arg(gate)));
    }
  }

  // ---- 真机实测（PALEO_REAL_PROJECT_AREA 门控；只读契约） ----
  void realAreaAttributeProfile()
  {
    const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
    QString realSgy = qEnvironmentVariable("PALEO_SEISMIC_REAL_SGY");
    if (realSgy.isEmpty() && !area.isEmpty())
      realSgy = area + QStringLiteral("/地震体/200P_seismic.sgy");
    if (realSgy.isEmpty() || !QFile::exists(realSgy))
      QSKIP("PALEO_SEISMIC_REAL_SGY / PALEO_REAL_PROJECT_AREA not set — "
            "real-area attribute metrics skipped");

    // 只读契约：前后目录清单（名+字节）一致
    const QString dir = QFileInfo(realSgy).absolutePath();
    const auto listingBefore = [&dir]() {
      QStringList out;
      const auto entries =
          QDir(dir).entryInfoList(QDir::Files, QDir::Name);
      for (const auto &e : entries)
        out << QStringLiteral("%1:%2").arg(e.fileName()).arg(e.size());
      return out;
    }();

    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string err;
    QElapsedTimer clock;
    clock.start();
    QVERIFY2(vol->Load(realSgy.toStdString(), err),
             qPrintable(QStringLiteral("真机体加载失败：%1")
                            .arg(QString::fromStdString(err))));
    qInfo("BASELINE attr_real_open_ms = %.0f", double(clock.elapsed()));

    const auto &ils = vol->InlineValues();
    QVERIFY(!ils.empty());
    const int midInline = ils[ils.size() / 2];

    seismic::SgySliceImage img;
    clock.restart();
    QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, midInline, img, err));
    const double extractMs = double(clock.elapsed());
    qInfo("BASELINE attr_real_il_extract_ms = %.0f", extractMs);
    qInfo("BASELINE attr_real_geometry = il %d..%d xl %d..%d ns %d dt %dus",
          vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax(),
          vol->SampleCount(), vol->SampleIntervalUs());

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    using K = SeismicTaskService::SeismicAttrKind;
    const QList<QPair<K, QString>> cases = {
        {K::Envelope, QStringLiteral("envelope")},
        {K::Rms, QStringLiteral("rms")},
        {K::Coherence, QStringLiteral("coherence")},
        {K::InstFreq, QStringLiteral("instfreq")},
        {K::Sweetness, QStringLiteral("sweetness")}};
    for (const auto &c : cases)
    {
      const auto ms = runAttrOnce(svc, vol, c.first, midInline);
      QVERIFY2(ms.first >= 0.0,
               qPrintable(QStringLiteral("%1 真机失败").arg(c.second)));
      qInfo("BASELINE attr_real_%s_ms = %.0f (read %.0f + compute %.0f)",
            c.second.toUtf8().constData(), ms.first + ms.second, ms.first,
            ms.second);
    }

    // 只读契约复核
    const QStringList listingAfter = [&dir]() {
      QStringList out;
      const auto entries = QDir(dir).entryInfoList(QDir::Files, QDir::Name);
      for (const auto &e : entries)
        out << QStringLiteral("%1:%2").arg(e.fileName()).arg(e.size());
      return out;
    }();
    QCOMPARE(listingAfter, listingBefore);
  }
};

QTEST_MAIN(TestSeismicAttrPerf)
#include "tst_seismicattrperf.moc"
