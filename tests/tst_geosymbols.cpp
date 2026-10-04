#include <QtTest>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsfillsymbollayer.h>
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgslinesymbollayer.h>
#include <qgsmarkersymbollayer.h>
#include <qgsproject.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbollayer.h>
#include <qgssymbollayerutils.h>
#include <qgsvectorlayer.h>

#include "../src/qgis/geopatterns.h"
#include "../src/qgis/qgisstyleservice.h"

// 方向 31（geological-symbols）批 1：花纹/线型语义词表 + 样式入口。
//  - 词表完整性：岩性 14 / 相 8 / 线型 8，每条花纹 texture 的 qrc 资源
//    真实存在（资源断链 = 编译期 qrc 与词表失配，发布即图例空洞）；
//  - 归一化：id/词面/同义词 → 规范 id；未知原样；空 → 空；
//  - 入口接管语义：缺字段不接管；有字段分类渲染，砂岩/河流相桶挂
//    QgsSVGFillSymbolLayer 且 svgFilePath 锚 qrc（不落机器路径）；
//  - 断层线型：实测双符号层（线 + 齿 marker line）、推测单层虚线；
//  - qrc SVG 真渲染冒烟：bigSymbolPreviewImage 非空（防 svg cache 对
//    qrc 路径支持回归）。
class TestGeoSymbols : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
  }

  void patternVocabularyCompleteness()
  {
    const QVariantList lith = GeoPatterns::lithologyDefinitions();
    const QVariantList facies = GeoPatterns::faciesDefinitions();
    const QVariantList lines = GeoPatterns::lineStyleDefinitions();
    QCOMPARE(lith.size(), 14);
    QCOMPARE(facies.size(), 8);
    QCOMPARE(lines.size(), 8);

    const auto first = lith.first().toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("sandstone"));
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("砂岩"));
    QCOMPARE(first.value(QStringLiteral("group")).toString(), QStringLiteral("lithology"));

    // 每条花纹的 qrc 资源真实存在；每条线型带色/宽/式。
    const auto defs = lith + facies;
    for (const auto &v : defs)
    {
      const auto id = v.toMap().value(QStringLiteral("id")).toString();
      QVERIFY2(!GeoPatterns::textureResourcePath(id).isEmpty(),
               qPrintable(QStringLiteral("missing qrc texture for %1").arg(id)));
      QVERIFY(QFile::exists(GeoPatterns::textureResourcePath(id)));
    }
    for (const auto &v : lines)
    {
      const auto m = v.toMap();
      QVERIFY(!m.value(QStringLiteral("color")).toString().isEmpty());
      QVERIFY(m.value(QStringLiteral("widthMm")).toDouble() > 0);
      const auto style = m.value(QStringLiteral("style")).toString();
      QVERIFY(style == QStringLiteral("solid") || style == QStringLiteral("dash") ||
              style == QStringLiteral("dot"));
    }
    QCOMPARE(lines.first().toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("fault_normal"));
    QCOMPARE(lines.last().toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("survey_boundary"));
  }

  void normalizationAndBuckets()
  {
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("sandstone")),
             QStringLiteral("sandstone"));
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("砂岩")),
             QStringLiteral("sandstone"));
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("石灰岩")),
             QStringLiteral("limestone"));
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("含砾砂岩")),
             QStringLiteral("sandstone_pebbly"));
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("神秘岩")),
             QStringLiteral("神秘岩"));
    QVERIFY(GeoPatterns::normalizeLithology(QString()).isEmpty());

    QCOMPARE(GeoPatterns::normalizeFaultKind(QStringLiteral("逆冲断层")),
             QStringLiteral("reverse"));
    QCOMPARE(GeoPatterns::normalizeFaultKind(QStringLiteral("平移断层")),
             QStringLiteral("strike"));
    QCOMPARE(GeoPatterns::normalizeFaultKind(QStringLiteral("suspected")),
             QStringLiteral("inferred"));
    QCOMPARE(GeoPatterns::normalizeFaultKind(QStringLiteral("不明")), QStringLiteral("不明"));

    // valueBuckets：id + 词面 + 同义词同桶登记（分类渲染与选择器过滤共用）。
    const QStringList sandstone = GeoPatterns::valueBuckets(QStringLiteral("sandstone"));
    QVERIFY(sandstone.contains(QStringLiteral("sandstone")));
    QVERIFY(sandstone.contains(QStringLiteral("砂岩")));
    QVERIFY(sandstone.contains(QStringLiteral("石英砂岩")));
    QVERIFY(!sandstone.contains(QStringLiteral("细砂岩")));
    const QStringList fluvial = GeoPatterns::valueBuckets(QStringLiteral("fluvial"));
    QCOMPARE(fluvial.size(), 2); // 相词面无同义词层：id + 词面
    QVERIFY(GeoPatterns::valueBuckets(QStringLiteral("nope")).isEmpty() == false);
    QCOMPARE(GeoPatterns::valueBuckets(QStringLiteral("nope")),
             QStringList{QStringLiteral("nope")});

    QCOMPARE(GeoPatterns::titleFor(QStringLiteral("gypsum")), QStringLiteral("石膏"));
    QCOMPARE(GeoPatterns::titleFor(QStringLiteral("nope")), QStringLiteral("nope"));
  }

  void lithologyPatternStyleEntry()
  {
    // 缺字段：不接管渲染器（默认 singleSymbol 保持原样）。
    QgsVectorLayer noField(
        QStringLiteral("Polygon?field=z:double&crs=EPSG:4326"),
        QStringLiteral("poly1"), QStringLiteral("memory"));
    QVERIFY(noField.isValid());
    QgisStyleService::applyLithologyPatternStyle(&noField, QString());
    QCOMPARE(noField.renderer()->type(), QStringLiteral("singleSymbol"));
    QgisStyleService::applyLithologyPatternStyle(&noField, QStringLiteral("nope"));
    QCOMPARE(noField.renderer()->type(), QStringLiteral("singleSymbol"));
    // 非面层不接管。
    QgsVectorLayer points(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"),
                          QStringLiteral("memory"));
    QgisStyleService::applyLithologyPatternStyle(&points, QStringLiteral("lith"));
    QCOMPARE(points.renderer()->type(), QStringLiteral("singleSymbol"));

    // 有岩性字段：分类渲染；砂岩桶 = SVG 平铺（qrc 路径 + 词表宽度）。
    QgsVectorLayer withField(
        QStringLiteral("Polygon?field=lith:string&crs=EPSG:4326"),
        QStringLiteral("poly2"), QStringLiteral("memory"));
    QVERIFY(withField.isValid());
    QgisStyleService::applyLithologyPatternStyle(&withField, QStringLiteral("lith"));
    QCOMPARE(withField.renderer()->type(), QStringLiteral("categorizedSymbol"));
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(withField.renderer());
    QCOMPARE(cat->classAttribute(), QStringLiteral("lith"));

    int svgBuckets = 0, fallbackBuckets = 0;
    QString sandstonePath, conglomeratePath;
    double conglomerateWidth = -1.0;
    for (const QgsRendererCategory &c : cat->categories())
    {
      QCOMPARE(c.symbol()->symbolLayerCount(), 1);
      if (c.value() == QVariant(QStringLiteral("sandstone")))
      {
        auto *svg = dynamic_cast<QgsSVGFillSymbolLayer *>(c.symbol()->symbolLayer(0));
        QVERIFY(svg != nullptr);
        sandstonePath = svg->svgFilePath();
        QCOMPARE(svg->patternWidth(), 10.0);
      }
      if (c.value() == QVariant(QStringLiteral("砾岩（卵石）")))
      {
        // 同义词桶与规范桶同符号（归一化口径）。
        auto *svg = dynamic_cast<QgsSVGFillSymbolLayer *>(c.symbol()->symbolLayer(0));
        QVERIFY(svg != nullptr);
        conglomeratePath = svg->svgFilePath();
        conglomerateWidth = svg->patternWidth();
      }
      if (dynamic_cast<QgsSVGFillSymbolLayer *>(c.symbol()->symbolLayer(0)) != nullptr)
        ++svgBuckets;
      else
        ++fallbackBuckets;
    }
    QCOMPARE(sandstonePath, QStringLiteral(":/geology/textures/tex_sandstone_medium.svg"));
    QVERIFY(!sandstonePath.isEmpty() && QFile::exists(sandstonePath));
    QCOMPARE(conglomeratePath,
             QStringLiteral(":/geology/textures/tex_conglomerate_pebble.svg"));
    QCOMPARE(conglomerateWidth, 14.0);
    QCOMPARE(fallbackBuckets, 1); // all-other 兜底桶（纯色，不吞未知值）
    QCOMPARE(svgBuckets, 44);     // 岩性 14 条的 id/词面/同义词桶（词表变更时同步）
    // 全桶 = 同义词桶 + 1 兜底。
    QCOMPARE(cat->categories().size(), 44 + 1);
  }

  void faciesPatternStyleEntry()
  {
    QgsVectorLayer withField(
        QStringLiteral("Polygon?field=facies:string&crs=EPSG:4326"),
        QStringLiteral("fac"), QStringLiteral("memory"));
    QVERIFY(withField.isValid());
    QgisStyleService::applyFaciesPatternStyle(&withField, QStringLiteral("facies"));
    QCOMPARE(withField.renderer()->type(), QStringLiteral("categorizedSymbol"));
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(withField.renderer());
    QString fluvialPath;
    for (const QgsRendererCategory &c : cat->categories())
    {
      if (c.value() == QVariant(QStringLiteral("fluvial")))
      {
        auto *svg = dynamic_cast<QgsSVGFillSymbolLayer *>(c.symbol()->symbolLayer(0));
        QVERIFY(svg != nullptr);
        fluvialPath = svg->svgFilePath();
        QCOMPARE(svg->patternWidth(), 16.0);
      }
    }
    QCOMPARE(fluvialPath, QStringLiteral(":/geology/strata/strat_fluvial_fill.svg"));
  }

  void faultLineStyleEntry()
  {
    // 缺字段：不接管。
    QgsVectorLayer noField(QStringLiteral("LineString?field=z:double&crs=EPSG:4326"),
                           QStringLiteral("ln1"), QStringLiteral("memory"));
    QVERIFY(noField.isValid());
    QgisStyleService::applyFaultLineLayerStyle(&noField, QString());
    QCOMPARE(noField.renderer()->type(), QStringLiteral("singleSymbol"));

    QgsVectorLayer withField(
        QStringLiteral("LineString?field=fault_kind:string&crs=EPSG:4326"),
        QStringLiteral("ln2"), QStringLiteral("memory"));
    QVERIFY(withField.isValid());
    QgisStyleService::applyFaultLineLayerStyle(&withField, QStringLiteral("fault_kind"));
    QCOMPARE(withField.renderer()->type(), QStringLiteral("categorizedSymbol"));
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(withField.renderer());

    for (const QgsRendererCategory &c : cat->categories())
    {
      const QString v = c.value().toString();
      if (v == QLatin1String("normal") || v == QLatin1String("逆断层"))
      {
        // 实测断层：主线 + 齿 marker line 两层。
        QCOMPARE(c.symbol()->symbolLayerCount(), 2);
        QCOMPARE(c.symbol()->symbolLayer(0)->layerType(), QLatin1String("SimpleLine"));
        QCOMPARE(c.symbol()->symbolLayer(1)->layerType(), QLatin1String("MarkerLine"));
      }
      else if (v == QLatin1String("inferred") || v == QLatin1String("推测断层"))
      {
        // 推测断层：单层虚线，无齿。
        QCOMPARE(c.symbol()->symbolLayerCount(), 1);
        QCOMPARE(c.symbol()->symbolLayer(0)->layerType(), QLatin1String("SimpleLine"));
        QCOMPARE(c.symbol()->color().name().toUpper(), QStringLiteral("#D71414"));
      }
    }
    // 中文词面桶与 all-other 桶都在。
    QSet<QString> values;
    for (const QgsRendererCategory &c : cat->categories())
      if (c.value().isValid())
        values.insert(c.value().toString());
    QVERIFY(values.contains(QStringLiteral("normal")));
    QVERIFY(values.contains(QStringLiteral("正断层")));
    QVERIFY(values.contains(QStringLiteral("inferred")));
    QCOMPARE(values.size(), 4 + 4); // 4 id + 4 中文词面（all-other 无效值不入 set）
  }

  void linePatternFillAndSurveyBoundary()
  {
    // line pattern 填充（渐变相带等）：单层 LinePattern，距离/角度可测。
    std::unique_ptr<QgsFillSymbol> pattern(
        GeoPatterns::linePatternFillSymbol(QColor("#5D6E80"), 3.0, 45.0));
    QCOMPARE(pattern->symbolLayerCount(), 1);
    auto *layer =
        dynamic_cast<QgsLinePatternFillSymbolLayer *>(pattern->symbolLayer(0));
    QVERIFY(layer != nullptr);
    QCOMPARE(layer->distance(), 3.0);
    QCOMPARE(layer->lineAngle(), 45.0);

    // 测区边界线型：墨色实线 + 圆端头规范。
    std::unique_ptr<QgsLineSymbol> survey(GeoPatterns::surveyBoundaryLineSymbol());
    QCOMPARE(survey->symbolLayerCount(), 1);
    auto *simple =
        dynamic_cast<QgsSimpleLineSymbolLayer *>(survey->symbolLayer(0));
    QVERIFY(simple != nullptr);
    QCOMPARE(simple->color().name().toUpper(), QStringLiteral("#24303E"));
    QCOMPARE(simple->width(), 0.6);
    QCOMPARE(simple->penStyle(), Qt::SolidLine);
    QCOMPARE(simple->penCapStyle(), Qt::RoundCap);

    // 相界三类：确定实线/推测虚线/渐变点线。
    std::unique_ptr<QgsLineSymbol> inferred(GeoPatterns::faciesBoundaryLineSymbol(
        QStringLiteral("inferred")));
    auto *infLayer =
        dynamic_cast<QgsSimpleLineSymbolLayer *>(inferred->symbolLayer(0));
    QCOMPARE(infLayer->penStyle(), Qt::DashLine);
    std::unique_ptr<QgsLineSymbol> transitional(GeoPatterns::faciesBoundaryLineSymbol(
        QStringLiteral("transitional")));
    auto *trLayer =
        dynamic_cast<QgsSimpleLineSymbolLayer *>(transitional->symbolLayer(0));
    QCOMPARE(trLayer->penStyle(), Qt::DotLine);
  }

  // qrc SVG 经 QgsSvgCache 真渲染冒烟：花纹符号预览非空且可见（不只是
  // 描边框）——防止「路径存在但 svg cache 不读 qrc」的静默退化。
  void qrcSvgRendersThroughSvgCache()
  {
    std::unique_ptr<QgsFillSymbol> sym(
        GeoPatterns::lithologyFillSymbol(QStringLiteral("sandstone")));
    // V2 重载显式选（无参调用与 deprecated 版歧义）；fill 符号不受
    // crosshair flag 影响。
    QImage preview = sym->bigSymbolPreviewImage(
        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols);
    QVERIFY(!preview.isNull());
    // 至少存在非纯白像素（纹理真的画出来了）。
    int textured = 0;
    for (int y = 0; y < preview.height(); ++y)
      for (int x = 0; x < preview.width(); ++x)
      {
        const QRgb rgb = preview.pixel(x, y);
        if (qRed(rgb) < 250 || qGreen(rgb) < 250 || qBlue(rgb) < 250)
          ++textured;
      }
    QVERIFY(textured > preview.width() * preview.height() / 10);
  }

  // 符号覆盖（语义 id + 版本锚）与渲染器一起随工程保存重开——Oracle 1：
  // 花纹 round-trip 逐字段一致（SVG path/尺寸），且路径始终锚 qrc（不落
  // 机器绝对路径）。断层线型（两层结构）同口径验证。
  void symbolOverrideRoundTrip()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString projectPath = dir.filePath(QStringLiteral("roundtrip.qgs"));

    QgsProject project;
    QgsVectorLayer *lith = new QgsVectorLayer(
        QStringLiteral("Polygon?field=lith:string&crs=EPSG:4326"),
        QStringLiteral("岩性覆盖"), QStringLiteral("memory"));
    QVERIFY(lith->isValid());
    QVERIFY(QgisStyleService::applySymbolOverride(lith, QStringLiteral("lithology"),
                                                  QStringLiteral("conglomerate")));
    // 族不匹配/几何不匹配/未知 id → 拒绝且不动渲染器。
    QgsVectorLayer *reject = new QgsVectorLayer(
        QStringLiteral("Polygon?crs=EPSG:4326"), QStringLiteral("r"), QStringLiteral("memory"));
    QVERIFY(!QgisStyleService::applySymbolOverride(reject, QStringLiteral("lithology"),
                                                   QStringLiteral("fluvial"))); // 跨族
    QVERIFY(!QgisStyleService::applySymbolOverride(reject, QStringLiteral("line"),
                                                   QStringLiteral("fault_normal"))); // 面层
    QVERIFY(!QgisStyleService::applySymbolOverride(reject, QStringLiteral("lithology"),
                                                   QStringLiteral("mystery"))); // 词表外
    QgsVectorLayer *fault = new QgsVectorLayer(
        QStringLiteral("LineString?crs=EPSG:4326"), QStringLiteral("断层覆盖"),
        QStringLiteral("memory"));
    QVERIFY(QgisStyleService::applySymbolOverride(fault, QStringLiteral("line"),
                                                  QStringLiteral("fault_reverse")));
    project.addMapLayer(lith);
    project.addMapLayer(fault);
    QVERIFY(project.write(projectPath));

    // 重开：renderer DOM 原生恢复 + customProperty 语义锚恢复。
    QgsProject reloaded;
    QVERIFY(reloaded.read(projectPath));
    QgsVectorLayer *lithBack = qobject_cast<QgsVectorLayer *>(
        reloaded.mapLayer(lith->id()));
    QgsVectorLayer *faultBack = qobject_cast<QgsVectorLayer *>(
        reloaded.mapLayer(fault->id()));
    QVERIFY(lithBack != nullptr);
    QVERIFY(faultBack != nullptr);

    // 花纹：SVG fill 逐字段（path/宽度）一致，路径仍为 qrc。
    auto *lithRenderer =
        dynamic_cast<QgsSingleSymbolRenderer *>(lithBack->renderer());
    QVERIFY(lithRenderer != nullptr);
    auto *svgBack =
        dynamic_cast<QgsSVGFillSymbolLayer *>(lithRenderer->symbol()->symbolLayer(0));
    QVERIFY(svgBack != nullptr);
    QCOMPARE(svgBack->svgFilePath(),
             QStringLiteral(":/geology/textures/tex_conglomerate_pebble.svg"));
    QCOMPARE(svgBack->patternWidth(), 14.0);
    QCOMPARE(svgBack->angle(), 0.0); // 旋转默认值保持（词表未设旋转档）
    QVERIFY(svgBack->svgFilePath().startsWith(QLatin1String(":/"))); // 不落机器路径

    // customProperty 语义锚：family/id/version 齐且版本=当前词表锚。
    const QVariantMap semantics =
        lithBack->customProperty("paleo/symbolSemantics").toMap();
    QCOMPARE(semantics.value(QStringLiteral("family")).toString(),
             QStringLiteral("lithology"));
    QCOMPARE(semantics.value(QStringLiteral("id")).toString(),
             QStringLiteral("conglomerate"));
    QCOMPARE(semantics.value(QStringLiteral("version")).toInt(),
             QgisStyleService::symbolTableVersion());

    // 断层线型：两层结构（线+齿）重开保持。
    auto *faultRenderer =
        dynamic_cast<QgsSingleSymbolRenderer *>(faultBack->renderer());
    QVERIFY(faultRenderer != nullptr);
    QCOMPARE(faultRenderer->symbol()->symbolLayerCount(), 2);

    // 语义重放幂等：restore 后符号结构不变（再 round-trip 稳定）。
    // setRenderer 销毁旧渲染器——先快照字段值，restore 后旧 svgBack 已悬垂。
    const QString expectedSvgPath = svgBack->svgFilePath();
    const double expectedPatternWidth = svgBack->patternWidth();
    QVERIFY(QgisStyleService::restoreSymbolOverride(lithBack));
    auto *restored = dynamic_cast<QgsSingleSymbolRenderer *>(lithBack->renderer());
    QVERIFY(restored != nullptr);
    auto *svgRestored =
        dynamic_cast<QgsSVGFillSymbolLayer *>(restored->symbol()->symbolLayer(0));
    QVERIFY(svgRestored != nullptr);
    QCOMPARE(svgRestored->svgFilePath(), expectedSvgPath);
    QCOMPARE(svgRestored->patternWidth(), expectedPatternWidth);
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestGeoSymbols tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_geosymbols.moc"
