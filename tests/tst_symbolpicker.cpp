#include <QtTest>
#include <QListWidget>
#include <QSignalSpy>

#include <qgsapplication.h>

#include "../src/ui/symbols/symbolpickerpanel.h"

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
