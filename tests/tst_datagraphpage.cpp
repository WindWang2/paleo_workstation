// 层：测试壳
// opt-01：资料图谱页（ui/pages/datagraphpage）返回栈契约的页面级测试。
// 「返回上一图谱」只能回到用户真正请求过的图谱状态——单一入口（打开成果
// 追溯/资料关联）不得在栈里留下中间态。
#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/perffixtures.h"
#include "../src/services/datagraph.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/pages/datagraphpage.h"
#include "../src/ui/paleotheme.h"

using paleo::datagraph::Mode;

class TestDataGraphPage : public QObject
{
  Q_OBJECT

  static QPushButton *backButton(DataGraphPage &page)
  {
    for (auto *b : page.findChildren<QPushButton *>())
      if (b->text() == QStringLiteral("返回上一图谱"))
        return b;
    return nullptr;
  }
  static Mode mode(DataGraphPage &page)
  {
    auto *combo = page.findChild<QComboBox *>(QStringLiteral("dataGraphMode"));
    return combo ? Mode(combo->currentData().toInt()) : Mode::Overview;
  }

private slots:
  void initTestCase()
  {
    PaleoTheme::ensureApplicationFonts();
    PaleoTheme::applyLightTheme();
  }

  // 工区概览 → 打开某真实版本的成果追溯 → 返回：回到概览（旧实现落到一个
  // 先发后弃的「资料关联」中间态，返回键仍亮着）。
  void lineageBackReturnsToOverview()
  {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 40));
    DataImportService importer;
    importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataCatalog *cat = importer.catalog();
    QVERIFY(cat && cat->isOpen() && !cat->versions().isEmpty());
    const CatalogVersion v = cat->versions().last();

    DataGraphPage page;
    page.setDocService(&doc);
    page.resize(1100, 720);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    QCOMPARE(mode(page), Mode::Overview);
    auto *back = backButton(page);
    QVERIFY(back);
    QVERIFY(!back->isEnabled());

    page.openLineage(v.assetId, v.id);
    QCOMPARE(mode(page), Mode::Lineage);
    QVERIFY(back->isEnabled());
    back->click();
    QCOMPARE(mode(page), Mode::Overview);
    QVERIFY2(!back->isEnabled(), "返回栈残留了未请求过的中间态");
    QTest::qWait(400); // 合并窗后让查询落地，页面不因回退崩溃
  }

  // 概览 → 资料关联(A) → 成果追溯(B) → 返回 → 返回：逐级回到 A 的关联视图
  // 再回概览，每一级都是用户实际到过的状态。
  void relationsThenLineageBackIsStepwise()
  {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 40));
    DataImportService importer;
    importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataCatalog *cat = importer.catalog();
    QVERIFY(cat && cat->versions().size() >= 2);
    const CatalogVersion a = cat->versions().first();
    const CatalogVersion b = cat->versions().last();
    QVERIFY(a.id != b.id);

    DataGraphPage page;
    page.setDocService(&doc);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *back = backButton(page);
    QVERIFY(back);
    page.openRelations(a.assetId);
    QCOMPARE(mode(page), Mode::Relations);
    page.openLineage(b.assetId, b.id);
    QCOMPARE(mode(page), Mode::Lineage);
    back->click();
    QCOMPARE(mode(page), Mode::Relations);
    QVERIFY(back->isEnabled());
    back->click();
    QCOMPARE(mode(page), Mode::Overview);
    QVERIFY(!back->isEnabled());
    QTest::qWait(400);
  }
};

QTEST_MAIN(TestDataGraphPage)
#include "tst_datagraphpage.moc"
