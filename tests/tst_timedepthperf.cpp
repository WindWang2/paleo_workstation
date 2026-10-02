// 层：测试壳
// goal/time-depth-velocity 性能面：
//  · 比率门（常跑）：W 井 IDW 模型整幅层位转换 ≤ 16× 单井转换——空间
//    查询代价随井数近线性（禁绝对墙钟，比率对同进程同机，机器无关）。
//  · 真机实测（PALEO_REAL_PROJECT_AREA 门控）：真实 TD 表建模 + 层位面
//    转换吞吐 + 966MB 体 IL 切片逐样深度换算延迟，输出 BASELINE 行（手工
//    誊 docs/progress/time-depth.md）；未设 env 时整套跳过。只读契约：源
//    目录前后清单一致。
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <functional>
#include <vector>

#include "../src/algorithms/velocitymodel.h"
#include "../src/domain/seismic/sgyvolume.h"
#include "../src/io/horizonbinner.h"
#include "../src/services/seismicmapping.h"
#include "../src/workflow/depthconversionworkflow.h"

namespace
{

paleo::velmodel::VelocityWellControl syntheticWell(const QString &id, double x, double y, int layers)
{
  paleo::velmodel::VelocityWellControl c;
  c.wellId = id;
  c.x = x;
  c.y = y;
  // 确定性伪随机层速度（每井不同），twt 300ms 起每层 +150ms。
  const double seed = 1.0 + std::abs(std::sin(x * 0.13 + y * 0.07));
  double twt = 300.0, depth = 300.0;
  for (int i = 0; i < layers; ++i)
  {
    c.knots.append(paleo::velmodel::VelocityKnot{twt, depth, QString()});
    const double v = 2000.0 + 900.0 * std::fmod(seed * (i + 1), 1.0);
    twt += 150.0;
    depth += v * 150.0 / 2000.0;
  }
  c.knots.append(paleo::velmodel::VelocityKnot{twt, depth, QString()});
  return c;
}

double bestOf3(const std::function<double()> &fn)
{
  double best = 1e18;
  for (int i = 0; i < 3; ++i)
    best = std::min(best, fn());
  return best;
}

} // namespace

class TestTimeDepthPerf : public QObject
{
  Q_OBJECT

private slots:
  void wellCountScalingRatioGate();
  void realAreaTimeDepthProfile();
};

// ---- 比率门：整幅转换代价随井数线性（防数量级回归，非掐抖动） ----
void TestTimeDepthPerf::wellCountScalingRatioGate()
{
  const int rows = 411, cols = 641; // 真工区层位栅格规模（263k 像元）
  const double gt[6] = {0.0, 25.0, 0.0, 10275.0, 0.0, -25.0};
  QVector<float> twt(rows * cols, 1800.0f);

  const paleo::velmodel::VelocityModel single = paleo::velmodel::VelocityModel::fit(
      {syntheticWell(QStringLiteral("W0"), 3000.0, 5000.0, 12)},
      paleo::velmodel::ModelType::IntervalAverage);
  QVERIFY(single.isValid());

  QVector<paleo::velmodel::VelocityWellControl> controls;
  for (int i = 0; i < 12; ++i)
    controls << syntheticWell(QStringLiteral("W%1").arg(i), 1000.0 + i * 700.0,
                              2000.0 + (i % 5) * 1500.0, 12 + (i % 4));
  const paleo::velmodel::VelocityModel multi = paleo::velmodel::VelocityModel::fit(
      controls, paleo::velmodel::ModelType::IntervalAverage);
  QVERIFY(multi.isValid());
  QCOMPARE(multi.wells().size(), 12);

  int convertedCells = 0;
  const double singleMs = bestOf3([&] {
    QElapsedTimer t;
    t.start();
    convertedCells =
        paleo::velmodel::convertTimeGridToDepth(single, twt, rows, cols, gt, -9999.0).convertedCells;
    return double(t.elapsed());
  });
  QCOMPARE(convertedCells, rows * cols);

  const double multiMs = bestOf3([&] {
    QElapsedTimer t;
    t.start();
    convertedCells =
        paleo::velmodel::convertTimeGridToDepth(multi, twt, rows, cols, gt, -9999.0).convertedCells;
    return double(t.elapsed());
  });
  QCOMPARE(convertedCells, rows * cols);

  QVERIFY(singleMs > 0.0);
  qInfo("BASELINE td_ratio_12wells = single %.1fms vs 12-well %.1fms -> %.2fx",
        singleMs, multiMs, multiMs / singleMs);
  // 门：≤ 16×（= W+4 余量；实测 ~12×，IDW 权重累加随井数线性）。
  QVERIFY2(multiMs <= 16.0 * singleMs,
           qPrintable(QStringLiteral("12 井转换 %1ms 超门（单井 %2ms×16）")
                          .arg(multiMs)
                          .arg(singleMs)));
}

// ---- 真机实测（PALEO_REAL_PROJECT_AREA 门控；只读契约） ----
void TestTimeDepthPerf::realAreaTimeDepthProfile()
{
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty() || !QFile::exists(area + QStringLiteral("/时深/TD")))
    QSKIP("PALEO_REAL_PROJECT_AREA not set — real-area time-depth metrics skipped");

  const auto listing = [&area]() {
    QStringList out;
    const auto entries = QDir(area).entryInfoList(QDir::Files, QDir::Name);
    for (const auto &e : entries)
      out << QStringLiteral("%1:%2").arg(e.fileName()).arg(e.size());
    return out;
  };
  const QStringList before = listing();

  // ---- 建模：真实 20 井校验炮表 + 井位坐标 ----
  VelocityModelBuildRequest req;
  req.type = paleo::velmodel::ModelType::IntervalAverage;
  req.wellHeadFilePaths << area + QStringLiteral("/井位/ExportWellHead.dat");
  const auto tdFiles = QDir(area + QStringLiteral("/时深/TD"))
                           .entryInfoList({QStringLiteral("*.dat")}, QDir::Files, QDir::Name);
  for (const auto &f : tdFiles)
    req.tdFilePaths << f.absoluteFilePath();
  QVERIFY2(req.tdFilePaths.size() >= 15,
           qPrintable(QStringLiteral("TD 表数量异常：%1").arg(req.tdFilePaths.size())));

  QElapsedTimer clock;
  clock.start();
  QString err;
  const paleo::velmodel::VelocityModel model = DepthConversionWorkflow::buildModel(req, &err);
  const double buildMs = double(clock.elapsed());
  QVERIFY2(model.isValid(), qPrintable(err));
  int knots = 0;
  for (const auto &w : model.wells())
    knots += w.knots.size();
  qInfo("BASELINE td_real_model_build_ms = %.0f (%d wells, %d knots)",
        buildMs, model.wells().size(), knots);

  // ---- 层位面转换吞吐（全部 8 个真实层位） ----
  const auto horizonFiles = QDir(area + QStringLiteral("/层位"))
                                .entryInfoList({QStringLiteral("*.dat")}, QDir::Files, QDir::Name);
  QVERIFY(horizonFiles.size() >= 8);
  double totalConvertMs = 0.0;
  qint64 totalCells = 0;
  int accounting = 0;
  for (const auto &hf : horizonFiles)
  {
    QFile f(hf.absoluteFilePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    BinnedHorizon binned;
    QVERIFY2(binHorizon(f.readAll(), &binned, &err), qPrintable(err));
    double gt[6] = {binned.originX, binned.dx, 0.0, binned.originY, 0.0, -binned.dy};
    const double convertMs = bestOf3([&] {
      QElapsedTimer t;
      t.start();
      const paleo::velmodel::DepthGridResult r = paleo::velmodel::convertTimeGridToDepth(
          model, binned.z, binned.rows, binned.cols, gt, -9999.0f);
      accounting = r.convertedCells + r.outsideModelCells + r.nodataCells;
      return double(t.elapsed());
    });
    QCOMPARE(accounting, binned.rows * binned.cols);
    totalConvertMs += convertMs;
    totalCells += qint64(binned.rows) * binned.cols;
    qInfo("BASELINE td_real_horizon_%s = %.1fms (%dx%d, %.2f Mcells/s)",
          hf.completeBaseName().toUtf8().constData(), convertMs,
          binned.cols, binned.rows, binned.rows * binned.cols / convertMs / 1000.0);
  }
  qInfo("BASELINE td_real_horizons_total_ms = %.0f (%lld cells)", totalConvertMs, totalCells);

  // ---- 966MB 体 IL 切片逐样深度换算（剖面深度标尺/深度域显示口径） ----
  const QString realSgy = area + QStringLiteral("/地震体/200P_seismic.sgy");
  if (QFile::exists(realSgy))
  {
    auto vol = std::make_shared<seismic::SgyVolume>();
    std::string sgyErr;
    clock.restart();
    QVERIFY2(vol->Load(realSgy.toStdString(), sgyErr),
             qPrintable(QString::fromStdString(sgyErr)));
    qInfo("BASELINE td_real_volume_open_ms = %.0f", double(clock.elapsed()));

    const auto &ils = vol->InlineValues();
    QVERIFY(!ils.empty());
    const int midInline = ils[ils.size() / 2];
    seismic::SgySliceImage img;
    clock.restart();
    QVERIFY(vol->ExtractSlice(seismic::SgySliceType::Inline, midInline, img, sgyErr));
    qInfo("BASELINE td_real_il_extract_ms = %.0f (%d traces x %d samples)",
          double(clock.elapsed()), img.width, img.height);

    // 切线坐标：层位头 P1/P2/P3 仿射把 (inline=mid, xline) 映到地图 XY——
    // 与层位/井位同一坐标架（切片不带道坐标，该近似的取舍记录在 progress 文档）。
    QFile hz(area + QStringLiteral("/层位/D61.dat"));
    QVERIFY(hz.open(QIODevice::ReadOnly));
    HorizonHeader header;
    QVERIFY(parseHorizonHeader(hz.readAll(), &header, &err));
    const SurveyGridGeometry geom = SurveyGridGeometry::fromHorizonHeader(header);
    QVERIFY(geom.valid);
    QVERIFY(geom.inlineMin <= midInline && midInline <= geom.inlineMax);

    const int ns = vol->SampleCount();
    const double dtMs = vol->SampleIntervalUs() / 1000.0;
    clock.restart();
    int finite = 0;
    std::vector<double> depth(std::size_t(img.width) * ns);
    for (int col = 0; col < img.width; ++col)
    {
      const double xline = geom.xlineMin + col *
          double(geom.xlineMax - geom.xlineMin) / qMax(img.width - 1, 1);
      double x = 0.0, y = 0.0;
      geom.inlineXlineToXy(midInline, xline, &x, &y);
      for (int s = 0; s < ns; ++s)
      {
        const double d = model.depthForTwt(x, y, s * dtMs);
        depth[std::size_t(col) * ns + s] = d;
        if (std::isfinite(d))
          ++finite;
      }
    }
    const double axisMs = double(clock.elapsed());
    qInfo("BASELINE td_real_section_axis_ms = %.0f (%d samples, %.1f%% in-model)",
          axisMs, img.width * ns, 100.0 * finite / (img.width * ns));
    QVERIFY(finite > 0);
    QVERIFY(axisMs < 5000.0); // 深度标尺换算不得进入秒级（防数量级回归）
  }

  QCOMPARE(listing(), before); // 只读契约
}

QTEST_MAIN(TestTimeDepthPerf)
#include "tst_timedepthperf.moc"
