#include <QSignalSpy>
#include <QTest>
#include <QPainter>
#include <QImage>
#include <QDir>

// wave/ux-polish：视觉取证截图只经 PALEO_UI_CAPTURE 开关落盘（tst_ui.cpp
// 同款惯例）——路径形如 $PALEO_UI_CAPTURE/<name>.png，未设则完全跳过；
// 绝不允许个人绝对路径混进测试。
static void captureIfAsked(const QImage &img, const QString &name)
{
  const QString dir = qEnvironmentVariable("PALEO_UI_CAPTURE");
  if (dir.isEmpty() || img.isNull())
    return;
  QDir().mkpath(dir);
  img.save(dir + QLatin1Char('/') + name);
}

#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
#include "ui/wellcomposite/curveconfigdialog.h"
#include "ui/wellcomposite/wellpositionlegendwidget.h"
#include "io/wellcompositexml.h"
#include "qgis/qgisruntime.h"

using namespace WellComposite;

class TestWellComposite : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
  }

  void testLithologyPatterns()
  {
    // 测试常见岩性画刷生成
    QBrush bSand = LithologyPatternFactory::getBrush(QStringLiteral("细砂岩"));
    QCOMPARE(bSand.style(), Qt::TexturePattern);
    QVERIFY(!bSand.texture().isNull());

    QBrush bMud = LithologyPatternFactory::getBrush(QStringLiteral("灰色泥岩"));
    QCOMPARE(bMud.style(), Qt::TexturePattern);

    QBrush bLime = LithologyPatternFactory::getBrush(QStringLiteral("生物灰岩"));
    QCOMPARE(bLime.style(), Qt::TexturePattern);

    QBrush bDolo = LithologyPatternFactory::getBrush(QStringLiteral("白云岩"));
    QCOMPARE(bDolo.style(), Qt::TexturePattern);
  }

  void testTrackCreationAndCurveOverlayLimit()
  {
    // 验证各道类型与曲线道 1-4 根合并显示限制
    auto depthTrack = std::make_shared<DepthScaleTrack>(64.0);
    QCOMPARE(depthTrack->type(), TrackType::DepthScale);

    auto formTrack = std::make_shared<FormationTrack>(QStringLiteral("地层"), 75.0);
    QCOMPARE(formTrack->type(), TrackType::Formation);

    auto lithoTrack = std::make_shared<LithologyTrack>(QStringLiteral("岩性"), 80.0);
    QCOMPARE(lithoTrack->type(), TrackType::Lithology);

    auto coreTrack = std::make_shared<CoreTrack>(QStringLiteral("取芯"), 65.0);
    QCOMPARE(coreTrack->type(), TrackType::Core);

    auto textTrack = std::make_shared<TextTrack>(QStringLiteral("结论"), 110.0);
    QCOMPARE(textTrack->type(), TrackType::Text);

    auto symTrack = std::make_shared<SymbolTrack>(QStringLiteral("符号"), 48.0);
    QCOMPARE(symTrack->type(), TrackType::Symbol);

    auto imgTrack = std::make_shared<ImageTrack>(QStringLiteral("图片"), 100.0);
    QCOMPARE(imgTrack->type(), TrackType::Image);

    auto curveTrack = std::make_shared<CurveTrack>(QStringLiteral("测井三孔隙度"), 180.0);
    QCOMPARE(curveTrack->type(), TrackType::Curve);

    auto stratTrack = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    QCOMPARE(stratTrack->type(), TrackType::StratigraphyCompound);

    auto faciesTrack = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    QCOMPARE(faciesTrack->type(), TrackType::FaciesCompound);

    // 测井曲线道：测试最多合并 4 根曲线（1-4根）
    CurveData c1; c1.name = QStringLiteral("GR"); c1.depths = {1000, 1001, 1002}; c1.values = {45, 50, 55};
    CurveData c2; c2.name = QStringLiteral("AC"); c2.depths = {1000, 1001, 1002}; c2.values = {220, 225, 230};
    CurveData c3; c3.name = QStringLiteral("DEN"); c3.depths = {1000, 1001, 1002}; c3.values = {2.3f, 2.4f, 2.35f};
    CurveData c4; c4.name = QStringLiteral("CNCF"); c4.depths = {1000, 1001, 1002}; c4.values = {18, 20, 22};
    CurveData c5; c5.name = QStringLiteral("RT"); c5.depths = {1000, 1001, 1002}; c5.values = {10, 12, 11};

    QVERIFY(curveTrack->addCurve(c1));
    QVERIFY(curveTrack->addCurve(c2));
    QVERIFY(curveTrack->addCurve(c3));
    QVERIFY(curveTrack->addCurve(c4));
    QCOMPARE(curveTrack->curveCount(), 4);

    // 第 5 根曲线添加必须返回 false（严格限制最多 4 根曲线）
    QVERIFY(!curveTrack->addCurve(c5));
    QCOMPARE(curveTrack->curveCount(), 4);
  }

  void testCanvasDepthAndZooming()
  {
    WellCompositeCanvas canvas;
    canvas.resize(800, 600);
    canvas.setDepthRange(1000.0, 3000.0);

    QCOMPARE(canvas.minDepth(), 1000.0);
    QCOMPARE(canvas.maxDepth(), 3000.0);
    QCOMPARE(canvas.scrollDepth(), 1000.0);

    // 缩放放大测试
    QSignalSpy zoomSpy(&canvas, &WellCompositeCanvas::zoomChanged);
    canvas.zoomIn();
    QCOMPARE(zoomSpy.count(), 1);
    QVERIFY(canvas.zoomFactor() > 1.0);

    // 缩放复位测试
    canvas.resetZoom();
    QCOMPARE(canvas.zoomFactor(), 1.0);
    QCOMPARE(canvas.scrollDepth(), 1000.0);

    // 坐标换算测试
    const double midY = canvas.depthToY(2000.0);
    const double calcDepth = canvas.yToDepth(midY);
    QVERIFY(std::abs(calcDepth - 2000.0) < 1e-3);
  }

  void testDynamicScaleRatioCalculation()
  {
    WellCompositeCanvas canvas;
    canvas.resize(800, 600);
    canvas.setDepthRange(1000.0, 3000.0);
    canvas.setScaleRatio(QStringLiteral("1:500"));

    QCOMPARE(canvas.scaleRatio(), QStringLiteral("1:500"));

    QSignalSpy scaleSpy(&canvas, &WellCompositeCanvas::scaleRatioChanged);

    // 深度放大后，有效比例尺联动更新（分子变大，分母变小，例如 1:380）
    canvas.zoomIn();
    QVERIFY(scaleSpy.count() >= 1);
    QVERIFY(canvas.scaleRatio() != QStringLiteral("1:500"));
    QVERIFY(canvas.scaleRatio().startsWith(QLatin1String("1:")));
    const int zoomedDenom = canvas.scaleRatio().mid(2).toInt();
    QVERIFY(zoomedDenom < 500);

    // 缩放复位后，恢复 1:500
    canvas.resetZoom();
    QCOMPARE(canvas.scaleRatio(), QStringLiteral("1:500"));
  }

  void testCurveConfigDialogCombineAndDissolve()
  {
    WellCompositeCanvas canvas;
    canvas.resize(800, 600);
    canvas.setDepthRange(1000.0, 2000.0);

    // 添加深度道与两个曲线道
    auto depthTrack = std::make_shared<DepthScaleTrack>(64.0);
    canvas.addTrack(depthTrack);

    auto ct1 = std::make_shared<CurveTrack>(QStringLiteral("岩性测井"), 180.0);
    CurveData c1; c1.name = QStringLiteral("GR"); c1.depths = {1000, 1001}; c1.values = {50, 60};
    CurveData c2; c2.name = QStringLiteral("SP"); c2.depths = {1000, 1001}; c2.values = {-20, -10};
    ct1->addCurve(c1);
    ct1->addCurve(c2);
    canvas.addTrack(ct1);

    auto ct2 = std::make_shared<CurveTrack>(QStringLiteral("声波测井"), 180.0);
    CurveData c3; c3.name = QStringLiteral("AC"); c3.depths = {1000, 1001}; c3.values = {200, 210};
    ct2->addCurve(c3);
    canvas.addTrack(ct2);

    auto faciesTrack = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    canvas.addTrack(faciesTrack);

    QCOMPARE(canvas.trackCount(), 4);
    QCOMPARE(canvas.tracks()[0]->type(), TrackType::DepthScale);
    QCOMPARE(canvas.tracks()[3]->type(), TrackType::FaciesCompound);

    // 1. 测试 CurveConfigDialog 加载全部井道
    CurveConfigDialog dlg(&canvas);
    QCOMPARE(dlg.trackItems().size(), 4);

    // 2. 测试井道顺序调整：将最末尾的沉积相道移动到最前面 (置顶)
    dlg.moveTrack(3, 0);
    QCOMPARE(dlg.trackItems()[0].type, TrackType::FaciesCompound);
    dlg.applyConfiguration();
    QCOMPARE(canvas.tracks()[0]->type(), TrackType::FaciesCompound);
    QCOMPARE(canvas.tracks()[1]->type(), TrackType::DepthScale);

    // 恢复沉积相道到末尾 (置底)
    dlg.moveTrack(0, 3);
    QCOMPARE(dlg.trackItems()[3].type, TrackType::FaciesCompound);
    dlg.applyConfiguration();
    QCOMPARE(canvas.tracks()[3]->type(), TrackType::FaciesCompound);

    // 3. 测试合并曲线道：将 ct1 的 GR 与 ct2 的 AC 合并为新曲线道
    // ct1 当前为 index 1 (GR:0, SP:1), ct2 为 index 2 (AC:0)
    const bool okCombine = dlg.combineCurves({{1, 0}, {2, 0}}, QStringLiteral("三孔+岩性组合"));
    QVERIFY(okCombine);

    // ct2 变空被清理，组合道插入在 index 1，原 ct1(剩余SP) 顺延到 index 2
    QCOMPARE(dlg.trackItems().size(), 4);
    QCOMPARE(dlg.trackItems()[1].title, QStringLiteral("三孔+岩性组合"));
    QCOMPARE(dlg.trackItems()[1].curves.size(), 2);

    dlg.applyConfiguration();
    QCOMPARE(canvas.trackCount(), 4);

    // 4. 测试解散多曲线道
    const bool okDissolve = dlg.dissolveTrack(1);
    QVERIFY(okDissolve);
    // 组合道解散为两个独立单道，总道数变为 5
    QCOMPARE(dlg.trackItems().size(), 5);
    dlg.applyConfiguration();
    QCOMPARE(canvas.trackCount(), 5);
  }

  void testParseComprehensiveXmlRealData()
  {
    const QString xmlPath = QStringLiteral(
        "/home/kevin/projects/paleo_project/data/project_area/artifacts/raw/ast-28/ver-28/HZ28-6-1井综合柱状图-2021-沉积-地化室-未钻遇烃源岩层-测井-惠州勘探室.xml");

    ComprehensiveWellData data;
    QString err;
    const bool ok = parseComprehensiveWellXml(xmlPath, data, &err);
    QVERIFY2(ok, qPrintable(err));

    QCOMPARE(data.wellName, QStringLiteral("HZ28-6-1"));
    QVERIFY(data.x > 2000000.0);
    QVERIFY(data.y > 20000000.0);
    QVERIFY(data.maxDepth > 2000.0);

    // 验证各标准工作表分流抽取结果
    QVERIFY(!data.continuousCurves.isEmpty());
    QVERIFY(!data.discreteCurves.isEmpty());
    QVERIFY(!data.lithologyIntervals.isEmpty());
    QVERIFY(!data.formationIntervals.isEmpty());
    QVERIFY(!data.textIntervals.isEmpty());
    QVERIFY(!data.standardHorizons.isEmpty());
  }

  void testCompositePanelAssembly()
  {
    const QString xmlPath = QStringLiteral(
        "/home/kevin/projects/paleo_project/data/project_area/artifacts/raw/ast-28/ver-28/HZ28-6-1井综合柱状图-2021-沉积-地化室-未钻遇烃源岩层-测井-惠州勘探室.xml");

    WellCompositePanel panel;
    panel.resize(1200, 800);

    QVERIFY(panel.loadComprehensiveXml(xmlPath));
    QCOMPARE(panel.wellName(), QStringLiteral("HZ28-6-1"));
    QVERIFY(panel.canvas()->trackCount() >= 5);

    // 验证井深跨度合理准确（0 - 2475m）
    QCOMPARE(panel.canvas()->minDepth(), 0.0);
    QCOMPARE(panel.canvas()->maxDepth(), 2475.0);

    // 确保包含标尺道与曲线道、岩性道、地层道
    bool hasRuler = false, hasCurve = false, hasLitho = false, hasForm = false;
    for (const auto &t : panel.canvas()->tracks())
    {
      if (t->type() == TrackType::DepthScale) hasRuler = true;
      if (t->type() == TrackType::Curve) hasCurve = true;
      if (t->type() == TrackType::Lithology) hasLitho = true;
      if (t->type() == TrackType::Formation) hasForm = true;
    }
    QVERIFY(hasRuler);
    QVERIFY(hasCurve);
    QVERIFY(hasLitho);
    QVERIFY(hasForm);
    // 验证沉积相道按石油地质规范放置在最右侧/最后一道
    QCOMPARE(panel.canvas()->tracks().last()->type(), TrackType::FaciesCompound);

    // 1. 全井自适应纵览图
    panel.canvas()->setScaleRatio(QStringLiteral("自适应"));
    QImage imgAdaptive(1200, 800, QImage::Format_ARGB32_Premultiplied);
    imgAdaptive.fill(Qt::white);
    panel.render(&imgAdaptive);
    QVERIFY(!imgAdaptive.isNull());
    captureIfAsked(imgAdaptive, QStringLiteral("screen_resform_composite.png"));

    // 2. 储层段 (2000m - 2100m) 1:500 地质精细标尺特写图
    panel.canvas()->setScaleRatio(QStringLiteral("1:500"));
    panel.canvas()->setScrollDepth(2000.0);
    QImage imgDetail(1200, 800, QImage::Format_ARGB32_Premultiplied);
    imgDetail.fill(Qt::white);
    panel.render(&imgDetail);
    QVERIFY(!imgDetail.isNull());
    captureIfAsked(imgDetail, QStringLiteral("screen_resform_reservoir_detail.png"));
  }

  void testWellPositionLegendWidgetAndSync()
  {
    WellCompositePanel panel;
    panel.resize(1000, 700);

    QVERIFY(panel.legendWidget() != nullptr);

    // 装配包含地层和曲线的测试数据
    QVector<CurveData> curves;
    CurveData gr;
    gr.name = QStringLiteral("GR");
    gr.minScale = 20.0f;
    gr.maxScale = 180.0f;
    gr.unit = QStringLiteral("API");
    gr.depths = {1000.0f, 1050.0f, 1100.0f, 1200.0f, 1500.0f};
    gr.values = {40.0f, 60.0f, 80.0f, 70.0f, 50.0f};
    curves.append(gr);

    QVector<FormationInterval> forms;
    FormationInterval f1;
    f1.name = QStringLiteral("韩江组");
    f1.topDepth = 1000.0;
    f1.bottomDepth = 1200.0;
    f1.color = QColor(QStringLiteral("#4CAF50"));
    forms.append(f1);

    FormationInterval f2;
    f2.name = QStringLiteral("珠江组");
    f2.topDepth = 1200.0;
    f2.bottomDepth = 1500.0;
    f2.color = QColor(QStringLiteral("#2196F3"));
    forms.append(f2);

    QVERIFY(panel.loadLasCurves(QStringLiteral("TEST_WELL_1"), curves, forms));
    // 验证 LAS 测井加载后沉积相道位于最右侧/最后一道
    QCOMPARE(panel.canvas()->tracks().last()->type(), TrackType::FaciesCompound);

    // 验证初始视口与全井深度匹配
    QCOMPARE(panel.canvas()->minDepth(), 1000.0);
    QCOMPARE(panel.canvas()->maxDepth(), 1500.0);
    QCOMPARE(panel.canvas()->visibleTopDepth(), 1000.0);
    QVERIFY(panel.canvas()->visibleBottomDepth() > 1000.0);
    QVERIFY(panel.canvas()->visibleDepthSpan() > 0.0);

    // 监听 viewportChanged 信号
    QSignalSpy spyViewport(panel.canvas(), &WellCompositeCanvas::viewportChanged);
    QSignalSpy spyScale(panel.canvas(), &WellCompositeCanvas::scaleRatioChanged);

    // 1. 测试滚动视口
    panel.canvas()->setScrollDepth(1150.0);
    QCOMPARE(spyViewport.count(), 1);
    QCOMPARE(panel.canvas()->visibleTopDepth(), 1150.0);

    // 2. 测试放大缩放联动
    panel.canvas()->zoomIn();
    QVERIFY(spyViewport.count() >= 2);
    QVERIFY(spyScale.count() >= 1);

    // 3. 验证图例组件尺寸与存在性
    QVERIFY(panel.legendWidget()->height() >= 30);
    QVERIFY(panel.legendWidget()->scaleBar() != nullptr);
    QVERIFY(panel.legendWidget()->miniBar() != nullptr);

    // 4. 验证渲染正常
    QImage imgLegend(1000, 50, QImage::Format_ARGB32_Premultiplied);
    imgLegend.fill(Qt::white);
    panel.legendWidget()->render(&imgLegend);
    QVERIFY(!imgLegend.isNull());
    captureIfAsked(imgLegend, QStringLiteral("screen_legend_bar_test.png"));
  }

  void testFaciesPatterns()
  {
    // 测试各标准沉积相及微相地质纹理画刷生成
    const QStringList patternKeys = {
        QStringLiteral("distributary_channel"),
        QStringLiteral("水下分流河道"),
        QStringLiteral("mouth_bar"),
        QStringLiteral("河口坝"),
        QStringLiteral("sheet_sand"),
        QStringLiteral("席状砂"),
        QStringLiteral("interdistributary_bay"),
        QStringLiteral("分流间湾"),
        QStringLiteral("delta_front"),
        QStringLiteral("三角洲前缘"),
        QStringLiteral("delta_plain"),
        QStringLiteral("三角洲平原"),
        QStringLiteral("prodelta"),
        QStringLiteral("前三角洲"),
        QStringLiteral("shallow_marine"),
        QStringLiteral("浅海陆棚"),
        QStringLiteral("turbidite"),
        QStringLiteral("浊积砂体"),
        QStringLiteral("channel_lag"),
        QStringLiteral("滞留沉积"),
        QStringLiteral("tidal_flat"),
        QStringLiteral("潮坪微相")};

    for (const auto &key : patternKeys)
    {
      const QBrush brush = FaciesPatternFactory::getBrush(key);
      QCOMPARE(brush.style(), Qt::TexturePattern);
      QVERIFY(!brush.texture().isNull());
      QVERIFY(brush.texture().width() > 0);
      QVERIFY(brush.texture().height() > 0);
    }
  }

  void testStratigraphyCompoundTrack()
  {
    auto track = std::make_shared<StratigraphyCompoundTrack>(QStringLiteral("地层"), 145.0);
    QCOMPARE(track->type(), TrackType::StratigraphyCompound);
    QCOMPARE(track->title(), QStringLiteral("地层"));
    QCOMPARE(track->width(), 145.0);
    QCOMPARE(track->systemWidth(), 38.0);
    QCOMPARE(track->seriesWidth(), 42.0);
    QCOMPARE(track->formationWidth(), 65.0);

    track->setSubColumnWidths(40.0, 45.0);
    QCOMPARE(track->systemWidth(), 40.0);
    QCOMPARE(track->seriesWidth(), 45.0);
    QCOMPARE(track->formationWidth(), 60.0);

    // 1. 测试基于空地层时的自动推导
    track->autoDeriveStratigraphy({}, 1000.0, 3000.0);
    QVERIFY(!track->intervals().isEmpty());
    QCOMPARE(track->intervals().first().system, QStringLiteral("新近系"));

    // 2. 测试基于珠江口盆地标准地层序列的自动推导
    QVector<FormationInterval> fms = {
        {1000.0f, 1300.0f, QStringLiteral("粤海组"), QStringLiteral("YH")},
        {1300.0f, 1700.0f, QStringLiteral("韩江组"), QStringLiteral("HJ")},
        {1700.0f, 2100.0f, QStringLiteral("珠江组"), QStringLiteral("ZJ")},
        {2100.0f, 2500.0f, QStringLiteral("珠海组"), QStringLiteral("ZH")},
        {2500.0f, 2900.0f, QStringLiteral("恩平组"), QStringLiteral("EP")},
        {2900.0f, 3300.0f, QStringLiteral("文昌组"), QStringLiteral("WC")},
    };
    track->autoDeriveStratigraphy(fms, 1000.0, 3300.0);
    QCOMPARE(track->intervals().size(), 6);

    QCOMPARE(track->intervals()[0].system, QStringLiteral("新近系"));
    QCOMPARE(track->intervals()[0].series, QStringLiteral("上新统"));
    QCOMPARE(track->intervals()[1].system, QStringLiteral("新近系"));
    QCOMPARE(track->intervals()[1].series, QStringLiteral("中新统"));
    QCOMPARE(track->intervals()[2].system, QStringLiteral("新近系"));
    QCOMPARE(track->intervals()[2].series, QStringLiteral("早中新统"));
    QCOMPARE(track->intervals()[3].system, QStringLiteral("古近系"));
    QCOMPARE(track->intervals()[3].series, QStringLiteral("渐新统"));
    QCOMPARE(track->intervals()[4].system, QStringLiteral("古近系"));
    QCOMPARE(track->intervals()[4].series, QStringLiteral("始新统"));
    QCOMPARE(track->intervals()[5].system, QStringLiteral("古近系"));
    QCOMPARE(track->intervals()[5].series, QStringLiteral("始新统"));

    // 3. 测试道头与道体绘制
    QImage img(145, 200, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    track->paintHeader(p, QRectF(0, 0, 145, 72), 1500.0);
    track->paintBody(p, QRectF(0, 72, 145, 128), 1000.0, 2000.0, 0.128);
    p.end();
    QVERIFY(!img.isNull());
  }

  void testFaciesCompoundTrack()
  {
    auto track = std::make_shared<FaciesCompoundTrack>(QStringLiteral("沉积相"), 180.0);
    QCOMPARE(track->type(), TrackType::FaciesCompound);
    QCOMPARE(track->title(), QStringLiteral("沉积相"));
    QCOMPARE(track->width(), 180.0);
    QCOMPARE(track->majorWidth(), 48.0);
    QCOMPARE(track->subWidth(), 54.0);
    QCOMPARE(track->microWidth(), 78.0);

    track->setSubColumnWidths(50.0, 55.0);
    QCOMPARE(track->majorWidth(), 50.0);
    QCOMPARE(track->subWidth(), 55.0);
    QCOMPARE(track->microWidth(), 75.0);

    // 1. 测试空数据时的自动推导
    track->autoDeriveFacies({}, {}, 1000.0, 3000.0);
    QVERIFY(!track->intervals().isEmpty());

    // 2. 测试根据地层与岩性联合推导相、亚相、微相及纹理
    QVector<FormationInterval> fms = {
        {1000.0f, 1500.0f, QStringLiteral("珠江组"), QStringLiteral("ZJ")},
        {1500.0f, 2000.0f, QStringLiteral("珠海组"), QStringLiteral("ZH")},
    };
    QVector<LithologyInterval> liths = {
        {1000.0f, 1100.0f, QStringLiteral("细砂岩"), QStringLiteral("SS")},
        {1100.0f, 1200.0f, QStringLiteral("粉砂岩"), QStringLiteral("ST")},
        {1200.0f, 1300.0f, QStringLiteral("灰色泥岩"), QStringLiteral("MD")},
        {1500.0f, 1600.0f, QStringLiteral("中砂岩"), QStringLiteral("SS")},
    };
    track->autoDeriveFacies(fms, liths, 1000.0, 2000.0);
    QCOMPARE(track->intervals().size(), 4);

    // 细砂岩 -> 水下分流河道
    QCOMPARE(track->intervals()[0].majorFacies, QStringLiteral("三角洲相"));
    QCOMPARE(track->intervals()[0].subFacies, QStringLiteral("三角洲前缘"));
    QCOMPARE(track->intervals()[0].microFacies, QStringLiteral("水下分流河道"));
    QCOMPARE(track->intervals()[0].patternType, QStringLiteral("distributary_channel"));

    // 粉砂岩 -> 席状砂
    QCOMPARE(track->intervals()[1].microFacies, QStringLiteral("席状砂"));
    QCOMPARE(track->intervals()[1].patternType, QStringLiteral("sheet_sand"));

    // 灰色泥岩 -> 分流间湾
    QCOMPARE(track->intervals()[2].microFacies, QStringLiteral("分流间湾"));
    QCOMPARE(track->intervals()[2].patternType, QStringLiteral("interdistributary_bay"));

    // 3. 测试道头与道体绘制（验证纹理图案与半透明文字胶囊无崩溃）
    QImage img(180, 200, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    track->paintHeader(p, QRectF(0, 0, 180, 72), 1200.0);
    track->paintBody(p, QRectF(0, 72, 180, 128), 1000.0, 1400.0, 0.32);
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
  TestWellComposite tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_wellcomposite.moc"
