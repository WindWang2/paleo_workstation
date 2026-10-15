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
#include "../src/metadata/paleoprojectstore.h"
#include "../src/services/datagraph.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/pages/datagraphpage.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/assetops.h"
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
  // opt-01 P1：工程数据端到端夹具 初始化→导入→关联→图谱→预览→更新→恢复→重开，
  // 数据驱动连跑两遍（各自全新工程），两遍的结构摘要必须一致（确定性）。
  // 全程以目录里的真实 asset/version/entity id 断言；源文件与历史版本字节不受损。
  void projectDataEndToEndTwice_data()
  {
    QTest::addColumn<int>("run");
    QTest::newRow("run1") << 1;
    QTest::newRow("run2") << 2;
  }
  void projectDataEndToEndTwice()
  {
    QFETCH(int, run);
    static QString firstDigest;
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString project = tmp.filePath(QStringLiteral("proj"));
    const QString srcDir = tmp.filePath(QStringLiteral("src"));
    QVERIFY(QDir().mkpath(project) && QDir().mkpath(srcDir));
    const QString las = QDir(srcDir).filePath(QStringLiteral("E2E-1.las"));
    const QByteArray v1Bytes =
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. E2E-1 : WELL\n"
        "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n100.5 51.0\n";
    auto writeSrc = [&](const QByteArray &b) {
      QFile f(las);
      return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
    };
    auto readAll = [](const QString &p) {
      QFile f(p);
      return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    QVERIFY(writeSrc(v1Bytes));

    QString assetId, v1Id, v1Sha, entityId, recoveredId;
    QStringList digest;
    {
      // 1) 初始化
      PaleoProjectStore store;
      DataImportService importer(&store);
      importer.setProjectDir(project);
      QVERIFY2(importer.catalogWritable(), qPrintable(importer.catalogOpenError()));
      DataCatalog *cat = importer.catalog();
      // 2) 导入
      QString err;
      assetId = importer.importProjectFile(las, &err);
      QVERIFY2(!assetId.isEmpty(), qPrintable(err));
      const CatalogVersion v1 = cat->currentVersion(assetId);
      QVERIFY(!v1.id.isEmpty() && !v1.sha256.isEmpty());
      v1Id = v1.id;
      v1Sha = v1.sha256;
      const QString v1Abs = DataCatalog::resolvedVersionPath(project, v1);
      QVERIFY2(!v1Abs.isEmpty(), "受管版本应解析在工程内");
      QCOMPARE(readAll(v1Abs), v1Bytes);
      // 3) 关联：井未建时导入只留未决链接；建井后走数据管理的「未决归位」
      //    （与导入同一判据，恰一候选才挂）把链接挂到真实井上。
      {
        bool pending = false;
        for (const auto &l : cat->linksForAsset(assetId))
          pending = pending || l.unresolved || l.entityId.isEmpty();
        QVERIFY2(pending, "无井时 LAS 导入应留未决链接");
        CatalogEntity w;
        w.id = cat->nextEntityId(QStringLiteral("well"));
        w.entityType = QStringLiteral("well");
        w.name = QStringLiteral("E2E-1");
        QVERIFY2(cat->addEntity(w, &err), qPrintable(err));
        const auto proposals = paleo::assetops::proposablePendingLinks(cat);
        QVector<int> picks;
        for (const auto &p : proposals)
          if (p.assetId == assetId && p.wellId == w.id)
            picks << p.linkIndex;
        QCOMPARE(picks.size(), 1);
        QCOMPARE(paleo::assetops::applyPendingResolutions(cat, picks, &err), 1);
      }
      QVector<EntityAssetLink> links;
      for (const auto &l : cat->linksForAsset(assetId))
        if (!l.unresolved && !l.entityId.isEmpty())
          links << l;
      QVERIFY(!links.isEmpty());
      entityId = links.first().entityId;
      QVERIFY(cat->hasEntity(entityId));
      // 4) 图谱：资料关联视图同时含真实版本节点与实体节点
      {
        using namespace paleo::datagraph;
        const auto index = buildIndex(*cat, {}, nullptr, nullptr);
        QVERIFY(index);
        Query q;
        q.mode = Mode::Relations;
        q.focus = assetKey(assetId);
        const Graph g = query(*index, q, nullptr, nullptr);
        QSet<QString> ids;
        for (const Node &n : g.nodes)
          ids.insert(n.id);
        QVERIFY2(ids.contains(versionKey(v1Id)), qPrintable(versionKey(v1Id)));
        QVERIFY2(ids.contains(entityKey(entityId)), qPrintable(entityKey(entityId)));
      }
      // 5) 预览：预览服务解析到的就是 v1 的受管字节
      PreviewDocService doc(&importer);
      QCOMPARE(readAll(doc.absolutePathForVersion(v1)), v1Bytes);
      // 6) 更新：源文件改动后再导入，库里出现内容 = 新字节的版本
      const QByteArray v2Bytes = v1Bytes + "101.0 52.0\n";
      QVERIFY(writeSrc(v2Bytes));
      const QString updatedAsset = importer.importProjectFile(las, &err);
      QVERIFY2(!updatedAsset.isEmpty(), qPrintable(err));
      const CatalogVersion v2 = cat->currentVersion(updatedAsset);
      QVERIFY(v2.id != v1Id);
      QCOMPARE(readAll(DataCatalog::resolvedVersionPath(project, v2)), v2Bytes);
      QCOMPARE(readAll(v1Abs), v1Bytes); // 历史版本字节不受更新影响
      // 观察到的产品口径：重导改过的同一源文件登记为新资产（不并入 A 的版本链），
      // 记入摘要。资产 A 的版本链更新走数据管理的受管版本原语：
      CatalogVersion a2 = cat->currentVersion(assetId);
      a2.id = cat->nextVersionId();
      a2.versionNumber = cat->currentVersion(assetId).versionNumber + 1;
      a2.parentVersionIds = {v1Id};
      a2.path = DataCatalog::managedPath(QStringLiteral("RAW"), assetId, a2.id, v1.fileName);
      {
        const QString abs = QDir(project).filePath(a2.path);
        QVERIFY(QDir().mkpath(QFileInfo(abs).absolutePath()));
        QFile f(abs);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(v2Bytes), qint64(v2Bytes.size()));
        f.close();
        a2.sha256 = DataCatalog::sha256FileHex(abs, &err);
      }
      QVERIFY2(cat->addVersion(a2, &err), qPrintable(err));
      QCOMPARE(cat->currentVersion(assetId).id, a2.id);
      // 7) 恢复：资产 A 回滚到 v1 内容（新版本指向 v1 字节，历史保留）
      const QString recoverAsset = assetId;
      const auto rb = paleo::assetops::rollbackToVersion(cat, project, recoverAsset, v1Id, &err);
      QVERIFY2(!rb.versionId.isEmpty(), qPrintable(err));
      QCOMPARE(rb.sha256, v1Sha);
      recoveredId = rb.versionId;
      QCOMPARE(cat->currentVersion(recoverAsset).id, recoveredId);
      QCOMPARE(readAll(DataCatalog::resolvedVersionPath(project, cat->versionById(recoveredId))),
               v1Bytes);
      QCOMPARE(readAll(v1Abs), v1Bytes);
      digest << QStringLiteral("assets=%1").arg(cat->assets().size())
             << QStringLiteral("versions=%1").arg(cat->versions().size())
             << QStringLiteral("links=%1").arg(cat->links().size())
             << QStringLiteral("sameAssetUpdate=%1").arg(updatedAsset == assetId)
             << QStringLiteral("recoveredNo=%1").arg(rb.versionNumber)
             << QStringLiteral("v1sha=%1").arg(v1Sha);
    }
    // 8) 重开：新服务实例读回同一事实
    {
      PaleoProjectStore store;
      DataImportService reopened(&store);
      reopened.setProjectDir(project);
      DataCatalog *cat = reopened.catalog();
      QVERIFY(cat && cat->isOpen());
      QCOMPARE(QStringLiteral("assets=%1").arg(cat->assets().size()), digest[0]);
      QCOMPARE(QStringLiteral("versions=%1").arg(cat->versions().size()), digest[1]);
      QCOMPARE(QStringLiteral("links=%1").arg(cat->links().size()), digest[2]);
      const CatalogVersion rec = cat->versionById(recoveredId);
      QCOMPARE(rec.sha256, v1Sha);
      QCOMPARE(cat->currentVersion(rec.assetId).id, recoveredId);
      QVERIFY(cat->hasEntity(entityId));
      QVERIFY(!cat->linksForAsset(assetId).isEmpty());
      const auto index = paleo::datagraph::buildIndex(*cat, {}, nullptr, nullptr);
      QVERIFY(index && index->nodes.contains(paleo::datagraph::versionKey(recoveredId)));
      QCOMPARE(readAll(las).isEmpty(), false); // 源文件仍在（导入不移动/不删源）
    }
    const QString d = digest.join(QLatin1Char(';'));
    qInfo().noquote() << QStringLiteral("OPT01_E2E run%1 %2").arg(run).arg(d);
    if (run == 1)
      firstDigest = d;
    else
      QCOMPARE(d, firstDigest); // 两遍确定性一致
  }
};

QTEST_MAIN(TestDataGraphPage)
#include "tst_datagraphpage.moc"
