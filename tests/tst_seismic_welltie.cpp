// 层：测试壳
// P5 Phase 5 井震与任意线测试（D5.1–D5.7）：任意线编辑器路径提取、
// 提取缓存命中、井轨迹投影（顶/底投影）、合成记录（AC+DEN→RC→Ricker；
// 缺曲线/缺时深的降级原因）、多井最近 N 过滤、井旁道数据路径。
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <cstring>
#include <limits>

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismicsection/seismicsectioncanvas.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

namespace {

bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns, float base = 0.0f)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(QByteArray(3200, ' '));
  QByteArray binHdr(400, 0);
  const auto put16 = [&](QByteArray &buf, int at, qint16 v) {
    buf[at] = char(quint8(v >> 8));
    buf[at + 1] = char(quint8(v));
  };
  const auto put32 = [&](QByteArray &buf, int at, qint32 v) {
    buf[at] = char(quint8(v >> 24));
    buf[at + 1] = char(quint8(v >> 16));
    buf[at + 2] = char(quint8(v >> 8));
    buf[at + 3] = char(quint8(v));
  };
  put16(binHdr, 12, qint16(xlines));
  put16(binHdr, 16, 2000);
  put16(binHdr, 20, qint16(ns));
  put16(binHdr, 24, 5);
  file.write(binHdr);
  for (int i = 0; i < inlines; ++i)
    for (int j = 0; j < xlines; ++j)
    {
      QByteArray trHdr(240, 0);
      put32(trHdr, 0, i * xlines + j + 1);
      put32(trHdr, 188, 1000 + i);
      put32(trHdr, 192, 2000 + j);
      put16(trHdr, 114, qint16(ns));
      file.write(trHdr);
      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        const float val = base + float((i + 1) * 100 + j) + k * 0.25f;
        quint32 bits;
        std::memcpy(&bits, &val, 4);
        bits = qToBigEndian(bits);
        std::memcpy(samples.data() + k * 4, &bits, 4);
      }
      file.write(samples);
    }
  file.close();
  return QFileInfo(filePath).size() > 3600;
}

QImage renderCanvas(SeismicSectionCanvas &canvas)
{
  QImage img(canvas.size(), QImage::Format_ARGB32);
  img.fill(Qt::white);
  QPainter painter(&img);
  canvas.render(&painter);
  painter.end();
  return img;
}

qint64 imageDiff(const QImage &a, const QImage &b)
{
  if (a.size() != b.size())
    return -1;
  qint64 diff = 0;
  for (int y = 0; y < a.height(); ++y)
  {
    const auto *ra = reinterpret_cast<const QRgb *>(a.scanLine(y));
    const auto *rb = reinterpret_cast<const QRgb *>(b.scanLine(y));
    for (int x = 0; x < a.width(); ++x)
      diff += (ra[x] != rb[x]) ? 1 : 0;
  }
  return diff;
}

} // namespace

class TestSeismicWellTie : public QObject
{
  Q_OBJECT

private slots:
  // ---- D5.4 合成记录：AC+DEN → 波阻抗 → 反射系数 → Ricker ----
  void syntheticSeismogram()
  {
    // 两层阻抗（1000-2000m AC 240μs/m ρ2.3；2000-3000m AC 200μs/m ρ2.5）
    const std::vector<double> depths = {1000, 1100, 1200, 1300, 1400, 1500,
                                        1600, 1700, 1800, 1900, 2000,
                                        2100, 2200, 2300, 2400, 2500, 2600,
                                        2700, 2800, 2900, 3000};
    std::vector<float> ac(depths.size()), den(depths.size());
    for (std::size_t i = 0; i < depths.size(); ++i)
    {
      ac[i] = depths[i] < 2000.0 ? 240.0f : 200.0f;
      den[i] = depths[i] < 2000.0 ? 2.30f : 2.50f;
    }
    TimeDepthModel td(2500.0); // 1000m→800ms, 2000m→1600ms, 3000m→2400ms

    const auto result = SeismicTaskService::computeSyntheticSeismogram(
        depths, ac, depths, den, td, 25.0);
    QVERIFY2(result.ok, qPrintable(result.reason));
    QVERIFY(result.sampleCount > 100);
    // 反射界面在 2000m → TWT 1600ms 附近出现强振幅
    int peakIdx = -1;
    float peakAbs = 0;
    for (std::size_t i = 0; i < result.amplitude.size(); ++i)
      if (std::abs(result.amplitude[i]) > peakAbs)
      {
        peakAbs = std::abs(result.amplitude[i]);
        peakIdx = int(i);
      }
    QVERIFY(peakIdx >= 0);
    // 反射界面在采样序列的 1900→2000m 区间（中点 1950m → TWT 1560ms）——
    // 零相位 Ricker 峰值应落在界面中点时间上
    QVERIFY(std::abs(result.twtMs[std::size_t(peakIdx)] - 1560.0) < 20.0);

    // 降级 1：缺密度
    const auto noDen = SeismicTaskService::computeSyntheticSeismogram(
        depths, ac, {}, {}, td);
    QVERIFY(!noDen.ok);
    QVERIFY(noDen.reason.contains(QStringLiteral("密度")));

    // 检查点表模型（实测时深）同路径可用；无效模型由构造器防御回退均速，
    // isValid 恒真——缺表降级在 UI 层按 hasCheckshots 注记（D5.3 原因态）
    TimeDepthModel checkshots;
    checkshots.setPoints({{0.0, 0.0}, {1000.0, 800.0}, {2000.0, 1600.0}, {3000.0, 2400.0}});
    const auto cs = SeismicTaskService::computeSyntheticSeismogram(
        depths, ac, depths, den, checkshots);
    QVERIFY2(cs.ok, qPrintable(cs.reason));
    QVERIFY(checkshots.hasCheckshots());
  }

  // ---- D5.2 任意线提取缓存：同路径第二次命中 ----
  void sectionCacheHit()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("cache.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 6, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    const std::vector<glm::ivec2> path = {{1000, 2000}, {1002, 2003}, {1004, 2004}};

    int finished = 0;
    std::shared_ptr<const SgySliceImage> first, second;
    QElapsedTimer clock;
    clock.start();
    svc.startSectionExtraction(volume, path, SgySectionOptions{},
        [&](bool ok, std::shared_ptr<const SgySliceImage> img, const SgySectionStats &, const QString &) {
          if (ok) { first = img; ++finished; }
        });
    QTRY_COMPARE_WITH_TIMEOUT(finished, 1, 30000);
    const qint64 firstMs = clock.elapsed();
    QVERIFY(first != nullptr);
    QVERIFY(first->width > 1);

    clock.restart();
    int secondFinished = 0;
    svc.startSectionExtraction(volume, path, SgySectionOptions{},
        [&](bool ok, std::shared_ptr<const SgySliceImage> img, const SgySectionStats &, const QString &) {
          if (ok) { second = img; ++secondFinished; }
        });
    QTRY_COMPARE_WITH_TIMEOUT(secondFinished, 1, 5000);
    const qint64 secondMs = clock.elapsed();
    QVERIFY(second != nullptr);
    QCOMPARE(second->width, first->width);
    QCOMPARE(second->values, first->values);
    qInfo("section cache: first=%lldms second=%lldms", firstMs, secondMs);
    QVERIFY2(secondMs <= firstMs + 3,
             qPrintable(QStringLiteral("cache hit %1 not faster than %2").arg(secondMs).arg(firstMs)));
    // RUNTIME-04 钉死：缓存命中的单发 QTimer 必须在事件循环回收后清干净
    // （deleteLater 的延迟删除由 QTRY 的循环处理），不得残留子对象。
    QVERIFY(svc.findChildren<QTimer *>().isEmpty());
  }

  // ---- #289 缓存键隔离：同尺寸不同体、不同提取选项互不命中 ----
  // 旧键只混 fileSize+路径节点：同尺寸不同振幅的两个体拖同一条线会返回
  // 先缓存体的剖面；命中时 stats 被合成为 c*25m 假道距。键含体身份+选项后
  // 必须各自 miss/各自命中，且命中 stats 与首次提取逐元素相等。
  void sectionCacheKeyIsolation()
  {
    QTemporaryDir dir;
    // 同尺寸不同振幅：尺寸由布局决定（逐字节等长），base 错开振幅
    const QString sgyA = dir.filePath("vol_a.sgy");
    const QString sgyB = dir.filePath("vol_b.sgy");
    QVERIFY(writeTestSegy(sgyA, 6, 6, 64));
    QVERIFY(writeTestSegy(sgyB, 6, 6, 64, 10000.0f));
    QVERIFY(QFileInfo(sgyA).size() == QFileInfo(sgyB).size());

    auto volumeA = std::make_shared<SgyVolume>();
    auto volumeB = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volumeA->Load(sgyA.toStdString(), err));
    QVERIFY(volumeB->Load(sgyB.toStdString(), err));

    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    const std::vector<glm::ivec2> path = {{1000, 2000}, {1002, 2003}, {1004, 2004}};

    auto extract = [&](std::shared_ptr<SgyVolume> vol, const SgySectionOptions &opt,
                       std::shared_ptr<const SgySliceImage> &img, SgySectionStats &stats) {
      int done = 0;
      svc.startSectionExtraction(vol, path, opt,
          [&](bool ok, std::shared_ptr<const SgySliceImage> out, const SgySectionStats &s, const QString &) {
            if (ok) { img = out; stats = s; ++done; }
          });
      QTRY_COMPARE_WITH_TIMEOUT(done, 1, 30000);
    };

    std::shared_ptr<const SgySliceImage> imgA, imgB, imgA2;
    SgySectionStats statsA, statsB, statsA2;
    extract(volumeA, SgySectionOptions{}, imgA, statsA);
    QVERIFY(imgA != nullptr);
    QVERIFY(!imgA->values.empty());
    // 真实道距必须存在（命中路径不再合成 25m 近似）
    QCOMPARE(statsA.columnDistances.size(), std::size_t(imgA->width));

    // 体 B 同路径：键含体路径，不得命中体 A 的缓存
    extract(volumeB, SgySectionOptions{}, imgB, statsB);
    QVERIFY(imgB != nullptr);
    QCOMPARE(imgB->values.size(), imgA->values.size());
    QVERIFY2(imgB->values != imgA->values,
             "同尺寸不同体的剖面互相命中缓存——振幅必须不同");

    // 体 A 再来一次：必须命中缓存，图像与 stats 与首次逐元素相等
    extract(volumeA, SgySectionOptions{}, imgA2, statsA2);
    QCOMPARE(imgA2->values, imgA->values);
    QCOMPARE(statsA2.columnDistances, statsA.columnDistances);
    QCOMPARE(statsA2.columnDistances.size(), statsA.columnDistances.size());
    QVERIFY(!statsA2.columnDistances.empty());

    // 不同 maxColumns：键含提取选项，不得互相命中
    std::shared_ptr<const SgySliceImage> imgCoarse;
    SgySectionStats statsCoarse;
    SgySectionOptions coarse;
    coarse.maxColumns = 2;
    extract(volumeB, coarse, imgCoarse, statsCoarse);
    QVERIFY(imgCoarse != nullptr);
    QVERIFY2(imgCoarse->width != imgB->width,
             "不同 maxColumns 互相命中缓存——列数必须不同");
  }

  // ---- D5.3 井轨迹投影：顶/底到剖面折线的独立投影 ----
  void wellTrajectoryProjection()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    SgySliceImage img;
    img.width = 64;
    img.height = 256;
    img.valueMin = -1;
    img.valueMax = 1;
    img.values.assign(std::size_t(64) * 256, 0.0f);
    img.rgba.assign(std::size_t(64) * 256 * 4, 255);
    canvas.setSectionData(img, 2.0f);

    std::vector<SeismicSectionCanvas::WellTrajectory> traj;
    SeismicSectionCanvas::WellTrajectory t;
    t.wellId = QStringLiteral("W1");
    t.topTracePos = 10.0;      // 顶在道 10
    t.bottomTracePos = 22.0;   // 底漂到道 22（斜井）
    t.bottomTwtMs = 400.0;
    traj.push_back(t);
    canvas.setWellTrajectories(traj);
    QCOMPARE(canvas.wellTrajectories().size(), std::size_t(1));
    QCOMPARE(canvas.wellTrajectories().front().bottomTracePos, 22.0);
  }

  // ---- goal/time-depth-velocity：深度标尺反投影（非常速模型贴准） ----
  // 常速模型深度刻度线性；校验炮分段模型同一深度刻度反解 TWT 的像素位置
  // 不同——两幅深度模式渲染必须可区分（修前左缘深度刻度按常速线性近似，
  // 两模型渲染相同）。悬停深度也走同一模型口径。
  void depthRulerBackProjection()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    SgySliceImage img;
    img.width = 64;
    img.height = 256;
    img.valueMin = -1;
    img.valueMax = 1;
    img.values.assign(std::size_t(64) * 256, 0.0f);
    img.rgba.assign(std::size_t(64) * 256 * 4, 255);
    canvas.setSectionData(img, 4.0f); // 0..1020ms
    canvas.setVerticalUnit(SectionVerticalUnit::DepthMeters);

    canvas.setTimeDepthModel(seismic::TimeDepthModel(2500.0)); // 常速
    const QImage linear = renderCanvas(canvas);
    QVERIFY(!linear.isNull());

    seismic::TimeDepthModel piecewise;
    QVERIFY(piecewise.setCheckshots({{0.0, 0.0},
                                     {500.0, 750.0},   // 上段 Vint=3000
                                     {1020.0, 1705.0}})); // 下段 Vint≈2625
    canvas.setTimeDepthModel(piecewise);
    const QImage nonlinear = renderCanvas(canvas);
    QVERIFY(!nonlinear.isNull());
    QVERIFY(imageDiff(linear, nonlinear) > 50); // 刻度位置变化可见

    // 时间模式渲染不受模型影响切换面仍在：切回时间轴可渲染（信号面冒烟）。
    canvas.setVerticalUnit(SectionVerticalUnit::TwoWayTimeMs);
    QVERIFY(!renderCanvas(canvas).isNull());
  }

  // ---- D5.7 多井开关：最近 N 过滤渲染差异 ----
  void multiWellToggle()
  {
    SeismicSectionCanvas canvas;
    canvas.resize(800, 600);
    SgySliceImage img;
    img.width = 64;
    img.height = 256;
    img.valueMin = -1;
    img.valueMax = 1;
    img.values.assign(std::size_t(64) * 256, 0.0f);
    img.rgba.assign(std::size_t(64) * 256 * 4, 255);
    canvas.setSectionData(img, 2.0f);

    std::vector<SectionWellInfo> wells(2);
    wells[0].wellId = QStringLiteral("W1");
    wells[0].wellName = QStringLiteral("近井");
    wells[0].tracePosition = 10.0;
    wells[0].offsetDistanceM = 50.0;
    wells[0].isWithinBuffer = true;
    wells[1].wellId = QStringLiteral("W2");
    wells[1].wellName = QStringLiteral("远井");
    wells[1].tracePosition = 40.0;
    wells[1].offsetDistanceM = 400.0;
    wells[1].isWithinBuffer = true;
    canvas.setWells(wells);

    const QImage all = renderCanvas(canvas);
    canvas.setMaxVisibleWells(1); // 只留最近 1 口（W1 近井）
    const QImage one = renderCanvas(canvas);
    QVERIFY(imageDiff(all, one) > 50); // 第二口井消失
    QCOMPARE(canvas.maxVisibleWells(), 1);
  }

  // ---- D5.1 任意线：多段折线经 dock 提取（编辑器的数据路径）----
  void arbitraryLineExtraction()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("arb.sgy");
    QVERIFY(writeTestSegy(sgy, 8, 8, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string err;
    QVERIFY(volume->Load(sgy.toStdString(), err));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.extractSectionFromVolumeAsync(
        volume, {{1000, 2000}, {1003, 2004}, {1006, 2005}},
        QStringLiteral("编辑器任意线"), {}, {});
    QVERIFY(finishedSpy.wait(30000));
    QVERIFY(dock.canvas()->hasData());
    QVERIFY(dock.canvas()->traceCount() > 1);
    QVERIFY(dock.candidateWells().empty());

    // #225：任意线入参候选井必须写入成员——井旁道/子波/反演低频井/井轨迹
    // 全读 m_candidateWells，旧实现成员恒空，生产链路恒死。
    SectionWellInfo well;
    well.wellId = QStringLiteral("W225");
    well.wellName = QStringLiteral("候选井");
    well.surfaceX = 1003.0;
    well.surfaceY = 2004.0;
    well.totalDepth = 1000.0;
    dock.extractSectionFromVolumeAsync(
        volume, {{1000, 2000}, {1003, 2004}, {1006, 2005}},
        QStringLiteral("带井任意线"), {{1000.0, 2000.0}, {1006.0, 2005.0}}, {well});
    QVERIFY(finishedSpy.wait(30000));
    QCOMPARE(dock.candidateWells().size(), std::size_t{1});
    QCOMPARE(dock.candidateWells().front().wellId, QStringLiteral("W225"));
  }
};

QTEST_MAIN(TestSeismicWellTie)
#include "tst_seismic_welltie.moc"
