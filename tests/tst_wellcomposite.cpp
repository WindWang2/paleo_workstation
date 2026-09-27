#include <QSignalSpy>
#include <QTest>
#include <QPainter>
#include <QImage>

#include "ui/wellcomposite/wellcompositetrack.h"
#include "ui/wellcomposite/wellcompositecanvas.h"
#include "ui/wellcomposite/wellcompositepanel.h"
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

    // 1. 全井自适应纵览图
    panel.canvas()->setScaleRatio(QStringLiteral("自适应"));
    QImage imgAdaptive(1200, 800, QImage::Format_ARGB32_Premultiplied);
    imgAdaptive.fill(Qt::white);
    panel.render(&imgAdaptive);
    QVERIFY(!imgAdaptive.isNull());
    imgAdaptive.save(QStringLiteral("/home/kevin/.config/antigravity_accounts/11111/.gemini/antigravity-cli/brain/45d666c3-a0cf-4a44-a54b-9328b30b5bea/screen_resform_composite.png"));

    // 2. 储层段 (2000m - 2100m) 1:500 地质精细标尺特写图
    panel.canvas()->setScaleRatio(QStringLiteral("1:500"));
    panel.canvas()->setScrollDepth(2000.0);
    QImage imgDetail(1200, 800, QImage::Format_ARGB32_Premultiplied);
    imgDetail.fill(Qt::white);
    panel.render(&imgDetail);
    QVERIFY(!imgDetail.isNull());
    imgDetail.save(QStringLiteral("/home/kevin/.config/antigravity_accounts/11111/.gemini/antigravity-cli/brain/45d666c3-a0cf-4a44-a54b-9328b30b5bea/screen_resform_reservoir_detail.png"));
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
