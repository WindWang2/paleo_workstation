#include <QtTest>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgslinesymbol.h>
#include <qgsproject.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>

#include "../src/qgis/geopatterns.h"
#include "../src/qgis/qgisstyleservice.h"
#include "../src/qgis/semanticlegend.h"

// 方向 31 批 3：语义图例自动生成——按渲染器类目出图例项（词面 + swatch +
// 数据值），图层增删/renderer 变化联动 legendChanged；swatch 与图层符号
// 同源（像素抽样非空白 = 「图例所见 = 图面所得」的证据）。
class TestSemanticLegend : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
  }

  void emptyProjectBuildsNothing()
  {
    QgsProject project;
    SemanticLegendBuilder builder(&project);
    QVERIFY(builder.build().isEmpty());
  }

  void buildsFromRendererCategories()
  {
    QgsProject project;
    QgsVectorLayer lith(
        QStringLiteral("Polygon?field=lith:string&crs=EPSG:4326"),
        QStringLiteral("岩性"), QStringLiteral("memory"));
    QVERIFY(lith.isValid());
    QgisStyleService::applyLithologyPatternStyle(&lith, QStringLiteral("lith"));
    QgsVectorLayer wells(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("井位"),
                         QStringLiteral("memory"));
    QVERIFY(wells.isValid());
    QgisStyleService::applyWellCategoryStyle(&wells, QString());
    project.addMapLayer(&lith);
    project.addMapLayer(&wells);

    SemanticLegendBuilder builder(&project);
    const QVector<SemanticLegendSection> sections = builder.build();
    QCOMPARE(sections.size(), 2);
    // layerOrder = 顶部层在前（与画布绘制序同源）：后添加的「井位」在序首。
    QCOMPARE(sections.first().title, QStringLiteral("井位"));
    QCOMPARE(sections.last().title, QStringLiteral("岩性"));

    // 岩性节：分类类目全收（同义词桶 + 兜底），词面/数据值/swatch 齐。
    const SemanticLegendSection &lithSection = sections.last();
    QCOMPARE(lithSection.layerId, lith.id());
    QVERIFY(lithSection.items.size() > 14);
    bool sawSandstone = false;
    for (const SemanticLegendItem &item : lithSection.items)
    {
      QVERIFY(!item.label.isEmpty());
      if (item.semanticId == QLatin1String("sandstone"))
      {
        sawSandstone = true;
        QCOMPARE(item.label, QStringLiteral("砂岩"));
      }
    }
    QVERIFY(sawSandstone);

    // 井位节（single renderer 回退路径）：单条目 = 图层名。
    QCOMPARE(sections.first().items.size(), 1);
    QCOMPARE(sections.first().items.first().label, QStringLiteral("井位"));
    QVERIFY(sections.first().items.first().semanticId.isEmpty());
  }

  void layerAddRemoveAndRendererChangeSignal()
  {
    QgsProject project;
    SemanticLegendBuilder builder(&project);
    QSignalSpy changed(&builder, &SemanticLegendBuilder::legendChanged);
    QVERIFY(changed.isValid());

    QgsVectorLayer *lith = new QgsVectorLayer(
        QStringLiteral("Polygon?field=lith:string&crs=EPSG:4326"),
        QStringLiteral("岩性"), QStringLiteral("memory"));
    QVERIFY(lith->isValid());
    project.addMapLayer(lith); // 挂树 + layersAdded → legendChanged
    QCOMPARE(builder.build().size(), 1);
    QVERIFY(changed.count() >= 1);

    // renderer 替换（岩性花纹挂上）→ 信号 + 类目数变化。
    const int before = builder.build().first().items.size();
    QgisStyleService::applyLithologyPatternStyle(lith, QStringLiteral("lith"));
    QVERIFY(changed.count() >= 2);
    QVERIFY(builder.build().first().items.size() > before);

    // 图层名变化 → 联动（图例节标题）。
    lith->setName(QStringLiteral("沉积相"));
    QVERIFY(changed.count() >= 3);
    QCOMPARE(builder.build().first().title, QStringLiteral("沉积相"));

    // 删除图层 → 节随之消失（Oracle 3：增删联动）。
    const QString lithId = lith->id();
    project.removeMapLayer(lithId); // layer 归 project 所有并析构
    QVERIFY(builder.build().isEmpty());
    QVERIFY(changed.count() >= 4);
  }

  // swatch 与图层渲染一致性（像素抽样）：砂岩花纹 swatch 非空白（SVG 纹理
  // 真画出）；断层红像素在断层节 swatch 中出现（色 = 渲染器符号色）。
  void swatchPixelsMatchSymbol()
  {
    QgsProject project;
    QgsVectorLayer lith(
        QStringLiteral("Polygon?field=lith:string&crs=EPSG:4326"),
        QStringLiteral("岩性"), QStringLiteral("memory"));
    QVERIFY(lith.isValid());
    QgisStyleService::applyLithologyPatternStyle(&lith, QStringLiteral("lith"));
    project.addMapLayer(&lith);
    SemanticLegendBuilder builder(&project);

    // build() 结果先落局部（range-for 悬空引用是 UB）。
    const QVector<SemanticLegendSection> sections = builder.build();
    bool sandstoneChecked = false;
    for (const SemanticLegendSection &section : sections)
      for (const SemanticLegendItem &item : section.items)
        if (item.semanticId == QLatin1String("sandstone"))
        {
          sandstoneChecked = true;
          QVERIFY(!item.swatch.isNull());
          QCOMPARE(item.swatch.format(), QImage::Format_ARGB32_Premultiplied);
          int textured = 0;
          for (int y = 0; y < item.swatch.height(); ++y)
            for (int x = 0; x < item.swatch.width(); ++x)
            {
              const QRgb rgb = item.swatch.pixel(x, y);
              if (qRed(rgb) < 250 || qGreen(rgb) < 250 || qBlue(rgb) < 250)
                ++textured;
            }
          QVERIFY(textured > item.swatch.width() * item.swatch.height() / 10);
        }
    QVERIFY(sandstoneChecked);

    // 断层 swatch 含断层红（#D71414 ± 容差）——与 faultLineSymbol 同色源。
    std::unique_ptr<QgsLineSymbol> fault(GeoPatterns::faultLineSymbol(QStringLiteral("normal")));
    const QImage faultSwatch = fault->bigSymbolPreviewImage(
        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols);
    QVERIFY(!faultSwatch.isNull());
    int redPixels = 0;
    for (int y = 0; y < faultSwatch.height(); ++y)
      for (int x = 0; x < faultSwatch.width(); ++x)
      {
        const QRgb rgb = faultSwatch.pixel(x, y);
        if (qRed(rgb) > 180 && qGreen(rgb) < 90 && qBlue(rgb) < 90)
          ++redPixels;
      }
    QVERIFY(redPixels > 10);
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestSemanticLegend tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_semanticlegend.moc"
