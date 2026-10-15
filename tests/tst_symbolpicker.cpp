#include <QtTest>
#include <QCoreApplication>
#include <QListWidget>
#include <QSignalSpy>
#include <QTranslator>

#include <qgsapplication.h>

#include "../src/ui/symbols/symbolpickerpanel.h"
#include "../src/qgis/geopatterns.h"
#include "../src/qgis/qgisstyleservice.h"

// 方向 31 批 4：符号选择器面板——三族词表浏览（缩略 + 词面）、语义过滤、
// patternPicked 语义 id 出口（视图只发信号）。缩略与图层渲染同管线
//（GeoPatterns 构造符号预览，icon 非空 = 渲染面可用）。
class TestSymbolPicker : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
  }

  void lithologyFamilyListsAndFilters()
  {
    SymbolPickerPanel panel(SymbolPickerPanel::Family::Lithology);
    auto *grid = panel.findChild<QListWidget *>(QStringLiteral("symbolGrid"));
    QVERIFY(grid != nullptr);
    QCOMPARE(grid->count(), 14); // 岩性词表全量（无过滤）

    // 语义过滤：词面「砂岩」子串命中 5 条（砂岩/粗/细/砾质砂岩/粉砂岩）。
    panel.setFilter(QStringLiteral("砂岩"));
    QCOMPARE(grid->count(), 5);
    panel.setFilter(QStringLiteral("salt")); // 语义 id 命中
    QCOMPARE(grid->count(), 1);
    panel.setFilter(QString());
    QCOMPARE(grid->count(), 14);

    // 缩略渲染非空（选择器所见 = 图面所得的最小证据）。
    for (int i = 0; i < grid->count(); ++i)
      QVERIFY(!grid->item(i)->icon().pixmap(44, 44).isNull());
  }

  void faciesAndLineFamilies()
  {
    SymbolPickerPanel facies(SymbolPickerPanel::Family::Facies);
    QCOMPARE(facies.findChild<QListWidget *>(QStringLiteral("symbolGrid"))->count(), 8);

    SymbolPickerPanel lines(SymbolPickerPanel::Family::LineStyle);
    auto *grid = lines.findChild<QListWidget *>(QStringLiteral("symbolGrid"));
    QCOMPARE(grid->count(), 8);
    lines.setFilter(QStringLiteral("断层"));
    QCOMPARE(grid->count(), 4); // 正/逆/走滑/推测断层
    lines.setFilter(QStringLiteral("boundary"));
    QCOMPARE(grid->count(), 1); // survey_boundary（语义 id 过滤）
  }

  void pickEmitsSemanticId()
  {
    SymbolPickerPanel panel(SymbolPickerPanel::Family::Lithology);
    QSignalSpy spy(&panel, &SymbolPickerPanel::patternPicked);
    QVERIFY(spy.isValid());
    panel.setFilter(QStringLiteral("岩盐"));
    auto *grid = panel.findChild<QListWidget *>(QStringLiteral("symbolGrid"));
    QCOMPARE(grid->count(), 1);
    grid->item(0)->setSelected(true);
    grid->setCurrentItem(grid->item(0));
    QCOMPARE(panel.selectedPatternId(), QStringLiteral("salt"));
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier,
                      grid->visualItemRect(grid->item(0)).center());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("salt"));
  }

  // goal7 W4：QT_TRANSLATE_NOOP 的登记上下文（GeoPatterns/QgisStyleService）
  // 必须与运行时查表上下文一致——QObject::tr 的上下文是 "QObject"，词面
  // 翻译永远查不中（本用例在修复前红）。用测试翻译器在目标上下文命中时
  // 返回标记串，验证取词确实走了登记的上下文。
  void translationContextsAreHonored()
  {
    class ProbeTranslator : public QTranslator
    {
      public:
        QString translate(const char *context, const char *sourceText,
                          const char *disambiguation = nullptr, int n = -1) const override
        {
          if (qstrcmp(context, "GeoPatterns") == 0 &&
              qstrcmp(sourceText, "砂岩") == 0)
            return QStringLiteral("TEST_SANDSTONE");
          if (qstrcmp(context, "QgisStyleService") == 0 &&
              qstrcmp(sourceText, "预探井") == 0)
            return QStringLiteral("TEST_WILDCAT");
          return QTranslator::translate(context, sourceText, disambiguation, n);
        }
    };
    ProbeTranslator probe;
    QCoreApplication::installTranslator(&probe);

    QCOMPARE(GeoPatterns::titleFor(QStringLiteral("sandstone")),
             QStringLiteral("TEST_SANDSTONE"));
    // 归一化比对也必须认译文（数据字段写译文的工程不丢桶）
    QCOMPARE(GeoPatterns::normalizeLithology(QStringLiteral("TEST_SANDSTONE")),
             QStringLiteral("sandstone"));

    QgisStyleService styles;
    const QVariantList defs = styles.wellCategoryDefinitions();
    bool wildcatFound = false;
    for (const QVariant &v : defs)
    {
      const QVariantMap m = v.toMap();
      if (m.value(QStringLiteral("id")).toString() == QLatin1String("wildcat"))
      {
        wildcatFound = true;
        QCOMPARE(m.value(QStringLiteral("title")).toString(),
                 QStringLiteral("TEST_WILDCAT"));
      }
    }
    QVERIFY(wildcatFound);

    QCoreApplication::removeTranslator(&probe);
    // 摘除翻译器后回落中文源串（守恒：默认行为不变）
    QCOMPARE(GeoPatterns::titleFor(QStringLiteral("sandstone")),
             QStringLiteral("砂岩"));
  }

  // goal7 W9：catalog.json 地层沉积相标准词面「三角洲」（strat_delta.svg）
  // 必须命中 delta 桶——词表只有「三角洲朵体」词面时数据值「三角洲」落
  // all-other（修复前红）。同义词层对齐 kLithologyAliases 既有先例。
  void faciesValueBucketsCoverCatalogTerms()
  {
    QVERIFY(GeoPatterns::valueBuckets(QStringLiteral("delta"))
                .contains(QStringLiteral("三角洲")));
    // 守恒：既有词面与近邻桶不受别名影响
    QVERIFY(GeoPatterns::valueBuckets(QStringLiteral("delta"))
                .contains(QStringLiteral("三角洲朵体")));
    QVERIFY(GeoPatterns::valueBuckets(QStringLiteral("deltaic"))
                .contains(QStringLiteral("三角洲相")));
    QVERIFY(!GeoPatterns::valueBuckets(QStringLiteral("deltaic"))
                 .contains(QStringLiteral("三角洲")));
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestSymbolPicker tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_symbolpicker.moc"
