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
#include "../src/workflow/datagraphworkflow.h"

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
  static QPushButton *button(DataGraphPage &page, const QString &text)
  {
    for (auto *b : page.findChildren<QPushButton *>())
      if (b->text() == text)
        return b;
    return nullptr;
  }
  static int prepareCount(const QSignalSpy &spy)
  {
    int n = 0;
    for (const auto &args : spy)
      n += args.at(0).toString() == QStringLiteral("正在准备图谱查询");
    return n;
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

    auto *wf = page.findChild<DataGraphWorkflow *>();
    QVERIFY(wf);
    QSignalSpy status(wf, &DataGraphWorkflow::statusChanged);
    page.openLineage(v.assetId, v.id);
    QCOMPARE(mode(page), Mode::Lineage);
    QCOMPARE(prepareCount(status), 1); // 单入口只发一次查询
    QVERIFY(back->isEnabled());
    status.clear();
    back->click();
    QCOMPARE(mode(page), Mode::Overview);
    QCOMPARE(prepareCount(status), 1);
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
    auto *open = button(page, QStringLiteral("预览文件 / 版本"));
    QVERIFY(back && open);
    auto *wf = page.findChild<DataGraphWorkflow *>();
    QVERIFY(wf);
    QSignalSpy status(wf, &DataGraphWorkflow::statusChanged);
    QSignalSpy assetAct(&page, &DataGraphPage::assetActivated);
    QSignalSpy versionAct(&page, &DataGraphPage::versionActivated);
    page.openRelations(a.assetId);
    QCOMPARE(mode(page), Mode::Relations);
    QCOMPARE(prepareCount(status), 1);
    page.openLineage(b.assetId, b.id);
    QCOMPARE(mode(page), Mode::Lineage);
    QCOMPARE(prepareCount(status), 2);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 15000);
    open->click(); // 追溯视图焦点 = B 的真实版本
    QCOMPARE(versionAct.count(), 1);
    QCOMPARE(versionAct.at(0).at(0).toString(), b.id);
    status.clear();
    back->click();
    QCOMPARE(mode(page), Mode::Relations);
    QCOMPARE(prepareCount(status), 1);
    QVERIFY(back->isEnabled());
    // 返回后焦点恢复为 A 的资产节点：预览激活的是 A 的真实资产 id。
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 15000);
    open->click();
    QCOMPARE(assetAct.count(), 1);
    QCOMPARE(assetAct.at(0).at(0).toString(), a.assetId);
    status.clear();
    back->click();
    QCOMPARE(mode(page), Mode::Overview);
    QCOMPARE(prepareCount(status), 1);
    QVERIFY(!back->isEnabled());
    QTest::qWait(400);
  }
  // opt-01 P4 + R-7：节点 → 版本 → 预览 → 追溯 → 返回 的往返，全程以目录里的
  // 真实 asset/version id 断言；且「追溯这个成果」单次点击只发一次图谱查询
  // （旧实现：模式下拉框信号 + 显式 request 双发，第二发撤掉第一发的代次）。
  void nodeVersionPreviewBackRoundTrip()
  {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 40));
    DataImportService importer;
    importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataCatalog *cat = importer.catalog();
    QVERIFY(cat && cat->isOpen() && !cat->versions().isEmpty());
    const CatalogVersion v = cat->versions().last();
    QVERIFY(!v.id.isEmpty() && !v.assetId.isEmpty());
    QCOMPARE(cat->versionById(v.id).assetId, v.assetId);

    DataGraphPage page;
    page.setDocService(&doc);
    page.resize(1100, 720);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *wf = page.findChild<DataGraphWorkflow *>();
    QVERIFY(wf);
    QSignalSpy status(wf, &DataGraphWorkflow::statusChanged);
    QSignalSpy activated(&page, &DataGraphPage::versionActivated);
    QSignalSpy located(&page, &DataGraphPage::versionFocusRequested);
    auto *open = button(page, QStringLiteral("预览文件 / 版本"));
    auto *locate = button(page, QStringLiteral("在数据管理中定位"));
    auto *trace = button(page, QStringLiteral("追溯这个成果"));
    auto *back = backButton(page);
    QVERIFY(open && locate && trace && back);

    // 1) 节点：以真实版本为焦点打开资料关联，渲染后焦点节点即选中节点。
    page.openRelations(v.assetId, v.id);
    QCOMPARE(mode(page), Mode::Relations);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 15000);
    // 2) 版本 → 预览：激活的必须正是该真实版本 id。
    open->click();
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), v.id);
    // 预览同时让数据管理页跟随定位（selectNode(navigate)），也须是同一真实 id。
    for (const auto &args : located)
      QCOMPARE(args.at(1).toString(), v.id);
    located.clear();
    locate->click();
    QCOMPARE(located.count(), 1);
    QCOMPARE(located.at(0).at(0).toString(), v.assetId);
    QCOMPARE(located.at(0).at(1).toString(), v.id);

    // 3) 追溯：单次点击只准备一次查询。
    status.clear();
    QVERIFY(trace->isEnabled());
    trace->click();
    QCOMPARE(mode(page), Mode::Lineage);
    QCOMPARE(prepareCount(status), 1);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 15000);
    open->click();
    QCOMPARE(activated.count(), 2);
    QCOMPARE(activated.at(1).at(0).toString(), v.id); // 追溯视图焦点仍是同一版本

    // 4) 返回：回到同一真实版本的资料关联，再预览仍是同一 id。
    QVERIFY(back->isEnabled());
    status.clear();
    back->click();
    QCOMPARE(mode(page), Mode::Relations);
    QCOMPARE(prepareCount(status), 1);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 15000);
    open->click();
    QCOMPARE(activated.count(), 3);
    QCOMPARE(activated.at(2).at(0).toString(), v.id);
    QVERIFY(QFile::exists(doc.absolutePathForVersion(cat->versionById(v.id))));
  }
};

QTEST_MAIN(TestDataGraphPage)
#include "tst_datagraphpage.moc"
