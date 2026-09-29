#include <QTest>

#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/depthtransform.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

#include <cmath>
#include <cstdlib>

using namespace WellComposite;

// wave/wellcomposite-deep — D7.6/D7.7/D8.6 性能基准测试
//
// 机器可读结果：每项基准输出一行 "PERF-BENCH <name> <value> <unit>"，
// 供 CI/脚本抓取（D8.6）；预算断言宽松（共享机负载抖动），超预算 3 倍才红。
class TestWellCompositePerf : public QObject
{
  Q_OBJECT

private slots:
  // ---- D7.6：20 道 × 15k 点渲染预算（<16ms/帧目标）----
  void testRenderBudget20Tracks()
  {
    WellCompositeCanvas canvas;
    canvas.resize(1200, 800);
    canvas.show();
    QApplication::processEvents();
    canvas.setDepthRange(1000.0, 2500.0);
    canvas.setScaleRatio(QStringLiteral("自适应"));

    // 20 道：1 标尺 + 19 曲线道（每道 4 根 × ~197 点 ≈ 15k 总点）
    canvas.addTrack(std::make_shared<DepthScaleTrack>(64.0));
    const int pointsPerCurve = 800;
    for (int t = 0; t < 19; ++t)
    {
      auto track = std::make_shared<CurveTrack>(
          QStringLiteral("道%1").arg(t + 1), 120.0);
      for (int c = 0; c < 4; ++c)
      {
        CurveData cd;
        cd.name = QStringLiteral("C%1").arg(c);
        cd.minScale = 0.0f;
        cd.maxScale = 100.0f;
        cd.depths.reserve(pointsPerCurve);
        cd.values.reserve(pointsPerCurve);
        for (int i = 0; i < pointsPerCurve; ++i)
        {
          cd.depths.append(1000.0f + i * 1.875f);
          cd.values.append(50.0f + 45.0f * std::sin(i * 0.05 + c));
        }
        track->addCurve(cd);
      }
      canvas.addTrack(track);
    }
    QCOMPARE(canvas.trackCount(), 20);
    QApplication::processEvents();

    // 预热 + 计时（3 帧取最优）
    WellCompositeBody *body = canvas.findChild<WellCompositeBody *>();
    QVERIFY(body);
    qint64 bestMs = std::numeric_limits<qint64>::max();
    for (int frame = 0; frame < 3; ++frame)
    {
      QImage img(1200, 800, QImage::Format_ARGB32_Premultiplied);
      QPainter p(&img);
      const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
      body->render(&p); // offscreen 直接驱动 paintEvent 等价路径
      const qint64 ms = QDateTime::currentMSecsSinceEpoch() - t0;
      p.end();
      bestMs = std::min(bestMs, ms);
    }

    printf("PERF-BENCH render_20tracks_15k_pts %lld ms\n", static_cast<long long>(bestMs));
    // 预算 16ms/帧；共享机抖动容忍 3 倍
    QVERIFY2(bestMs < 48, qPrintable(QStringLiteral("渲染 %1ms 超预算(48ms 宽容线)").arg(bestMs)));
  }

  // ---- D6.6 LOD 抽稀：点数压缩与极值保真 ----
  void testLodDecimation()
  {
    QVector<float> depths;
    QVector<float> values;
    const int n = 15000;
    for (int i = 0; i < n; ++i)
    {
      depths.append(static_cast<float>(i));
      values.append(static_cast<float>(50.0 + 45.0 * std::sin(i * 0.01)));
    }

    const auto decimated = DepthTransform::decimateForLod(depths, values, 600);
    printf("PERF-BENCH lod_decimate_15000_to %d points\n",
           static_cast<int>(decimated.size()));
    QVERIFY(decimated.size() <= 600 * 3 + 8); // 桶保护点（首末+极值）
    QVERIFY(decimated.size() >= 2);

    // 极值保真：全局 min/max 必须在抽稀结果中
    float vmin = values.first(), vmax = values.first();
    for (float v : values)
    {
      vmin = std::min(vmin, v);
      vmax = std::max(vmax, v);
    }
    bool hasMin = false, hasMax = false;
    for (const auto &p : decimated)
    {
      if (std::abs(p.second - vmin) < 1e-4f) hasMin = true;
      if (std::abs(p.second - vmax) < 1e-4f) hasMax = true;
    }
    QVERIFY(hasMin);
    QVERIFY(hasMax);

    // 深度单调（视觉不回跳）
    for (int i = 1; i < decimated.size(); ++i)
      QVERIFY(decimated.at(i).first >= decimated.at(i - 1).first);

    // 小数据不抽
    QVERIFY(DepthTransform::shouldDecimate(15000, 600));
    QVERIFY(!DepthTransform::shouldDecimate(100, 600));
    const auto same = DepthTransform::decimateForLod({1.0f, 2.0f}, {5.0f, 6.0f}, 600);
    QCOMPARE(same.size(), 2);
  }

  // ---- D6.x 深度变换正确性 + 吞吐 ----
  void testDepthTransformMath()
  {
    DepthTransform dt;

    // 缺表：禁用原因 + MD≡TVD
    QVERIFY(!dt.hasDeviationSurvey());
    QVERIFY(!dt.deviationUnavailableReason().isEmpty());
    QCOMPARE(dt.mdToTvd(1234.0), 1234.0);

    // 井斜表：0°→90°（最小曲率）
    QVector<DeviationStation> stations;
    for (int i = 0; i <= 10; ++i)
      stations.append({1000.0 + i * 100.0, i * 9.0, 90.0}); // 每 100m 增 9°
    dt.setDeviationSurvey(stations);
    QVERIFY(dt.hasDeviationSurvey());
    QVERIFY(dt.deviationUnavailableReason().isEmpty());

    // 垂直段 TVD=MD
    QCOMPARE(dt.mdToTvd(1000.0), 1000.0);
    // 单调：TVD 增量 < MD 增量（有井斜后）
    const double tvd1500 = dt.mdToTvd(1500.0);
    QVERIFY(tvd1500 < 1500.0);
    QVERIFY(tvd1500 > 1300.0); // 45° 附近 cos ≈ 0.7 → ~1350
    // 反变换往返
    const double mdBack = dt.tvdToMd(tvd1500);
    QVERIFY(std::abs(mdBack - 1500.0) < 0.5);

    // KB 海拔（D6.2）：TVDSS = KB − TVD
    dt.setKbElevation(25.0);
    QVERIFY(dt.hasKbElevation());
    QCOMPARE(dt.mdToTvdss(1000.0), 25.0 - 1000.0);

    // TWT（D6.5）
    QVERIFY(!dt.hasTimeDepthTable());
    QVERIFY(!dt.twtUnavailableReason().isEmpty());
    dt.setTimeDepthTable({{1000.0, 850.0}, {2000.0, 1450.0}});
    QVERIFY(dt.hasTimeDepthTable());
    QCOMPARE(dt.twtAtTvd(1500.0), 1150.0); // 线性中点
    QCOMPARE(dt.twtAtTvd(500.0), 850.0);   // 外推夹界内值
  }

  void testDepthSampling()
  {
    CurveData c;
    c.name = QStringLiteral("GR");
    for (int i = 0; i < 100; ++i)
    {
      c.depths.append(1000.0f + i * 0.5f);
      c.values.append(i % 2 ? 10.0f : 90.0f);
    }

    // D6.4 单点取样（最近样本）
    const auto s = DepthTransform::sampleAt(c, 1024.9);
    QVERIFY(s.valid);
    QCOMPARE(s.value, 90.0f); // depth 1025 → index 50 → 偶 → 90

    // 区间等步长取样
    const auto list = DepthTransform::sampleRange(c, 1000.0, 1010.0, 2.5);
    QCOMPARE(list.size(), 5); // 1000,1002.5,...,1010
    QVERIFY(list.first().valid);

    // 吞吐（15k 点二分 × 1k 次 < 50ms 量级——宽松断言）
    CurveData big;
    for (int i = 0; i < 15000; ++i)
    {
      big.depths.append(static_cast<float>(1000 + i * 0.125));
      big.values.append(static_cast<float>(std::sin(i * 0.01) * 50 + 50));
    }
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 1000; ++i)
      DepthTransform::sampleAt(big, 1000.0 + (i % 1800));
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - t0;
    printf("PERF-BENCH sample_1000_of_15000 %lld ms\n", static_cast<long long>(ms));
    QVERIFY(ms < 500);
  }

  // ---- D7.7 内存预算：10 井对比会话 ----
  void testMemoryBudget10Wells()
  {
    // 构造 10 井 × 2 曲线 × 15k 点（重数据面），测进程 RSS 增量 < 400MB
    const qint64 rssBefore = currentRssKb();

    QVector<ComprehensiveWellData> wells;
    for (int w = 0; w < 10; ++w)
    {
      ComprehensiveWellData d;
      d.wellName = QStringLiteral("MW-%1").arg(w);
      d.minDepth = 1000.0;
      d.maxDepth = 2875.0;
      for (int c = 0; c < 2; ++c)
      {
        CurveData cd;
        cd.name = QStringLiteral("C%1").arg(c);
        cd.depths.reserve(15000);
        cd.values.reserve(15000);
        for (int i = 0; i < 15000; ++i)
        {
          cd.depths.append(1000.0f + i * 0.125f);
          cd.values.append(static_cast<float>(std::sin(i * 0.01 + w + c) * 50 + 50));
        }
        d.continuousCurves << cd;
      }
      wells << d;
    }
    // 渲染器快照（shared_ptr 井道集）再持一份引用面
    QList<std::shared_ptr<CurveTrack>> tracks;
    for (const auto &d : wells)
    {
      for (const auto &c : d.continuousCurves)
      {
        auto t = std::make_shared<CurveTrack>(c.name, 120.0);
        t->addCurve(c);
        tracks << t;
      }
    }

    const qint64 rssAfter = currentRssKb();
    const double deltaMb = (rssAfter - rssBefore) / 1024.0;
    printf("PERF-BENCH memory_10wells_15k x2curves %.1f MB\n", deltaMb);

    // 数据体本身 10×2×15000×2×4B ≈ 2.4MB×2（副本）——预算极宽松
    QVERIFY2(deltaMb < 400.0, qPrintable(QStringLiteral("10 井会话增量 %1 MB 超预算").arg(deltaMb)));
    QVERIFY(tracks.size() == 20);
  }

private:
  static qint64 currentRssKb()
  {
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
      return 0;
    const QByteArray all = f.readAll();
    for (const QByteArray &line : all.split('\n'))
    {
      if (line.startsWith("VmRSS:"))
      {
        const QByteArray num = line.mid(6).trimmed();
        const int sp = num.indexOf(' ');
        return num.left(sp > 0 ? sp : num.size()).toLongLong();
      }
    }
    return 0;
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositePerf tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_perf.moc"
