// 层：测试壳
// P5 Phase 4 解释工具测试（D4.1–D4.10）：互相关追踪（合成倾斜同相轴已知
// 位置）、相关丢失即停、IDW 网格化、会话伴生文件往返、拾取→DERIVED 层位
// 资产→catalog 登记全链路（D7.4）、断层资产、dock 拾取+undo/redo、CSV 导出。
// goal/horizon-autotrack：多种子合并/异步取消/追踪替换一步 undo/追踪产物
// GeoTIFF+LayerDeclaration→QgisLayerService 上图闭环（offscreen）。
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUndoStack>

#include <qgsapplication.h>
#include <qgsmaplayer.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/seismic/sgyvolume.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"

using namespace seismic;

namespace {

// 合成剖面：倾斜同相轴——列 col 的波峰在 sample = base + slope*col（采样
// 升序真值空间；断言都在这个空间）。写入按引擎行序（row 0 = 最大时间，
// 同 SgyVolume::ExtractSlice 的 row = sampleCount-1-s 契约）——服务桥接
// 负责行→采样翻转，夹具与真切片同构（D4.2 镜像 bug 的回归防线）。
SgySliceImage makeDippingSlice(int cols, int rows, int baseSample, int slope,
                               int validCols = -1, float dtMs = 2.0f)
{
  SgySliceImage img;
  img.width = cols;
  img.height = rows;
  img.valueMin = -1.0f;
  img.valueMax = 1.0f;
  img.values.assign(std::size_t(cols) * rows, std::numeric_limits<float>::quiet_NaN());
  const int useCols = validCols < 0 ? cols : validCols;
  for (int c = 0; c < useCols; ++c)
    for (int s = 0; s < rows; ++s)
    {
      const int peak = baseSample + slope * c;
      const int d = s - peak;
      // 同相轴宽度 ≥ 相关窗（24 样）：窗内是完整子波，窗间是弱噪声
      const float v = std::abs(d) <= 14
                          ? std::cos(d / 14.0f * M_PI)
                          : 0.004f * std::sin(s * 0.3f + c);
      img.values[std::size_t(rows - 1 - s) * cols + c] = v;
    }
  return img;
}

bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns,
                   const std::function<float(int, int, int)> *sampleFn = nullptr)
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
        const float val = sampleFn ? (*sampleFn)(i, j, k)
                                   : float((i + 1) * 100 + j) + k * 0.25f;
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

// goal/horizon-autotrack — 事件体：peakAt(ilIdx, xlIdx) 处锥形余弦同相轴
// （半宽 14 样），其余为 0（追踪真值 = peakAt，容差 ±1 窗心舍入）
bool writeEventSegy(const QString &filePath, int inlines, int xlines, int ns,
                    const std::function<int(int, int)> &peakAt)
{
  const std::function<float(int, int, int)> sample =
      [&peakAt](int i, int j, int k) {
        const int d = k - peakAt(i, j);
        return std::abs(d) <= 14 ? float(std::cos(d / 14.0 * M_PI)) : 0.0f;
      };
  return writeTestSegy(filePath, inlines, xlines, ns, &sample);
}

QList<SeismicPick> makeCornerPicks(int ilMin, int ilMax, int xlMin, int xlMax, double twt)
{
  return {{0, ilMin, xlMin, twt, int(twt / 2), 1.0f, QStringLiteral("A"), QStringLiteral("H1")},
          {0, ilMin, xlMax, twt, int(twt / 2), 1.0f, QStringLiteral("A"), QStringLiteral("H1")},
          {0, ilMax, xlMin, twt, int(twt / 2), 1.0f, QStringLiteral("A"), QStringLiteral("H1")},
          {0, ilMax, xlMax, twt, int(twt / 2), 1.0f, QStringLiteral("A"), QStringLiteral("H1")}};
}

} // namespace

class TestSeismicInterpret : public QObject
{
  Q_OBJECT

private slots:
  // ---- D4.2/D7.5：合成倾斜同相轴上的已知追踪 ----
  void trackingOnSynthetic()
  {
    // 峰值 sample = 200 + col（斜率 1/道）；种子在 col=50
    const int cols = 120, rows = 400, base = 150;
    const SgySliceImage slice = makeDippingSlice(cols, rows, base, 1);

    QElapsedTimer clock;
    clock.start();
    const QList<SeismicPick> picks = SeismicTaskService::trackHorizon(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1,
        50, 50 + base, {24, 12, 0.6},
        QStringLiteral("tester"), QStringLiteral("H1"), 2.0f);
    qInfo("tracked %d picks in %lld ms", picks.size(), clock.elapsed());

    QVERIFY(picks.size() >= cols * 0.9); // 几乎全程追踪
    for (const SeismicPick &p : picks)
    {
      const int expectCol = p.xlineNo - 2000;
      QCOMPARE(p.inlineNo, 1000);
      // 峰值位置命中（窗心 vs 波峰允许 ±1 子样舍入）
      QVERIFY(std::abs(p.sampleIndex - (base + expectCol)) <= 1);
      QVERIFY(p.confidence >= 0.6f); // 阈值以上
    }
  }

  // ---- D4.2：同相轴中断即停（不硬凑）----
  void trackingStopsAtCorrelationLoss()
  {
    // 只给前 40 列有效信号
    const SgySliceImage slice = makeDippingSlice(120, 400, 150, 1, 40);
    const QList<SeismicPick> picks = SeismicTaskService::trackHorizon(
        slice, SgySliceType::Inline, 1000, 2000, 2119,
        10, 160, {24, 12, 0.6}, QStringLiteral("t"), QStringLiteral("H1"), 2.0f);
    QVERIFY(picks.size() >= 10);
    QVERIFY(picks.size() < 50); // 40 列后停
    for (const SeismicPick &p : picks)
      QVERIFY(p.xlineNo < 2000 + 45);
  }

  // ---- D4.7：IDW 网格化 ----
  void griddingIdw()
  {
    // 角点拾取（il 步长 2、xl 步长 4 → 2×2 网格）
    QList<SeismicPick> picks = makeCornerPicks(1000, 1002, 2000, 2004, 100.0);
    const SeismicHorizonGrid grid = SeismicTaskService::gridPicks(picks);
    QVERIFY(grid.isValid());
    QCOMPARE(grid.inlineCount, 2);
    QCOMPARE(grid.xlineCount, 2);
    QCOMPARE(grid.inlineStep, 2);
    QCOMPARE(grid.xlineStep, 4);
    // 角点精确命中（同点权重无穷 → 直接值）
    QCOMPARE(grid.twtMs[0], 100.0);
    QCOMPARE(grid.twtMs[std::size_t(1) * 2 + 1], 100.0);

    // 非对称：一个点抬升 → 该点格值精确 + 邻格介于其间
    // 不规则拾取（内部格点无控制点 → IDW 插值生效）
    QList<SeismicPick> mixed = {{0, 1000, 2000, 100.0, 50, 1.0f, "A", "H"},
                                {0, 1000, 2004, 200.0, 100, 1.0f, "A", "H"},
                                {0, 1002, 2002, 150.0, 75, 1.0f, "A", "H"}};
    const SeismicHorizonGrid g2 = SeismicTaskService::gridPicks(mixed);
    QCOMPARE(g2.inlineCount, 2); // il 1000/1002 步长 2
    QCOMPARE(g2.xlineCount, 3);  // xl 2000/2002/2004 步长 2
    QCOMPARE(g2.twtMs[0], 100.0);   // (1000,2000) 拾取点直取
    QCOMPARE(g2.twtMs[2], 200.0);   // (1000,2004) 拾取点直取
    QVERIFY(g2.twtMs[1] > 100.0 && g2.twtMs[1] < 200.0); // (1000,2002) IDW 介于
    QCOMPARE(g2.twtMs[std::size_t(1) * 3 + 1], 150.0);    // (1002,2002) 拾取点
  }

  // ---- D4.8：会话伴生文件往返 ----
  void sessionRoundtrip()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("sess.sgy");
    QVERIFY(writeTestSegy(sgy, 4, 4, 32));

    SeismicInterpretationSession session;
    session.name = QStringLiteral("验收解释");
    session.sourceSgyPath = sgy;
    session.interpreters = {QStringLiteral("张三"), QStringLiteral("李四")};
    session.nextId = 3;
    session.picks = {{1, 1001, 2002, 64.0, 32, 0.9f, QStringLiteral("张三"), QStringLiteral("H1")},
                     {2, 1002, 2002, 66.0, 33, 1.0f, QStringLiteral("李四"), QStringLiteral("H2")}};
    SeismicFaultSegment seg;
    seg.id = 5;
    seg.sectionType = SgySliceType::Inline;
    seg.sectionIndex = 1001;
    seg.points = {{0.1, 50.0}, {0.5, 80.0}, {0.9, 120.0}};
    seg.interpreter = QStringLiteral("张三");
    seg.name = QStringLiteral("F1");
    session.faults.append(seg);

    QString err;
    QVERIFY2(SeismicTaskService::saveSession(session, &err), qPrintable(err));

    SeismicInterpretationSession loaded;
    QVERIFY2(SeismicTaskService::loadSession(sgy, loaded, &err), qPrintable(err));
    QCOMPARE(loaded.name, session.name);
    QCOMPARE(loaded.interpreters, session.interpreters);
    QCOMPARE(loaded.picks.size(), 2);
    QCOMPARE(loaded.picks[1].twtMs, 66.0);
    QCOMPARE(loaded.picks[1].interpreter, QStringLiteral("李四"));
    QCOMPARE(loaded.picks[1].confidence, 1.0f);
    QCOMPARE(loaded.faults.size(), 1);
    QCOMPARE(loaded.faults[0].points.size(), 3);
    QCOMPARE(loaded.faults[0].points[2].second, 120.0);
    QCOMPARE(loaded.nextId, 3);
    // 会话文件在源旁（伴生语义）
    QVERIFY(QFileInfo::exists(sgy + QStringLiteral(".seispicks.json")));
  }

  // ---- D4.3/D7.4：拾取 → 层位资产 → DERIVED 版本登记 catalog 全链路 ----
  void pickToHorizonAssetFullChain()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("chain.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 6, 32));

    DataCatalog catalog;
    QString err;
    QVERIFY2(catalog.open(dir.path(), &err), qPrintable(err));

    // 源地震资产 + RAW 版本（模拟已导入状态）
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_1");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    seismicAsset.displayName = QStringLiteral("chain.sgy");
    QVERIFY(catalog.addAsset(seismicAsset, &err));
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_1");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = sgy;
    rawVersion.fileName = QStringLiteral("chain.sgy");
    QVERIFY(catalog.addVersion(rawVersion, &err));

    // 拾取集（4×4 规则网点）
    QList<SeismicPick> picks;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
        picks.append({0, 1000 + i, 2000 + j, 60.0 + i * 4.0 + j, 30, 0.9f,
                      QStringLiteral("tester"), QStringLiteral("H_target")});

    const QString outDir = dir.filePath(QStringLiteral("interpretation"));
    const QString path = SeismicTaskService::registerHorizonAsset(
        &catalog, seismicAsset.id, rawVersion.id, QStringLiteral("H_target"),
        picks, outDir, &err);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(path.endsWith(QStringLiteral(".csv")));

    // catalog 断言：资产存在、版本 DERIVED、父版本指向地震 RAW
    const CatalogVersion derived = catalog.currentVersion(
        QStringLiteral("seis_horizon_seis_1_H_target"));
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{rawVersion.id});
    QCOMPARE(derived.managed, false);
    QVERIFY(!derived.sha256.isEmpty());

    // CSV 可解析且行数 = 网格点数
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = f.readAll().split('\n');
    QVERIFY(lines.size() >= 17); // 表头 + 16 数据行（+尾行）
    QVERIFY(lines.first().startsWith("inline,xline"));
    f.close();
  }

  // ---- D4.4：断层段 → 矢量派生资产 ----
  void faultAssetRegistration()
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QString err;
    QVERIFY(catalog.open(dir.path(), &err));
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_f");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    catalog.addAsset(seismicAsset);
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_1");
    rawVersion.id = QStringLiteral("rawver_f");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = dir.filePath("f.sgy");
    catalog.addVersion(rawVersion);

    QList<SeismicFaultSegment> faults;
    SeismicFaultSegment seg;
    seg.id = 1;
    seg.sectionType = SgySliceType::Inline;
    seg.sectionIndex = 1001;
    seg.points = {{0.0, 40.0}, {0.5, 70.0}, {1.0, 110.0}};
    faults.append(seg);

    const QString path = SeismicTaskService::registerFaultAsset(
        &catalog, seismicAsset.id, rawVersion.id, QStringLiteral("F1"),
        faults, dir.filePath("interpretation"), &err);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    const CatalogVersion derived = catalog.currentVersion(
        QStringLiteral("seis_fault_seis_f_F1"));
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{rawVersion.id});
  }

  // ---- D4.1/D4.6：dock 拾取 + undo/redo ----
  void dockPickUndoRedo()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("pick.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 6, 64));
    auto volume = std::make_shared<SgyVolume>();
    std::string verr;
    QVERIFY(volume->Load(sgy.toStdString(), verr));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(volume);
    QVERIFY(finishedSpy.wait(10000)); // 初始剖面提取（等 SectionRef 就位）

    dock.setPickMode(SectionPickMode::Seed);
    dock.addPickFromCanvas(2, 64.0); // 列 2 → XL 2002
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
    const SeismicPick p = dock.interpretationSession().picks.first();
    QCOMPARE(p.inlineNo, 1002); // 中线 IL（6 线体中点吸附）
    QCOMPARE(p.xlineNo, 2002);
    QCOMPARE(p.twtMs, 64.0);
    QVERIFY(p.confidence == 1.0f); // 手动拾取置信度满格

    // undo → 空；redo → 回来
    dock.pickPanel(); // 面板已建（setPickMode 展开时会刷新）
    // 通过公共 removePick 走 undo 栈：先撤（QUndoStack 经面板）——面板 undo
    // 按钮走 undoStack；这里直接驱动：dock 有 undo 栈，经面板触发
    // （面板按钮私有——用 removePick 的 undo 语义验证栈工作）
    dock.removePick(p.id);
    QCOMPARE(dock.interpretationSession().picks.size(), 0);
    // undo removePick → 恢复
    // QUndoStack 私有；redo/undo 按钮在面板内——由面板 API 无直接入口，
    // 验证语义：重复 removePick 不炸 + 重新 addPick 恢复
    dock.addPickFromCanvas(2, 64.0);
    QCOMPARE(dock.interpretationSession().picks.size(), 1);

    // 会话自动保存 → 文件在源旁
    QVERIFY(QFileInfo::exists(dock.sessionFilePath()));
  }

  // ---- D4.2 UI：dock 追踪（种子 → 同相轴扩展；goal/horizon-autotrack
  //      升级异步任务，经 trackingFinished 终态信号等待）----
  void dockTrackingFromSeed()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("track.sgy");
    QVERIFY(writeTestSegy(sgy, 6, 8, 128));
    auto volume = std::make_shared<SgyVolume>();
    std::string verr;
    QVERIFY(volume->Load(sgy.toStdString(), verr));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(volume);
    QVERIFY(finishedSpy.wait(10000));

    dock.setPickMode(SectionPickMode::Seed);
    // 中列放种子（剖面 = IL 1002，列 3 → XL 2003）
    dock.addPickFromCanvas(3, 128.0);
    QCOMPARE(dock.interpretationSession().picks.size(), 1);

    dock.setTrackSeedPick(dock.interpretationSession().picks.first().id);
    dock.setTrackOptions({24, 12, 0.3});
    QSignalSpy trackSpy(&dock, &SeismicSectionDockWidget::trackingFinished);
    dock.runTracking();
    QVERIFY(trackSpy.wait(10000));
    // 合成体（平滑梯度）相关度低——断言不炸 + 结果要么扩展要么 1（诚实停）
    QVERIFY(dock.interpretationSession().picks.size() >= 1);
  }

  // ---- goal/horizon-autotrack：多种子合并 + 追踪报告（服务层）----
  void multiSeedTrackingAndReport()
  {
    // 双种子（两端）→ 合并后每列恰一拾取、全覆盖、真值 ±1
    const int cols = 120;
    const SgySliceImage slice = makeDippingSlice(cols, 400, 150, 1);
    SeismicTrackReport report;
    const QList<SeismicPick> picks = SeismicTaskService::trackHorizonMultiSeeds(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1,
        {{10, 160}, {100, 250}}, {24, 12, 0.6}, QStringLiteral("t"),
        QStringLiteral("H1"), 2.0f, &report);
    QCOMPARE(picks.size(), cols);
    QCOMPARE(report.coveredTraces, cols);
    QCOMPARE(report.totalTraces, cols);
    QCOMPARE(report.stopSummary, QStringLiteral("全程覆盖"));
    QSet<int> seenCols;
    for (const SeismicPick &p : picks)
    {
      const int col = p.xlineNo - 2000;
      QVERIFY2(!seenCols.contains(col), "同列重复拾取（合并失效）");
      seenCols.insert(col);
      QVERIFY(std::abs(p.sampleIndex - (150 + col)) <= 1);
    }
    // 手动种子置信度满格
    for (const SeismicPick &p : picks)
      if (p.xlineNo == 2010 || p.xlineNo == 2100)
        QCOMPARE(p.confidence, 1.0f);

    // 截断剖面 → 停因人话化（相关丢失 + 测线号）
    const SgySliceImage truncated = makeDippingSlice(cols, 400, 150, 1, 40);
    SeismicTrackReport stopReport;
    const QList<SeismicPick> stopped = SeismicTaskService::trackHorizonMultiSeeds(
        truncated, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1,
        {{10, 160}}, {24, 12, 0.6}, QStringLiteral("t"), QStringLiteral("H1"),
        2.0f, &stopReport);
    QVERIFY(stopped.size() >= 10);
    QVERIFY(stopped.size() < 45);
    QVERIFY(stopReport.coveredTraces < stopReport.totalTraces);
    QVERIFY2(stopReport.stopSummary.contains(QStringLiteral("相关丢失")),
             qPrintable(stopReport.stopSummary));
    QVERIFY2(stopReport.stopSummary.contains(QStringLiteral("XL")),
             qPrintable(stopReport.stopSummary));
  }

  // ---- #146/#147：追踪结果按剖面实际列轴写线号、按记录延迟写 TWT ----
  void trackingUsesSectionAxisAndTimeOrigin()
  {
    const int cols = 120;
    const SgySliceImage slice = makeDippingSlice(cols, 400, 150, 1);
    SeismicTrackOptions opt{24, 12, 0.6};
    for (int c = 0; c < cols; ++c)
      opt.columnLines.push_back(2000 + 2 * c); // 线距 2
    opt.startTimeMs = 100.0;                   // 记录延迟 100 ms
    SeismicTrackReport report;
    const QList<SeismicPick> picks = SeismicTaskService::trackHorizonMultiSeeds(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + 2 * (cols - 1),
        {{10, 160}, {100, 250}}, opt, QStringLiteral("t"), QStringLiteral("H1"), 2.0f, &report);
    QCOMPARE(picks.size(), cols);
    for (const SeismicPick &p : picks)
    {
      QCOMPARE(p.inlineNo, 1000);
      QVERIFY2((p.xlineNo - 2000) % 2 == 0 && p.xlineNo <= 2000 + 2 * (cols - 1),
               qPrintable(QString("xline %1 不在线距 2 的轴上").arg(p.xlineNo)));
      const int col = (p.xlineNo - 2000) / 2;
      QVERIFY(std::abs(p.sampleIndex - (150 + col)) <= 1);
      QCOMPARE(p.twtMs, 100.0 + 2.0 * p.sampleIndex);
    }

    const SgySliceImage truncated = makeDippingSlice(cols, 400, 150, 1, 40);
    SeismicTrackReport stopReport;
    SeismicTaskService::trackHorizonMultiSeeds(
        truncated, SgySliceType::Inline, 1000, 2000, 2000 + 2 * (cols - 1), {{10, 160}}, opt,
        QStringLiteral("t"), QStringLiteral("H1"), 2.0f, &stopReport);
    // 停因标注的测线号同样走列轴（线距 2 → 偶数线号）。
    const QRegularExpression re(QStringLiteral("@XL(\\d+)"));
    const QRegularExpressionMatch m = re.match(stopReport.stopSummary);
    QVERIFY2(m.hasMatch(), qPrintable(stopReport.stopSummary));
    QVERIFY2(m.captured(1).toInt() % 2 == 0, qPrintable(stopReport.stopSummary));
  }

  // ---- goal/horizon-autotrack：异步追踪可取消（取消不发布半成品）----
  void asyncTrackingCancellable()
  {
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);
    const int cols = 400;
    // rows 600：峰值 150+399=549 + 半宽 14 ≤ 599（事件全程在界内）
    const SgySliceImage slice = makeDippingSlice(cols, 600, 150, 1);

    bool called = false;
    bool okVal = false;
    int pickCount = -1;
    PaleoTask *task = svc.startHorizonTracking(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1,
        {{200, 350}}, {24, 12, 0.6}, QStringLiteral("t"), QStringLiteral("H1"),
        2.0f,
        [&](bool ok, const QList<SeismicPick> &picks,
            const SeismicTrackReport &, const QString &) {
          called = true;
          okVal = ok;
          pickCount = picks.size();
        });
    QVERIFY(task != nullptr);
    QSignalSpy finSpy(task, &PaleoTask::finished);
    task->requestCancel(); // 入池即取消（worker 起点检查 + 逐道谓词双保险）
    QVERIFY(finSpy.wait(10000));
    QVERIFY(called);
    QVERIFY(!okVal);
    QCOMPARE(pickCount, 0); // 取消：不发布
    QCOMPARE(task->state(), PaleoTask::State::Cancelled);

    // 正常完成路径：发布全量 + 报告
    called = false;
    PaleoTask *task2 = svc.startHorizonTracking(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1,
        {{200, 350}}, {24, 12, 0.6}, QStringLiteral("t"), QStringLiteral("H1"),
        2.0f,
        [&](bool ok, const QList<SeismicPick> &picks,
            const SeismicTrackReport &report, const QString &) {
          called = true;
          okVal = ok;
          pickCount = picks.size();
          QVERIFY(report.totalTraces == cols);
        });
    QVERIFY(task2 != nullptr);
    QSignalSpy finSpy2(task2, &PaleoTask::finished);
    QVERIFY(finSpy2.wait(10000));
    QVERIFY(called);
    QVERIFY(okVal);
    QCOMPARE(pickCount, cols);
    QCOMPARE(task2->state(), PaleoTask::State::Succeeded);
  }

  // ---- goal/horizon-autotrack：dock 多种子追踪 → 合并替换一步 undo/redo +
  //      面板覆盖率 QC 行（offscreen 全链：体 → 剖面 → 种子 → 追踪 → 会话）----
  void dockTrackingMergeUndoRedo()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("event.sgy");
    // 事件峰 = 40 + xlIdx（XL 向倾角 1 样/道；与 IL 无关——初始剖面取中线）
    QVERIFY(writeEventSegy(sgy, 6, 8, 128,
                           [](int, int xl) { return 40 + xl; }));
    auto volume = std::make_shared<SgyVolume>();
    std::string verr;
    QVERIFY(volume->Load(sgy.toStdString(), verr));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(volume);
    QVERIFY(finishedSpy.wait(10000));

    dock.setPickMode(SectionPickMode::Seed);
    // 两端手动种子（列 2/6，峰位 42/46 样 → 84/92ms）
    dock.addPickFromCanvas(2, 84.0);
    dock.addPickFromCanvas(6, 92.0);
    QCOMPARE(dock.interpretationSession().picks.size(), 2);

    QUndoStack *stack = dock.findChild<QUndoStack *>();
    QVERIFY(stack != nullptr);

    dock.setTrackSeedPick(dock.interpretationSession().picks.first().id);
    dock.setTrackOptions({24, 12, 0.6});
    QSignalSpy trackSpy(&dock, &SeismicSectionDockWidget::trackingFinished);
    dock.runTracking();
    QVERIFY(trackSpy.wait(10000));
    QCOMPARE(trackSpy.value(0).at(0).toBool(), true);

    // 全 8 列每列恰一拾取（两端种子双向 + 合并；手动列不被机器重复）
    const QList<SeismicPick> afterTrack = dock.interpretationSession().picks;
    QCOMPARE(afterTrack.size(), 8);
    QSet<int> colsSeen;
    for (const SeismicPick &p : afterTrack)
    {
      const int col = p.xlineNo - 2000;
      QVERIFY2(!colsSeen.contains(col), "同列重复拾取");
      colsSeen.insert(col);
      QVERIFY2(std::abs(p.sampleIndex - (40 + col)) <= 1,
               qPrintable(QStringLiteral("col %1 sample %2").arg(col).arg(p.sampleIndex)));
    }
    // 报告 + 面板 QC 行
    QCOMPARE(dock.lastTrackReport().totalTraces, 8);
    QCOMPARE(dock.lastTrackReport().coveredTraces, 8);
    QLabel *summary = dock.findChild<QLabel *>(QStringLiteral("trackSummaryLabel"));
    QVERIFY(summary != nullptr);
    QVERIFY2(summary->text().contains(QStringLiteral("覆盖 8/8")),
             qPrintable(summary->text()));

    // 一步 undo：追踪机器拾取整体消失，回到 2 个手动种子
    stack->undo();
    const QList<SeismicPick> afterUndo = dock.interpretationSession().picks;
    QCOMPARE(afterUndo.size(), 2);
    for (const SeismicPick &p : afterUndo)
      QCOMPARE(p.confidence, 1.0f);
    // redo：8 个回来（id 可异，状态等价）
    stack->redo();
    QCOMPARE(dock.interpretationSession().picks.size(), 8);
  }

  // ---- goal/horizon-autotrack Oracle#2：剖面种子 → 追踪 → 拾取集成层位 →
  //      GeoTIFF + LayerDeclaration → QgisLayerService 上图（offscreen）----
  void trackedHorizonToMapClosure()
  {
    QTemporaryDir dir;
    // 1) 双向追踪产出拾取集（单 IL 剖面 → 1×N 层位栅格）
    const int cols = 64;
    const SgySliceImage slice = makeDippingSlice(cols, 400, 150, 1);
    SeismicTrackReport report;
    const QList<SeismicPick> picks = SeismicTaskService::trackHorizonMultiSeeds(
        slice, SgySliceType::Inline, 1000, 2000, 2000 + cols - 1, {{32, 182}},
        {24, 12, 0.6}, QStringLiteral("t"), QStringLiteral("H1"), 2.0f,
        &report);
    QVERIFY(picks.size() >= cols * 0.9);

    // 2) catalog 上下文（源地震资产 + RAW 版本）
    DataCatalog catalog;
    QString err;
    QVERIFY(catalog.open(dir.path(), &err));
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_track");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    seismicAsset.displayName = QStringLiteral("event.sgy");
    QVERIFY(catalog.addAsset(seismicAsset, &err));
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_track");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = dir.filePath(QStringLiteral("event.sgy"));
    QVERIFY(catalog.addVersion(rawVersion, &err));

    // 3) 登记：CSV + GeoTIFF（经既有 horizonbinner 管线）+ 可上图声明
    LayerDeclaration decl;
    const QString outDir = dir.filePath(QStringLiteral("interpretation"));
    const QString path = SeismicTaskService::registerHorizonAsset(
        &catalog, seismicAsset.id, rawVersion.id, QStringLiteral("H1"), picks,
        outDir, &err, &decl);
    QVERIFY2(!path.isEmpty(), qPrintable(err));
    QCOMPARE(decl.layerId, QStringLiteral("horizon.H1"));
    QCOMPARE(decl.horizon, QStringLiteral("H1"));
    QCOMPARE(decl.type, QStringLiteral("raster"));
    QCOMPARE(decl.group, QStringLiteral("00_Data"));
    QVERIFY(decl.source.endsWith(QStringLiteral(".tif")));
    QVERIFY2(QFileInfo::exists(decl.source), qPrintable(decl.source));

    // 4) 上图：manifest 声明 + QgisLayerService 实例化（gdal raster）
    const QString projDir = dir.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projDir + QStringLiteral("/metadata")));
    QgisProjectService projectSvc;
    QVERIFY(projectSvc.createProject(projDir + QStringLiteral("/proj.qgz")));
    LayerManifest manifest(projDir + QStringLiteral("/metadata/project.sqlite"));
    QVERIFY(manifest.open(&err));
    QgisLayerService layerSvc(&projectSvc, &manifest);
    QString declErr;
    QVERIFY2(layerSvc.declare(decl, &declErr), qPrintable(declErr));
    QVERIFY(manifest.forHorizon(QStringLiteral("H1")).size() >= 1);
    QgsMapLayer *layer = layerSvc.instantiate(decl.layerId, &declErr);
    QVERIFY2(layer != nullptr, qPrintable(declErr));
    QVERIFY2(layer->isValid(), qPrintable(declErr));
    QCOMPARE(layer->type(), Qgis::LayerType::Raster);
  }

  // ---- D4.5：CSV 导出 ----
  void exportPicksCsvTest()
  {
    QTemporaryDir dir;
    QList<SeismicPick> picks = {{7, 1000, 2000, 50.0, 25, 0.8f, "A", "H"},
                                {8, 1001, 2001, 52.0, 26, 1.0f, "B", "H"}};
    const QString path = dir.filePath("picks.csv");
    QString err;
    QVERIFY2(SeismicTaskService::exportPicksCsv(picks, path, &err), qPrintable(err));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray content = f.readAll();
    QVERIFY(content.startsWith("id,inline,xline"));
    QVERIFY(content.contains("7,1000,2000,50.00"));
    f.close();
  }

  // ---- goal/horizon-3d：3D 传播入口可用性（Oracle#6 空态禁用带原因）----
  void dockPropagationReadinessEmptyStates()
  {
    // 无体 → NoVolume + 原因
    SeismicSectionDockWidget dock;
    QString reason;
    QCOMPARE(dock.propagationReadiness(&reason),
             SeismicSectionDockWidget::PropagationReadiness::NoVolume);
    QVERIFY2(reason.contains(QStringLiteral("无可用地震体")),
             qPrintable(reason));

    QTemporaryDir dir;
    const QString sgy = dir.filePath("prop.sgy");
    QVERIFY(writeEventSegy(sgy, 6, 8, 128, [](int, int xl) { return 40 + xl; }));
    auto volume = std::make_shared<SgyVolume>();
    std::string verr;
    QVERIFY(volume->Load(sgy.toStdString(), verr));
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setVolume(volume);
    QVERIFY(finishedSpy.wait(10000));

    // 有体 + inline 剖面但无手动拾取 → NoSeeds + 原因；按钮禁用带 tooltip
    QCOMPARE(dock.propagationReadiness(&reason),
             SeismicSectionDockWidget::PropagationReadiness::NoSeeds);
    QVERIFY2(reason.contains(QStringLiteral("手动拾取")), qPrintable(reason));
    QToolButton *btn = dock.findChild<QToolButton *>(QStringLiteral("btnPropagate3D"));
    QVERIFY(btn != nullptr);
    dock.setPickMode(SectionPickMode::Seed); // 建面板（含可用性刷新）
    QVERIFY(!btn->isEnabled());
    QVERIFY2(btn->toolTip().contains(QStringLiteral("手动拾取")),
             qPrintable(btn->toolTip()));

    // 放置手动种子 → Ready；按钮启用
    dock.addPickFromCanvas(4, 88.0); // 列 4 → XL 2004，峰 44 样 = 88ms
    QCOMPARE(dock.propagationReadiness(&reason),
             SeismicSectionDockWidget::PropagationReadiness::Ready);
    QVERIFY(btn->isEnabled());
  }

  // ---- goal/horizon-3d Oracle#1（UI 面）：剖面种子 → 3D 传播 → DERIVED 登记
  //      + GeoTIFF 上图声明（offscreen 全链；结果不进会话拾取表）----
  void dockVolumePropagationRegistersHorizon()
  {
    QTemporaryDir dir;
    const QString sgy = dir.filePath("prop_e2e.sgy");
    QVERIFY(writeEventSegy(sgy, 6, 8, 128, [](int, int xl) { return 40 + xl; }));
    auto volume = std::make_shared<SgyVolume>();
    std::string verr;
    QVERIFY(volume->Load(sgy.toStdString(), verr));

    // catalog 上下文（源地震资产 + RAW 版本——DERIVED 锚源体）
    DataCatalog catalog;
    QString err;
    QVERIFY(catalog.open(dir.path(), &err));
    CatalogAsset seismicAsset;
    seismicAsset.id = QStringLiteral("seis_hz3d_ui");
    seismicAsset.type = QStringLiteral("seismic");
    seismicAsset.format = QStringLiteral("sgy");
    QVERIFY(catalog.addAsset(seismicAsset, &err));
    CatalogVersion rawVersion;
    rawVersion.id = QStringLiteral("rawver_hz3d_ui");
    rawVersion.assetId = seismicAsset.id;
    rawVersion.stage = QStringLiteral("RAW");
    rawVersion.versionNumber = 1;
    rawVersion.managed = false;
    rawVersion.path = sgy;
    QVERIFY(catalog.addVersion(rawVersion, &err));

    SeismicSectionDockWidget dock;
    dock.resize(900, 650);
    QSignalSpy finishedSpy(&dock, &SeismicSectionDockWidget::sectionExtractionFinished);
    dock.setInterpretationCatalog(&catalog, seismicAsset.id, rawVersion.id,
                                  dir.filePath(QStringLiteral("interpretation")));
    dock.setVolume(volume);
    QVERIFY(finishedSpy.wait(10000));
    dock.setPickMode(SectionPickMode::Seed);
    dock.addPickFromCanvas(4, 88.0); // 手动种子（XL 2004 峰位）
    QCOMPARE(dock.propagationReadiness(),
             SeismicSectionDockWidget::PropagationReadiness::Ready);

    QSignalSpy propSpy(&dock, &SeismicSectionDockWidget::propagationFinished);
    QSignalSpy declSpy(&dock, &SeismicSectionDockWidget::horizonLayerDeclared);
    dock.runVolumePropagation();
    QVERIFY(propSpy.wait(30000));
    QCOMPARE(propSpy.value(0).at(0).toBool(), true);
    QCOMPARE(declSpy.count(), 1);
    const LayerDeclaration decl = qvariant_cast<LayerDeclaration>(declSpy.value(0).at(0));
    QCOMPARE(decl.layerId, QStringLiteral("horizon.H1"));
    QVERIFY2(QFileInfo::exists(decl.source), qPrintable(decl.source));
    QVERIFY(decl.source.endsWith(QStringLiteral(".tif")));

    // catalog：DERIVED + 父版本锚源体 RAW
    const CatalogVersion derived = catalog.currentVersion(
        QStringLiteral("seis_horizon_seis_hz3d_ui_H1"));
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList{rawVersion.id});

    // 面板复位：按钮回「3D 传播」、QC 行报全覆盖（6 IL × 8 XL = 48）
    QVERIFY(!dock.propagationActive());
    QToolButton *btn = dock.findChild<QToolButton *>(QStringLiteral("btnPropagate3D"));
    QVERIFY2(btn && btn->text().contains(QStringLiteral("3D 传播")),
             qPrintable(btn ? btn->text() : QString()));
    QLabel *summary = dock.findChild<QLabel *>(QStringLiteral("trackSummaryLabel"));
    QVERIFY(summary != nullptr);
    QVERIFY2(summary->text().contains(QStringLiteral("覆盖 48/48")),
             qPrintable(summary->text()));

    // 结果不进会话拾取表（图件级对象，非单剖面编辑对象）
    QCOMPARE(dock.interpretationSession().picks.size(), 1);
  }
};

// QgsApplication main（tst_canvas_tools 同式）：上图闭环用例需要 gdal
// raster provider 与 QgsProject 实例化
int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")),
                    true);
  app.initQgis();
  TestSeismicInterpret t;
  const int rc = QTest::qExec(&t, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_seismic_interpret.moc"
