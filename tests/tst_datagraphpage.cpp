// 层：测试壳
// opt-01：资料图谱页（ui/pages/datagraphpage）返回栈契约的页面级测试。
// 「返回上一图谱」只能回到用户真正请求过的图谱状态——单一入口（打开成果
// 追溯/资料关联）不得在栈里留下中间态。
#include <QtTest>
#include <QAbstractButton>
#include <QComboBox>
#include <QGraphicsObject>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QDialog>
#include <QDialogButtonBox>
#include "helpers/visualcapture.h"
#include "../src/ui/pages/datalist.h"
#include <QToolButton>

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

  static QAbstractButton *backButton(DataGraphPage &page)
  {
    return page.findChild<QAbstractButton *>(QStringLiteral("dataGraphBack"));
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
  // 在图上按标题前缀找节点并双击（等价用户双击；NodeItem 经 singleShot 派发）。
  static bool doubleClickNode(DataGraphPage &page, const QString &titlePrefix)
  {
    auto *view = page.findChild<QGraphicsView *>(QStringLiteral("dataGraphView"));
    if (!view || !view->scene())
      return false;
    for (QGraphicsItem *item : view->scene()->items())
    {
      auto *obj = item->toGraphicsObject();
      if (!obj || !obj->toolTip().startsWith(titlePrefix))
        continue;
      view->centerOn(obj);
      const QPoint pt = view->mapFromScene(obj->sceneBoundingRect().center());
      QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, pt);
      return true;
    }
    return false;
  }
  static Mode mode(DataGraphPage &page)
  {
    auto *combo = page.findChild<QComboBox *>(QStringLiteral("dataGraphMode"));
    return combo ? Mode(combo->currentData().toInt()) : Mode::Overview;
  }

  // P1 E2E 主体：在全新临时工程里跑一遍，*digestOut = 结构摘要。失败经 QVERIFY
  // 直接返回（调用方以 QTest::currentTestFailed() 判定）。
  static void runEndToEnd(QString *digestOut)
  {
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
      // 明确断言产品口径：重导改过的同一源文件登记为新资产（新版本链从 1 起），
      // 不并入资产 A 的版本链；A 的当前版本仍是 v1。
      QVERIFY2(updatedAsset != assetId, "重导改动源文件应登记为新资产");
      QCOMPARE(cat->currentVersion(updatedAsset).versionNumber, 1);
      QCOMPARE(cat->currentVersion(assetId).id, v1Id);
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
    *digestOut = digest.join(QLatin1Char(';'));
  }

private slots:
  void associationEditorAndKnowledgeGraph() {
    QTemporaryDir dir;
    DataImportService importer; importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    auto *cat = importer.catalog();
    QVERIFY(cat && cat->isOpen());
    for (const auto &pair : {qMakePair(QString("w1"), QString("A1")), qMakePair(QString("s1"), QString("地震测区甲"))}) {
      CatalogEntity entity; entity.id = pair.first; entity.name = pair.second;
      entity.entityType = pair.first == "w1" ? "well" : "seismic_survey";
      QVERIFY(cat->addEntity(entity));
    }
    for (const auto &id : {QString("log"), QString("tops"), QString("volume"), QString("horizon")}) {
      CatalogAsset file; file.id = id; file.type = "auxiliary"; file.displayName = id + ".dat";
      QVERIFY(cat->addAsset(file));
    }
    DataListPanel list; list.setDocService(&doc); list.refreshAssetTable();
    list.selectAssetInViews("tops");
    list.editAssociationForSelection();
    auto *editor = list.findChild<QDialog *>("assetAssociationEditor");
    QVERIFY(editor);
    auto *target = editor->findChild<QComboBox *>("associationTarget");
    auto *role = editor->findChild<QComboBox *>("associationRole");
    auto *operation = editor->findChild<QComboBox *>("associationOperation");
    QVERIFY(target && role && operation);
    target->setCurrentIndex(target->findData("w1"));
    role->setCurrentIndex(role->findData("tops"));
    operation->setCurrentIndex(1);
    auto *buttons = editor->findChild<QDialogButtonBox *>();
    QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
    QVERIFY(paleo::tests::captureVisual(editor, "asset-association", QSize(620, 400)));
    QSignalSpy association(&list, &DataListPanel::assetAssociationChanged);
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(association.count(), 1);
    QCOMPARE(cat->linksForAsset("tops").size(), 1);
    QCOMPARE(cat->linksForAsset("tops").front().role, QString("tops"));
    for (const auto &id : {QString("log"), QString("volume"), QString("horizon")}) {
      EntityAssetLink link; link.assetId = id;
      link.entityId = id == "log" ? "w1" : "s1";
      link.entityType = id == "log" ? "well" : "seismic_survey";
      link.role = id == "log" ? "well_log" : id == "volume" ? "seismic_volume" : "horizon";
      QVERIFY(cat->addLink(link));
    }
    DataGraphPage graph; graph.resize(1200, 800); graph.setDocService(&doc);
    auto *workflow = graph.findChild<DataGraphWorkflow *>();
    QSignalSpy ready(workflow, &DataGraphWorkflow::ready);
    graph.show();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && ready.last().first().value<paleo::datagraph::Graph>().nodes.size() == 6, 15000);
    QCOMPARE(mode(graph), Mode::Knowledge);
    auto *graphView = graph.findChild<QGraphicsView *>("dataGraphView");
    QVERIFY(graphView->width() > graph.width() / 2);
    graph.findChild<QToolButton *>("dataGraphFit")->click();
    QVERIFY2(graphView->transform().m11() >= .60,
             qPrintable(QString::number(graphView->transform().m11())));
    QVERIFY(paleo::tests::captureVisual(&graph, "data-knowledge-graph", QSize(1200, 800)));
  }
  void initTestCase()
  {
    PaleoTheme::ensureApplicationFonts();
    PaleoTheme::applyLightTheme();
  }

  // 资料图谱顶栏是一行：操作是图标，筛选仍用下拉框显示当前值。
  void toolbarStaysOnOneIconRow()
  {
    DataGraphPage page;
    page.resize(1480, 640);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *bar = page.findChild<QWidget *>(QStringLiteral("dataGraphToolBar"));
    QVERIFY(bar);
    QVERIFY(qobject_cast<QHBoxLayout *>(bar->layout()));
    QVERIFY2(bar->height() > 0 && bar->height() <= 72,
             qPrintable(QString::number(bar->height())));
    const QStringList names{
        QStringLiteral("dataGraphBack"),     QStringLiteral("dataGraphHistory"),
        QStringLiteral("dataGraphRefresh"),  QStringLiteral("dataGraphFit"),
        QStringLiteral("dataGraphZoomIn"),   QStringLiteral("dataGraphZoomOut"),
        QStringLiteral("dataGraphCancel"),   QStringLiteral("dataGraphManage"),
        QStringLiteral("dataGraphExport")};
    for (const QString &name : names) {
      auto *button = page.findChild<QToolButton *>(name);
      QVERIFY2(button, qPrintable(name));
      QCOMPARE(button->parentWidget(), bar);
      QVERIFY2(button->text().isEmpty(), qPrintable(name));
      QVERIFY2(!button->icon().isNull(), qPrintable(name));
    }
    auto *summary = page.findChild<QLabel *>(QStringLiteral("dataGraphSummary"));
    QVERIFY(summary);
    QCOMPARE(summary->parentWidget(), bar);
    QVERIFY(button(page, QStringLiteral("预览文件 / 版本")));
    if (const QString shot = qEnvironmentVariable("PALEO_GRAPH_BAR_SHOT");
        !shot.isEmpty()) {
      QVERIFY(bar->grab().save(shot));
      QVERIFY(page.grab().copy(0, 0, page.width(), 120).save(shot + QStringLiteral(".page.png")));
    }
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
    QCOMPARE(mode(page), Mode::Knowledge);
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
    QCOMPARE(mode(page), Mode::Knowledge);
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
    QCOMPARE(mode(page), Mode::Knowledge);
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
    // 参照摘要每行自行计算（独立全新工程再跑一遍），单独跑 run2 也成立。
    QString reference;
    runEndToEnd(&reference);
    if (QTest::currentTestFailed())
      return;
    QString d;
    runEndToEnd(&d);
    if (QTest::currentTestFailed())
      return;
    qInfo().noquote() << QStringLiteral("OPT01_E2E run%1 %2").arg(run).arg(d);
    QCOMPARE(d, reference); // 确定性：同一流程两次独立执行摘要一致
  }
  // opt-01 P3：换代守卫的系统级回归——合并窗内连发两个焦点请求，最终落地的
  // 只能是后一个（真实版本 id 断言）；随后「取消计算」不留半截图，再刷新可恢复。
  void rapidRefocusLandsOnLatestGeneration()
  {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 400));
    DataImportService importer;
    importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataCatalog *cat = importer.catalog();
    QVERIFY(cat && cat->versions().size() >= 2);
    const CatalogVersion a = cat->versions().first();
    const CatalogVersion b = cat->versions().last();
    QVERIFY(a.id != b.id && a.assetId != b.assetId);
    DataGraphPage page;
    page.setDocService(&doc);
    page.resize(1100, 720);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *open = button(page, QStringLiteral("预览文件 / 版本"));
    auto *cancel = page.findChild<QAbstractButton *>(QStringLiteral("dataGraphCancel"));
    auto *refresh = page.findChild<QAbstractButton *>(QStringLiteral("dataGraphRefresh"));
    QVERIFY(open && cancel && refresh);
    auto *wf = page.findChild<DataGraphWorkflow *>();
    QVERIFY(wf);
    // ready 带产出该图的查询：按焦点记录每次落地，代替固定 qWait。
    QStringList landed;
    connect(wf, &DataGraphWorkflow::ready, &page,
            [&landed](const paleo::datagraph::Graph &g) { landed << g.query.focus; });
    QSignalSpy status(wf, &DataGraphWorkflow::statusChanged);
    QSignalSpy activated(&page, &DataGraphPage::versionActivated);
    const QString keyA = paleo::datagraph::versionKey(a.id);
    const QString keyB = paleo::datagraph::versionKey(b.id);
    page.openRelations(a.assetId, a.id);
    page.openRelations(b.assetId, b.id); // 同一合并窗内换焦点
    // 等「期望代」（焦点 B）的结果落地且工作流回到空闲。
    QTRY_VERIFY_WITH_TIMEOUT(landed.contains(keyB) && !status.last().at(1).toBool(), 15000);
    QVERIFY2(!landed.contains(keyA), qPrintable(landed.join(QLatin1Char(','))));
    QVERIFY(open->isEnabled());
    open->click();
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), b.id); // 不是 a：旧代被丢弃

    // 取消：刷新进入合并窗（busy）→ 取消计算；被取消的这一代不得落地。
    landed.clear();
    status.clear();
    refresh->click();
    QVERIFY(cancel->isEnabled()); // 「正在准备图谱查询」同步置忙
    cancel->click();
    QVERIFY(!status.isEmpty());
    QCOMPARE(status.last().at(0).toString(), QStringLiteral("图谱任务已取消"));
    QCOMPARE(status.last().at(1).toBool(), false);
    QVERIFY(!cancel->isEnabled());
    // 再刷新：恰好只落地这一次（被取消的那代若迟到会多出一次），焦点仍是 B。
    refresh->click();
    QTRY_VERIFY_WITH_TIMEOUT(!landed.isEmpty() && !status.last().at(1).toBool(), 15000);
    QCOMPARE(landed, QStringList{keyB});
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(), 5000);
    activated.clear();
    open->click();
    QCOMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), b.id);
  }
  // S5 第 2 轮 Low 4：R-7 的其余入口——下游影响、展开关联、实体目录分组、实体节点
  // 激活——各自单次操作只准备一次图谱查询（旧实现均为 2 次）。
  void everyRefocusEntryPointSendsOneQuery()
  {
    QTemporaryDir dir;
    QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), 40));
    DataImportService importer;
    importer.setProjectDir(dir.path());
    PreviewDocService doc(&importer);
    DataCatalog *cat = importer.catalog();
    QVERIFY(cat && !cat->versions().isEmpty());
    const CatalogVersion v = cat->versions().last();
    const auto links = cat->linksForAsset(v.assetId);
    QVERIFY(!links.isEmpty() && !links.first().entityId.isEmpty());
    const CatalogEntity entity = cat->entityById(links.first().entityId);
    QVERIFY(!entity.name.isEmpty());

    DataGraphPage page;
    page.setDocService(&doc);
    page.resize(1400, 900);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    auto *wf = page.findChild<DataGraphWorkflow *>();
    QVERIFY(wf);
    QSignalSpy status(wf, &DataGraphWorkflow::statusChanged);
    auto *open = button(page, QStringLiteral("预览文件 / 版本"));
    auto *impact = button(page, QStringLiteral("查看来源的下游影响"));
    auto *related = button(page, QStringLiteral("展开这个节点的关联"));
    QVERIFY(open && impact && related);
    const auto idle = [&] {
      return !status.isEmpty() && !status.last().at(1).toBool();
    };

    // 下游影响
    page.openRelations(v.assetId, v.id);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled() && idle(), 15000);
    QVERIFY(impact->isEnabled());
    status.clear();
    impact->click();
    QCOMPARE(mode(page), Mode::Impact);
    QCOMPARE(prepareCount(status), 1);
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);

    // 展开关联
    page.openRelations(v.assetId, v.id);
    QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled() && related->isEnabled() && idle(), 15000);
    page.openLineage(v.assetId, v.id); // 先离开关联模式，展开关联才有模式切换
    QTRY_VERIFY_WITH_TIMEOUT(related->isEnabled() && idle(), 15000);
    status.clear();
    related->click();
    QCOMPARE(mode(page), Mode::Relations);
    QCOMPARE(prepareCount(status), 1);
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);

    // 实体节点激活（双击图上实体）：先切到「实体目录」，确保双击引起真实的模式
    // 切换（实体目录 → 资料关联）——若停留在资料关联，旧码的下拉框信号不触发，
    // 子例在旧码上也会通过、失去鉴别力（S5 第 3 轮）。
    auto *modeBox = page.findChild<QComboBox *>(QStringLiteral("dataGraphMode"));
    QVERIFY(modeBox);
    modeBox->setCurrentIndex(modeBox->findData(int(Mode::Entities)));
    QCOMPARE(mode(page), Mode::Entities);
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);
    status.clear();
    QVERIFY2(doubleClickNode(page, entity.name), qPrintable(entity.name));
    QTRY_VERIFY_WITH_TIMEOUT(prepareCount(status) >= 1, 5000);
    QTest::qWait(50);
    QCOMPARE(mode(page), Mode::Relations);
    QCOMPARE(prepareCount(status), 1);
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);

    // 实体目录分组（工区概览里双击「实体目录」）
    modeBox->setCurrentIndex(modeBox->findData(int(Mode::Overview)));
    QTRY_VERIFY_WITH_TIMEOUT(idle(), 15000);
    status.clear();
    QVERIFY(doubleClickNode(page, QStringLiteral("实体目录")));
    QTRY_VERIFY_WITH_TIMEOUT(prepareCount(status) >= 1, 5000);
    QTest::qWait(50);
    QCOMPARE(mode(page), Mode::Entities);
    QCOMPARE(prepareCount(status), 1);
  }
};

QTEST_MAIN(TestDataGraphPage)
#include "tst_datagraphpage.moc"
