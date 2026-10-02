#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "ui/wellcomposite/chronostratcolors.h"
#include "ui/wellcomposite/patterncatalog.h"
#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/exportengine.h"
#include "ui/wellcomposite/wellcompositestore.h"
#include "ui/paleotheme.h" // initTestCase 钉死渲染环境
#include "domain/wellcompositemodel.h"
#include "qgis/qgisruntime.h"

#include <cmath>

using namespace WellComposite;

namespace {

// 确定性合成井（黄金图与导出共用）
ComprehensiveWellData syntheticWell()
{
  ComprehensiveWellData d;
  d.wellName = QStringLiteral("SYN-1");
  d.minDepth = 1000.0;
  d.maxDepth = 1400.0;

  CurveData gr;
  gr.name = QStringLiteral("GR");
  gr.unit = QStringLiteral("API");
  gr.minScale = 20.0f;
  gr.maxScale = 120.0f;
  gr.color = QColor(QStringLiteral("#2E7D32"));
  for (int i = 0; i <= 400; ++i)
  {
    const double depth = 1000.0 + i;
    gr.depths.append(static_cast<float>(depth));
    gr.values.append(static_cast<float>(60.0 + 40.0 * std::sin(i / 12.0)));
  }
  d.continuousCurves = {gr};

  d.formationIntervals = {
      {1000.0f, 1200.0f, QStringLiteral("珠江组"), QStringLiteral("ZJ"), QColor(QStringLiteral("#FFE082"))},
      {1200.0f, 1400.0f, QStringLiteral("珠海组"), QStringLiteral("ZH"), QColor(QStringLiteral("#FFCC80"))}};
  d.lithologyIntervals = {
      {1000.0f, 1100.0f, QStringLiteral("灰色泥岩")},
      {1100.0f, 1250.0f, QStringLiteral("细砂岩")},
      {1250.0f, 1400.0f, QStringLiteral("生物灰岩")}};
  d.standardHorizons = {{1200.0, QStringLiteral("T35")}};
  d.faciesIntervals = {
      {1000.0f, 1200.0f, QStringLiteral("三角洲相"), QStringLiteral("三角洲前缘"),
       QStringLiteral("河口坝"), QStringLiteral("mouth_bar")}};
  return d;
}

} // namespace

// wave/wellcomposite-deep — D4.x 视觉与输出测试（含 D8.2 黄金图像素抽样）
class TestWellCompositeVisual : public QObject
{
  Q_OBJECT

private slots:
  // 渲染环境钉死（fe7f226 同款）：vendor 字体 + Fusion + 浅色 palette——
  // 黄金图跨机器可复现的前提（本 wave 前缺此钉死，跨机色差 10-66/255
  // 超 22 容差；基线图随本提交在钉死环境重生成）。
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); }

  // ---- D4.1 国际年代色标 ----
  void testChronostratCounts()
  {
    const QStringList systems = ChronostratColors::systems();
    const QStringList series = ChronostratColors::allSeries();
    QVERIFY(systems.size() >= 14);
    QVERIFY(series.size() >= 34);
    // 合计 ≥40 分色单元
    QVERIFY(systems.size() + series.size() >= 40);
    // 每系至少一统且颜色合法
    for (const QString &sys : systems)
    {
      QVERIFY(!ChronostratColors::seriesOfSystem(sys).isEmpty());
      QVERIFY(ChronostratColors::systemColor(sys).isValid());
    }
    for (const QString &ser : series)
      QVERIFY(ChronostratColors::seriesColor(ser).isValid());
    // 未识别 = 中性灰（不臆造）
    QCOMPARE(ChronostratColors::systemColor(QStringLiteral("某某系")), QColor(QStringLiteral("#ECEFF1")));
  }

  void testChronostratFormationLookup()
  {
    QString sys, ser;
    ChronostratColors::lookupByFormation(QStringLiteral("珠江组"), &sys, &ser);
    QCOMPARE(sys, QStringLiteral("新近系"));
    QCOMPARE(ser, QStringLiteral("早中新统"));

    ChronostratColors::lookupByFormation(QStringLiteral("文昌组"), &sys, &ser);
    QCOMPARE(sys, QStringLiteral("古近系"));

    // 未登记名：通用系名后缀回退
    ChronostratColors::lookupByFormation(QStringLiteral("侏罗系XX段"), &sys, &ser);
    QCOMPARE(sys, QStringLiteral("侏罗系"));

    // 完全未识别 → 空（程序不猜）
    ChronostratColors::lookupByFormation(QStringLiteral("A"), &sys, &ser);
    QVERIFY(sys.isEmpty());

    // 系→统反查（指派对话框级联）
    QCOMPARE(ChronostratColors::systemForSeries(QStringLiteral("上新统")), QStringLiteral("新近系"));
    QCOMPARE(ChronostratColors::systemForSeries(QStringLiteral("更新统")), QStringLiteral("第四系"));
    QCOMPARE(ChronostratColors::systemForSeries(QStringLiteral("早中新统")), QStringLiteral("新近系"));
  }

  // ---- D4.2 花纹库 ≥30 ----
  void testPatternCatalogCount()
  {
    QVERIFY(PatternCatalog::lithologyPatternCount() >= 30);
    const auto &defs = PatternCatalog::lithologyPatterns();
    QSet<QString> keys;
    QSet<QString> categories;
    for (const auto &d : defs)
    {
      keys.insert(d.key);
      categories.insert(d.category);
      QVERIFY(d.bg.isValid());
      QVERIFY(d.fg.isValid());
    }
    QCOMPARE(keys.size(), defs.size()); // key 唯一
    QVERIFY(categories.size() >= 6);    // 碎屑/碳酸/蒸发/有机/火成/其他
    QVERIFY(categories.contains(QStringLiteral("蒸发岩")));
    QVERIFY(categories.contains(QStringLiteral("火成岩")));
  }

  void testPatternLookupAndPixmaps()
  {
    struct
    {
      const char *name;
      const char *expectedKey;
    } cases[] = {
        {"细砂岩", "fine_sandstone"}, {"白云质灰岩", "dolomitic_limestone"},
        {"石膏层", "gypsum"}, {"油页岩", "oil_shale"}, {"玄武岩", "basalt"},
        {"鲕粒灰岩", "oolitic_limestone"}, {"礁灰岩", "reef_limestone"}, {"凝灰岩", "tuff"},
        {"硬石膏", "anhydrite"}, {"磷块岩", "phosphorite"}};
    for (const auto &c : cases)
    {
      PatternDef def;
      QVERIFY2(PatternCatalog::lookupLithology(QString::fromUtf8(c.name), &def), c.name);
      QCOMPARE(def.key, QString::fromUtf8(c.expectedKey));
      const QPixmap pm = PatternCatalog::createLithoPattern(def.key, def.bg, def.fg);
      QVERIFY(!pm.isNull());
      QCOMPARE(pm.width(), 16);
    }
    // 最长关键词匹配：泥质砂岩 优先于 砂岩/泥岩
    PatternDef def;
    QVERIFY(PatternCatalog::lookupLithology(QStringLiteral("泥质砂岩"), &def));
    QCOMPARE(def.key, QStringLiteral("argillaceous_sandstone"));
    // 完全未命中
    QVERIFY(!PatternCatalog::lookupLithology(QStringLiteral("???"), nullptr));

    // Factory 集成：30+ 种经 LithologyPatternFactory 出纹理刷
    const QBrush b = LithologyPatternFactory::getBrush(QStringLiteral("油页岩"));
    QCOMPARE(b.style(), Qt::TexturePattern);
    QVERIFY(!b.texture().isNull());
  }

  // ---- D4.3 相名→花纹 JSON 映射 ----
  void testFaciesJsonMap()
  {
    // 内置映射命中
    QCOMPARE(PatternCatalog::faciesPatternKey(QStringLiteral("河口坝")), QStringLiteral("mouth_bar"));
    QCOMPARE(PatternCatalog::faciesPatternKey(QStringLiteral("颗粒滩")), QStringLiteral("shoal_grainstone"));
    // 未映射名 → 空（走 Factory 旧 if-else 兜底）
    QVERIFY(PatternCatalog::faciesPatternKey(QStringLiteral("未知相")).isEmpty());
    // 内置资源文件与代码同源
    QFile res(QStringLiteral(PATTERNS_JSON_PATH));
    QVERIFY(res.exists());
    QVERIFY(res.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(res.readAll());
    QVERIFY(doc.isObject());
    QVERIFY(doc.object().size() >= 10);
    QCOMPARE(doc.object().value(QStringLiteral("河口坝")).toString(), QStringLiteral("mouth_bar"));
  }

  void testUserFaciesMapOverride()
  {
    // 用户覆盖文件（QStandardPaths 沙箱内）
    const QString path = PatternCatalog::userFaciesMapPath();
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"自定义微相\": \"basalt\"}");
    f.close();

    PatternCatalog::reloadFaciesMap();
    QCOMPARE(PatternCatalog::faciesPatternKey(QStringLiteral("自定义微相")), QStringLiteral("basalt"));
    // 用户条目不冲掉内置
    QCOMPARE(PatternCatalog::faciesPatternKey(QStringLiteral("河口坝")), QStringLiteral("mouth_bar"));

    // 清理
    QFile::remove(path);
    PatternCatalog::reloadFaciesMap();
    QVERIFY(PatternCatalog::faciesPatternKey(QStringLiteral("自定义微相")).isEmpty());
  }

  // ---- D4.4 自动图例 ----
  void testLegendCollect()
  {
    const ComprehensiveWellData d = syntheticWell();
    WellCompositeCanvas canvas;
    canvas.resize(900, 600);
    auto curveTrack = std::make_shared<CurveTrack>(QStringLiteral("测井"), 180.0);
    curveTrack->addCurve(d.continuousCurves.first());
    canvas.addTrack(curveTrack);

    const auto entries = LegendGenerator::collect(d, canvas.tracks());
    QVERIFY(entries.size() >= 5); // 3 岩性 + 1 微相 + 1 曲线 + 1 标志层
    bool hasCurve = false, hasLitho = false, hasMarker = false;
    for (const auto &e : entries)
    {
      if (e.symbol == QStringLiteral("GR"))
      {
        hasCurve = true;
        QVERIFY(e.isLine);
        QVERIFY(e.detail.contains(QStringLiteral("API")));
      }
      if (e.symbol == QStringLiteral("细砂岩"))
        hasLitho = true;
      if (e.symbol == QStringLiteral("T35"))
        hasMarker = true;
    }
    QVERIFY(hasCurve);
    QVERIFY(hasLitho);
    QVERIFY(hasMarker);

    const QStringList lines = LegendGenerator::textLines(entries);
    QVERIFY(lines.join(QLatin1Char('|')).contains(QStringLiteral("GR\t20~120 API")));
  }

  void testLegendPaintSmoke()
  {
    const ComprehensiveWellData d = syntheticWell();
    const auto entries = LegendGenerator::collect(d, {});
    QImage img(500, 160, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    ExportEngine::applyExportRenderHints(p);
    const qreal used = LegendGenerator::paint(p, QRectF(4, 4, 492, 152), entries);
    p.end();
    QVERIFY(used > 0);
    QVERIFY(!img.isNull());
  }

  // ---- D4.5/D4.6/D4.7 导出 ----
  void testExportPng()
  {
    QTemporaryDir dir;
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("LegendCase"));
    panel.setProjectName(QStringLiteral("ExportPngCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"),
                                syntheticWell().continuousCurves,
                                syntheticWell().formationIntervals));

    ExportEngine::Options opt;
    opt.topDepth = 1000.0;
    opt.bottomDepth = 1400.0;
    opt.dpi = 300;
    opt.scaleRatio = QStringLiteral("1:500");
    opt.wellName = QStringLiteral("SYN-1");
    const QString path = dir.path() + QStringLiteral("/export.png");
    QString err;
    const QImage img = ExportEngine::renderToImage(*panel.canvas(), panel.currentData(), opt, &err);
    QVERIFY2(!img.isNull(), qPrintable(err));
    QVERIFY(img.save(path));
    QVERIFY(QFile(path).size() > 10000); // 300dpi 等效位图非平凡
    QVERIFY(img.height() > 1000);        // 400m @ ~23.6px/m ≈ 9400+（封顶前）或封顶值
  }

  void testExportPdf()
  {
    QTemporaryDir dir;
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("ExportPdfCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    const auto d = syntheticWell();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"), d.continuousCurves, d.formationIntervals));

    ExportEngine::Options opt;
    opt.topDepth = 1000.0;
    opt.bottomDepth = 1400.0;
    opt.scaleRatio = QStringLiteral("1:1000");
    opt.dpi = 300;
    opt.metersPerPage = 120.0; // D4.8 分页策略：2 页
    opt.wellName = QStringLiteral("SYN-1");
    opt.projectName = QStringLiteral("黄金图测试");

    const QString path = dir.path() + QStringLiteral("/export.pdf");
    const QString err = ExportEngine::exportCanvas(*panel.canvas(), panel.currentData(),
                                                   ExportEngine::Format::Pdf, path, opt);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    // PDF 头 + 多页（/Type /Page 出现次数 ≥ 2）
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    QVERIFY(bytes.startsWith("%PDF"));
    const int pageBreaks = bytes.count("/Type /Page");
    QVERIFY(pageBreaks >= 2);
    // 页眉四要素（文本流内）
    QVERIFY(bytes.contains("SYN-1"));
  }

  void testExportSvg()
  {
    QTemporaryDir dir;
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("ExportSvgCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    const auto d = syntheticWell();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"), d.continuousCurves, d.formationIntervals));

    ExportEngine::Options opt;
    opt.topDepth = 1000.0;
    opt.bottomDepth = 1400.0;
    opt.includeLegend = false;
    opt.wellName = QStringLiteral("SYN-1");
    const QString path = dir.path() + QStringLiteral("/export.svg");
    const QString err = ExportEngine::exportCanvas(*panel.canvas(), panel.currentData(),
                                                   ExportEngine::Format::Svg, path, opt);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    QVERIFY(bytes.contains("<svg"));
  }

  void testExportSkipsNoPrint()
  {
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("SkipNoPrintCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    const auto d = syntheticWell();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"), d.continuousCurves, d.formationIntervals));

    // 全部道关打印 → 拒绝导出并给原因
    for (const auto &t : panel.canvas()->tracks())
      t->setPrintIncluded(false);
    WellCompositeStore::clearSessionTracks(panel.projectName(), panel.wellName()); // 不污染后续
    ExportEngine::Options opt;
    opt.topDepth = 1000.0;
    opt.bottomDepth = 1400.0;
    const QString err = ExportEngine::exportCanvas(*panel.canvas(), panel.currentData(),
                                                   ExportEngine::Format::Pdf,
                                                   QStringLiteral("/tmp/never.pdf"), opt);
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains(QStringLiteral("无可导出的道")));
  }

  void testExportHeaderAndPreview()
  {
    ExportEngine::Options opt;
    opt.wellName = QStringLiteral("W-77");
    opt.projectName = QStringLiteral("P");
    opt.scaleRatio = QStringLiteral("1:500");
    const QStringList lines = ExportEngine::headerLines(opt);
    QCOMPARE(lines.size(), 4);
    QVERIFY(lines.at(0).contains(QStringLiteral("W-77")));
    QVERIFY(lines.at(2).contains(QStringLiteral("日期")));
    QVERIFY(lines.at(3).contains(QStringLiteral("1:500")));

    // D4.9 预览（整井快览定高）
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("PreviewCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    const auto d = syntheticWell();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"), d.continuousCurves, d.formationIntervals));
    for (const auto &t : panel.canvas()->tracks())
      qInfo() << "  t:" << t->title() << t->isVisible() << t->isPrintIncluded();
    const QImage preview = ExportEngine::renderPreview(*panel.canvas(), panel.currentData(), 320);
    QVERIFY(!preview.isNull());
    QCOMPARE(preview.height(), 320);
  }

  // ---- D4.10 导出预设持久化 ----
  void testExportPresets()
  {
    QTemporaryDir dir;
    const QString src = dir.path() + QStringLiteral("/well.xml");
    WellCompositeStore store(src);
    QVERIFY(store.load());

    QList<ExportPreset> presets;
    ExportPreset p;
    p.name = QStringLiteral("汇报版");
    p.format = QStringLiteral("pdf");
    p.scaleRatio = QStringLiteral("1:200");
    p.dpi = 600;
    presets << p;
    store.setExportPresets(presets);
    QVERIFY(store.save());

    WellCompositeStore reread(src);
    QVERIFY(reread.load());
    QCOMPARE(reread.exportPresets().size(), 1);
    QCOMPARE(reread.exportPresets().first().name, QStringLiteral("汇报版"));
    QCOMPARE(reread.exportPresets().first().dpi, 600);
  }

  // ---- D4.11/D4.12 网格与双渲染参数 ----
  void testGridDensityAndExportPens()
  {
    CurveTrack t(QStringLiteral("测井"), 170.0);
    CurveData c;
    c.name = QStringLiteral("GR");
    c.penWidth = 1.0f;
    c.depths = {1000.0f, 1001.0f};
    c.values = {50.0f, 60.0f};
    t.addCurve(c);

    t.setShowGrid(false);
    QVERIFY(!t.showGrid());
    t.setGridDensity(3);
    QCOMPARE(t.gridDensity(), 3);
    t.setGridDensity(9);
    QCOMPARE(t.gridDensity(), 3); // 夹 [0,3]
    t.setGridDensity(-1);
    QCOMPARE(t.gridDensity(), 0);

    // D4.12：导出档抗锯齿 hints
    QImage img(100, 100, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    ExportEngine::applyExportRenderHints(p);
    QVERIFY(p.testRenderHint(QPainter::Antialiasing));
    QVERIFY(p.testRenderHint(QPainter::TextAntialiasing));
    p.end();
  }

  // ---- D8.2 黄金图像素抽样 ----
  void testGoldenImageSampling()
  {
    const QString goldenDir = QStringLiteral(WELLGOLDEN_DIR);
    WellCompositePanel panel;
    panel.setProjectName(QStringLiteral("GoldenCase"));
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();
    const auto d = syntheticWell();
    QVERIFY(panel.loadLasCurves(QStringLiteral("SYN-1"), d.continuousCurves, d.formationIntervals));
    // 岩性道补充（loadLasCurves 不装岩性）
    auto litho = std::make_shared<LithologyTrack>(QStringLiteral("岩性"), 80.0);
    litho->setIntervals(d.lithologyIntervals);
    panel.canvas()->addTrack(litho);
    panel.canvas()->setMarkerLines(d.standardHorizons);
    panel.canvas()->setScaleRatio(QStringLiteral("自适应"));
    QApplication::processEvents();

    QImage img(900, 600, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    panel.render(&img);

    // 黄金图不存在则写入基线（首跑建立；PR 附带基线图）
    const QString goldenPath = goldenDir + QStringLiteral("/golden_syn_full.png");
    if (!QFile::exists(goldenPath))
    {
      QDir().mkpath(goldenDir);
      QVERIFY(img.save(goldenPath));
      QWARN("黄金图基线已建立（首跑）");
    }

    QImage golden(goldenPath);
    QVERIFY(!golden.isNull());
    // 像素抽样：8x6 网格中心色，均值比对抗 AA/字体差异（每点 7x7 均值）
    const auto sampleAt = [](const QImage &image, int x, int y) {
      long r = 0, g = 0, b = 0;
      const int half = 3;
      int n = 0;
      for (int dy = -half; dy <= half; ++dy)
        for (int dx = -half; dx <= half; ++dx)
        {
          const QColor c = image.pixelColor(qBound(0, x + dx, image.width() - 1),
                                            qBound(0, y + dy, image.height() - 1));
          r += c.red();
          g += c.green();
          b += c.blue();
          ++n;
        }
      return QColor(r / n, g / n, b / n);
    };

    int diffs = 0;
    const int total = 8 * 6;
    QStringList diffDetail; // 诊断输出：超容差点的网格坐标与两侧均值色
    for (int gy = 0; gy < 6; ++gy)
    {
      for (int gx = 0; gx < 8; ++gx)
      {
        const int x = gx * golden.width() / 8 + golden.width() / 16;
        const int y = gy * golden.height() / 6 + golden.height() / 16;
        const QColor a = sampleAt(golden, x, y);
        const QColor b = sampleAt(img, x, y);
        // 大色块均值容差：22/255（抗 AA 与光栅化差异）
        if (std::abs(a.red() - b.red()) > 22 || std::abs(a.green() - b.green()) > 22 ||
            std::abs(a.blue() - b.blue()) > 22)
        {
          ++diffs;
          diffDetail << QStringLiteral("gx=%1 gy=%2 golden=%3 cur=%4")
                            .arg(QString::number(gx), QString::number(gy), a.name(), b.name());
        }
      }
    }
    if (diffs > 4)
      qInfo() << "GOLDEN-DIFF" << diffDetail.join(QStringLiteral(", "));
    // ≥ 44/48 抽样点稳定一致（允许 4 个网格点跨文字/线条）
    QVERIFY2(diffs <= 4, qPrintable(QStringLiteral("%1/48 抽样点超容差").arg(diffs)));
  }

  // ---- D7.4 高对比 + D6.5 TWT 副列 ----
  void testHighContrastAndTwt()
  {
    WellCompositeCanvas canvas;
    auto ruler = std::make_shared<DepthScaleTrack>(64.0);
    canvas.addTrack(ruler);

    // TWT 副刻度
    QVector<QPair<double, QString>> twt = {{1050.0, QStringLiteral("880ms")}};
    canvas.setTwtLabels(twt);
    QCOMPARE(ruler->twtLabels().size(), 1);

    // 高对比传播
    canvas.setHighContrast(true);
    QVERIFY(ruler->highContrast());
    canvas.setHighContrast(false);
    QVERIFY(!ruler->highContrast());

    // 绘制冒烟
    QImage img(64, 300, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    canvas.setDepthRange(1000.0, 1400.0);
    ruler->paintBody(p, QRectF(0, 0, 64, 300), 1000.0, 1400.0, 0.75);
    p.end();
    QVERIFY(!img.isNull());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestWellCompositeVisual tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite_visual.moc"
