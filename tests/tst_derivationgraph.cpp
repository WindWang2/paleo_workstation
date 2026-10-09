#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QPainter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/derivationgraph.h"
#include "../src/services/cataloghealth.h"
#include "../src/ui/dialogs/cataloghealthdialog.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/datapreview/datapreviewtabs.h"
#include "../src/ui/datapreview/officepreviewwidget.h"
#include "../src/ui/pages/datapage.h"
#include "../src/ui/pages/entitypanel.h"
#include "../src/ui/pages/derivationgraph.h"
#include "../src/ui/paleotheme.h"
#include "../src/ui/paleomainwindow.h"

using namespace paleo::derivation;
class TestDerivationGraph : public QObject
{
  Q_OBJECT
  static CatalogAsset asset(const QString &id)
  {
    CatalogAsset a; a.id = id; a.type = QStringLiteral("image_reference");
    a.format = QStringLiteral("png"); a.displayName = id + QStringLiteral(" 图件"); return a;
  }
  static CatalogVersion version(const QString &id, const QString &assetId, const QStringList &parents = {}, int no = 1)
  {
    CatalogVersion v; v.id = id; v.assetId = assetId; v.versionNumber = no;
    v.stage = parents.isEmpty() ? QStringLiteral("RAW") : QStringLiteral("DERIVED");
    v.parentVersionIds = parents; v.fileName = id + QStringLiteral(".png");
    v.path = DataCatalog::managedPath(v.stage, assetId, id, v.fileName); return v;
  }
  static QGraphicsItem *item(DerivationGraph *view, const QString &id)
  {
    for (QGraphicsItem *it : view->scene()->items())
      if (it->zValue() == 1 && it->data(0).toString() == id) return it;
    return nullptr;
  }
  static void click(DerivationGraph *view, const QString &id)
  {
    QGraphicsItem *node = item(view, id);
    Q_ASSERT(node);
    view->ensureVisible(node);
    const QPoint at = view->mapFromScene(node->scenePos() + QPointF(80, 38));
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, at);
  }
  static QSet<QString> ids(const Graph &g)
  {
    QSet<QString> out; for (const Node &n : g.nodes) out.insert(n.versionId); return out;
  }
  static bool add(DataCatalog &cat, const QString &id, const QStringList &parents = {})
  {
    return cat.addAsset(asset(id)) && cat.addVersion(version(id, id, parents));
  }
  static bool saveImage(const QString &dir, const CatalogVersion &v, const QColor &color)
  {
    const QString path = DataCatalog::resolvedVersionPath(dir, v);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QImage img(24, 24, QImage::Format_RGB32); img.fill(color); return img.save(path);
  }
  static bool screenshot(QWidget *widget, const QString &name)
  {
    const QString dir = qEnvironmentVariable("PALEO_DERIVATION_SCREENSHOTS");
    if (dir.isEmpty()) return true;
    QDir().mkpath(dir);
    if (auto *view = qobject_cast<DerivationGraph *>(widget))
    {
      QImage rendered(900, 450, QImage::Format_ARGB32); rendered.fill(PaleoTheme::tokens().surface);
      QPainter painter(&rendered); painter.setRenderHint(QPainter::Antialiasing);
      view->scene()->render(&painter, QRectF(16, 16, 868, 418), view->sceneRect()); painter.end();
      return rendered.save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
    widget->resize(900, 450); widget->show(); QApplication::processEvents();
    return widget->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
  }
private slots:
  void initTestCase() { PaleoTheme::pinRenderEnvironment(); }
  void chainAndExactVersionPreview()
  {
    QTemporaryDir dir;
    DataImportService import(nullptr); import.setProjectDir(dir.path());
    DataCatalog *cat = import.catalog(); QVERIFY(cat->isOpen());
    QVERIFY(cat->addAsset(asset(QStringLiteral("a"))));
    const auto raw = version(QStringLiteral("raw"), QStringLiteral("a"));
    const auto d1 = version(QStringLiteral("d1"), QStringLiteral("a"), {raw.id}, 2);
    const auto d2 = version(QStringLiteral("d2"), QStringLiteral("a"), {d1.id}, 3);
    QVERIFY(cat->addVersion(raw)); QVERIFY(cat->addVersion(d1)); QVERIFY(cat->addVersion(d2));
    QVERIFY(saveImage(dir.path(), raw, QColor(220, 40, 40)));
    QVERIFY(saveImage(dir.path(), d1, QColor(40, 220, 40)));
    QVERIFY(saveImage(dir.path(), d2, QColor(40, 40, 220)));
    Query q; q.versionId = d1.id;
    const Graph graph = Service::build(cat, q);
    QCOMPARE(graph.nodes.size(), 3); QCOMPARE(graph.edges.size(), 2);
    QCOMPARE(graph.edges.at(0).parentId.isEmpty(), false);
    bool rawEdge = false, dEdge = false;
    for (const Edge &e : graph.edges) { rawEdge |= e.parentId == raw.id && e.childId == d1.id; dEdge |= e.parentId == d1.id && e.childId == d2.id; }
    QVERIFY(rawEdge); QVERIFY(dEdge);
    DerivationGraph chainView; chainView.loadGraph(graph); QVERIFY(screenshot(&chainView, QStringLiteral("chain")));
    DataPage page; PreviewDocService doc(&import);
    page.setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(&doc));
    page.selectAsset(QStringLiteral("a"));
    DataPreviewTabs preview; preview.setDocService(&doc);
    connect(&page, &DataPage::versionActivated, &preview, &DataPreviewTabs::openVersion);
    connect(&preview, &DataPreviewTabs::versionContextChanged, &page, &DataPage::focusVersion);
    QSignalSpy activated(&page, &DataPage::versionActivated);
    auto *view = page.findChild<DerivationGraph *>(); QVERIFY(view);
    view->resize(720, 350); view->show();
    // 实体面板在测试 page 未 show 时隐藏；脱离布局给真实点击事件可见视口。
    view->setParent(nullptr); view->show();
    click(view, raw.id);
    QTRY_COMPARE(activated.count(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), raw.id);
    auto *tabs = preview.findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")); QVERIFY(tabs);
    QWidget *p = tabs->currentWidget(); QVERIFY(p);
    QCOMPARE(p->property("previewVersionId").toString(), raw.id);
    QCOMPARE(preview.versionIdAt(tabs->currentIndex()), raw.id);
    bool redImage = false;
    for (QLabel *label : p->findChildren<QLabel *>())
      if (!label->pixmap().isNull()) redImage |= label->pixmap().toImage().pixelColor(10, 10) == QColor(220, 40, 40);
    QVERIFY(redImage); // 历史 RAW 字节真实进预览，而不是最新版 D2。
    preview.openVersion(d2.id);
    QCOMPARE(tabs->currentWidget()->property("previewVersionId").toString(), d2.id);
    bool blueImage = false;
    for (QLabel *label : tabs->currentWidget()->findChildren<QLabel *>())
      if (!label->pixmap().isNull()) blueImage |= label->pixmap().toImage().pixelColor(10, 10) == QColor(40, 40, 220);
    QVERIFY(blueImage);
    delete view;
  }
  void realShellRestoresHistoricalVersionOnReselection()
  {
    QTemporaryDir dir; DataImportService import(nullptr); import.setProjectDir(dir.path());
    DataCatalog *cat = import.catalog(); QVERIFY(cat->addAsset(asset(QStringLiteral("a"))));
    const CatalogVersion raw = version(QStringLiteral("raw"), QStringLiteral("a"));
    const CatalogVersion d1 = version(QStringLiteral("d1"), QStringLiteral("a"), {raw.id}, 2);
    const CatalogVersion d2 = version(QStringLiteral("d2"), QStringLiteral("a"), {d1.id}, 3);
    for (const CatalogVersion &v : {raw, d1, d2}) { QVERIFY(cat->addVersion(v)); QVERIFY(saveImage(dir.path(), v, QColor(80, 100, 140))); }
    QVERIFY(add(*cat, QStringLiteral("other"))); QVERIFY(saveImage(dir.path(), cat->currentVersion(QStringLiteral("other")), QColor(100, 140, 80)));
    for (const QString &id : {QStringLiteral("a"), QStringLiteral("other")})
    {
      CatalogEntity entity; entity.id = id + QStringLiteral("-well"); entity.entityType = QStringLiteral("well");
      entity.name = entity.id; QVERIFY(cat->addEntity(entity));
      EntityAssetLink link; link.assetId = id; link.entityId = entity.id; link.entityType = entity.entityType;
      link.role = QStringLiteral("other"); QVERIFY(cat->addLink(link));
    }
    PaleoMainWindow window(nullptr, nullptr, nullptr, nullptr, nullptr);
    window.attachWorkflows(nullptr, nullptr, nullptr, nullptr, &import);
    auto *page = window.findChild<DataPage *>(); auto *preview = window.findChild<DataPreviewTabs *>();
    QVERIFY(page); QVERIFY(preview);
    auto *graph = window.findChild<DerivationGraph *>(); QVERIFY(graph);
    page->selectAsset(QStringLiteral("other"));
    preview->openVersion(raw.id);
    QCOMPARE(page->property("paleo.page.entityId").toString(), QStringLiteral("a-well"));
    QCOMPARE(page->property("paleo.page.assetId").toString(), QStringLiteral("a"));
    QVERIFY(item(graph, raw.id)); QCOMPARE(item(graph, raw.id)->scenePos().x(), 0.0);
    page->assetActivated(QStringLiteral("a")); // 真实主窗接线；已有标签保留 RAW。
    QCOMPARE(item(graph, raw.id)->scenePos().x(), 0.0);
    // 外部再次定位同 id 必须取新事实，不能误用图内点击的一次保留。
    const QString refreshedReason = QStringLiteral("当前血缘源已更新");
    QVERIFY(cat->markDownstreamStale(raw.id, refreshedReason));
    preview->openVersion(raw.id);
    QVERIFY(item(graph, d1.id)->toolTip().contains(refreshedReason));
    preview->openAsset(QStringLiteral("other"));
    QCOMPARE(page->property("paleo.page.entityId").toString(), QStringLiteral("other-well"));
    auto *tabs = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")); QVERIFY(tabs);
    for (int i = 0; i < preview->tabCount(); ++i)
      if (preview->assetIdAt(i) == QStringLiteral("a")) tabs->setCurrentIndex(i);
    QCOMPARE(preview->versionIdAt(tabs->currentIndex()), raw.id);
    QCOMPARE(page->property("paleo.page.entityId").toString(), QStringLiteral("a-well"));
    QVERIFY(item(graph, raw.id)); QCOMPARE(item(graph, raw.id)->scenePos().x(), 0.0);
    PreviewDocService doc(&import);
    auto *transient = new EntityPanel; transient->setDocService(&doc); transient->setContext(QString(), QStringLiteral("a"));
    const QPointer<EntityPanel> lifetime(transient);
    connect(transient, &EntityPanel::versionActivated, &doc, [transient] { delete transient; });
    transient->findChild<DerivationPanel *>()->nodeClicked(raw.id); // 定位接收者同步销毁图宿主。
    QVERIFY(lifetime.isNull());
  }
  void historicalOfficePreviewIsHonest()
  {
    QTemporaryDir dir; DataImportService import(nullptr); import.setProjectDir(dir.path());
    CatalogAsset a = asset(QStringLiteral("office")); a.type = QStringLiteral("document"); a.format = QStringLiteral("docx");
    QVERIFY(import.catalog()->addAsset(a));
    CatalogVersion raw = version(QStringLiteral("original"), a.id); raw.fileName = QStringLiteral("original.docx");
    raw.path = DataCatalog::managedPath(raw.stage, a.id, raw.id, raw.fileName);
    QVERIFY(import.catalog()->addVersion(raw));
    const QString path = DataCatalog::resolvedVersionPath(dir.path(), raw);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath())); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
    PreviewDocService doc(&import); DataPreviewTabs preview; preview.setDocService(&doc); preview.openVersion(raw.id);
    auto *tabs = preview.findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")); QVERIFY(tabs);
    QCOMPARE(preview.versionIdAt(tabs->currentIndex()), raw.id);
    QVERIFY(tabs->currentWidget()->findChild<OfficePreviewWidget *>());
  }
  void forkMergeSelectionAndDedup()
  {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); DataCatalog::BatchSave batch(&cat);
    QVERIFY(add(cat, QStringLiteral("r"))); QVERIFY(add(cat, QStringLiteral("a"), {QStringLiteral("r")}));
    QVERIFY(add(cat, QStringLiteral("b"), {QStringLiteral("r")}));
    QVERIFY(add(cat, QStringLiteral("merge"), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a")}));
    Query q; q.versionId = QStringLiteral("r"); const Graph g = Service::build(&cat, q);
    QCOMPARE(g.nodes.size(), 4); QCOMPARE(g.edges.size(), 4);
    const auto selected = Service::selectionClosure(&cat, g, QStringLiteral("a"));
    QCOMPARE(selected, (QSet<QString>{QStringLiteral("r"), QStringLiteral("a"), QStringLiteral("merge")}));
    DerivationGraph view; Graph duplicate = g; duplicate.nodes << g.nodes.first(); duplicate.edges << g.edges.first();
    view.loadGraph(duplicate); QCOMPARE(view.nodeCount(), 4);
    view.highlight(QStringLiteral("a"), selected);
    QVERIFY(screenshot(&view, QStringLiteral("fork-merge")));
    QCOMPARE(item(&view, QStringLiteral("b"))->opacity(), 0.25);
    QCOMPARE(item(&view, QStringLiteral("merge"))->opacity(), 1.0);
    QVERIFY(item(&view, QStringLiteral("r"))->scenePos().x() < item(&view, QStringLiteral("a"))->scenePos().x());
    QVERIFY(item(&view, QStringLiteral("a"))->scenePos().x() < item(&view, QStringLiteral("merge"))->scenePos().x());
  }
  void staleReasonFiltersAndLiveTheme()
  {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path()));
    QVERIFY(add(cat, QStringLiteral("r"))); QVERIFY(add(cat, QStringLiteral("d"), {QStringLiteral("r")}));
    const QString reason = QStringLiteral("上游外链版本 sha 校验失败");
    QVERIFY(cat.markDownstreamStale(QStringLiteral("r"), reason));
    const auto report = paleo::health::buildCatalogHealth(&cat, dir.path());
    QCOMPARE(report.count(paleo::health::IssueKind::StaleVersion), 1);
    for (const auto &issue : report.issues)
      if (issue.kind == paleo::health::IssueKind::StaleVersion)
      { QCOMPARE(issue.versionId, QStringLiteral("d")); QCOMPARE(issue.detail, reason); }
    CatalogHealthDialog health;
    health.setReport(report, -1, 0);
    auto *categories = health.findChild<QListWidget *>(QStringLiteral("healthCategories"));
    QVERIFY(categories);
    for (int i = 0; i < categories->count(); ++i)
      if (categories->item(i)->text().startsWith(QStringLiteral("过时版本"))) categories->setCurrentRow(i);
    auto *issues = health.findChild<QTableWidget *>(QStringLiteral("healthIssues"));
    QCOMPARE(issues->rowCount(), 1);
    QSignalSpy jump(&health, &CatalogHealthDialog::jumpToVersion);
    issues->cellDoubleClicked(0, 0);
    QCOMPARE(jump.count(), 1); QCOMPARE(jump.at(0).at(0).toString(), QStringLiteral("d"));

    Query q; q.versionId = QStringLiteral("r"); q.staleOnly = true;
    const Graph g = Service::build(&cat, q); QCOMPARE(g.nodes.size(), 2);
    DerivationPanel panel; panel.setGraph(g); panel.resize(780, 640); panel.show();
    auto *view = panel.graphView(); QVERIFY(item(view, QStringLiteral("d"))->toolTip().contains(reason));
    QVERIFY(item(view, QStringLiteral("d"))->data(2).toBool());
    QCOMPARE(item(view, QStringLiteral("r"))->data(1).toInt(), int(Kind::Raw));
    QCOMPARE(item(view, QStringLiteral("d"))->data(1).toInt(), int(Kind::Derived));
    QSignalSpy changed(&panel, &DerivationPanel::queryChanged);
    panel.findChild<QCheckBox *>(QStringLiteral("derivationStaleOnly"))->setChecked(true); QCOMPARE(changed.count(), 1);
    panel.findChild<QLineEdit *>(QStringLiteral("derivationAssetFilter"))->setText(QStringLiteral("d"));
    QCOMPARE(panel.query().assetFilter, QStringLiteral("d"));
    q.kindFilter = int(Kind::Raw); QCOMPARE(Service::build(&cat, q).nodes.size(), 1);
    q.kindFilter = int(Kind::External); QVERIFY(Service::build(&cat, q).nodes.isEmpty());
    PaleoTheme::applyDarkTheme(); QApplication::processEvents();
    QCOMPARE(view->backgroundBrush().color(), PaleoTheme::tokens().surface);
    const auto warningPixels = [view]() {
      QImage render(160, 112, QImage::Format_ARGB32); render.fill(PaleoTheme::tokens().surface);
      QPainter painter(&render);
      view->scene()->render(&painter, QRectF(0, 0, 160, 112), item(view, QStringLiteral("d"))->sceneBoundingRect());
      painter.end();
      int pixels = 0;
      for (int y = 0; y < render.height(); ++y)
        for (int x = 0; x < render.width(); ++x)
          if (render.pixelColor(x, y) == PaleoTheme::tokens().warning) ++pixels;
      return pixels;
    };
    QVERIFY(warningPixels() > 20); // 真正画出当前主题 warning 描边，不只测 DTO stale 位。

    PaleoTheme::applyLightTheme();
    QVERIFY(warningPixels() > 20);
    if (!qEnvironmentVariableIsEmpty("PALEO_DERIVATION_SCREENSHOTS"))
    {
      const QString dirName = qEnvironmentVariable("PALEO_DERIVATION_SCREENSHOTS"); QDir().mkpath(dirName);
      panel.findChild<QLineEdit *>(QStringLiteral("derivationAssetFilter"))->clear();
      panel.setGraph(g); view->fitGraph();
      QVERIFY(panel.grab().save(dirName + QStringLiteral("/lineage-light.png")));
      PaleoTheme::applyDarkTheme(); QApplication::processEvents();
      QVERIFY(panel.grab().save(dirName + QStringLiteral("/lineage-dark.png"))); PaleoTheme::applyLightTheme();
    }
  }
  void largeGraphHonestCountsAndDepthControl()
  {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); DataCatalog::BatchSave batch(&cat);
    QVERIFY(add(cat, QStringLiteral("root")));
    for (int i = 1; i < 240; ++i)
      QVERIFY(add(cat, QStringLiteral("n%1").arg(i, 3, 10, QLatin1Char('0')), {i == 1 ? QStringLiteral("root") : QStringLiteral("n%1").arg(i - 1, 3, 10, QLatin1Char('0'))}));
    Query q; q.versionId = QStringLiteral("root");
    QElapsedTimer sample; sample.start();
    const Graph g = Service::build(&cat, q);
    DerivationPanel panel; panel.setGraph(g);
    qInfo() << "240-version default build/render sample ns:" << sample.nsecsElapsed(); // 抽样观测，不设绝对墙钟断言。
    QCOMPARE(g.nodes.size(), 3); QCOMPARE(g.collapsedDownstream, 237); QCOMPARE(g.collapsedUpstream, 0);
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("derivationSummary"))->text().contains(QStringLiteral("237")));
    panel.findChild<QSlider *>(QStringLiteral("derivationDownstreamDepth"))->setValue(3);
    Query next = panel.query(); next.versionId = q.versionId;
    const Graph expanded = Service::build(&cat, next); QCOMPARE(expanded.nodes.size(), 4); QCOMPARE(expanded.collapsedDownstream, 236);
    q.versionId = QStringLiteral("n120"); const Graph middle = Service::build(&cat, q);
    QCOMPARE(middle.nodes.size(), 5); QCOMPARE(middle.collapsedUpstream, 118); QCOMPARE(middle.collapsedDownstream, 117);
    // 广分叉命中节点阈值；计数加总必须等于真实版本数，过滤不算折叠。
    QVERIFY(add(cat, QStringLiteral("wide")));
    for (int i = 0; i < 200; ++i) QVERIFY(add(cat, QStringLiteral("w%1").arg(i), {QStringLiteral("wide")}));
    q.versionId = QStringLiteral("wide"); const Graph wide = Service::build(&cat, q);
    QCOMPARE(wide.nodes.size(), 80); QCOMPARE(wide.collapsedDownstream, 121);
    panel.setGraph(wide); QCOMPARE(panel.graphView()->nodeCount(), 80);
    QImage frame(900, 450, QImage::Format_ARGB32); frame.fill(PaleoTheme::tokens().surface);
    QElapsedTimer paintSample; paintSample.start(); QPainter painter(&frame);
    panel.graphView()->scene()->render(&painter, QRectF(0, 0, 900, 450), panel.graphView()->scene()->sceneRect());
    painter.end();
    qInfo() << "80-node capped scene paint sample ns:" << paintSample.nsecsElapsed();

    q.assetFilter = QStringLiteral("missing"); const Graph filtered = Service::build(&cat, q);
    QCOMPARE(filtered.filtered, 201); QCOMPARE(filtered.collapsedDownstream, 0);
  }
  void cyclesMissingSourcesAndEmptyStates()
  {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); DataCatalog::BatchSave batch(&cat);
    QVERIFY(add(cat, QStringLiteral("a"), {QStringLiteral("c")}));
    QVERIFY(add(cat, QStringLiteral("b"), {QStringLiteral("a")}));
    QVERIFY(add(cat, QStringLiteral("c"), {QStringLiteral("b")}));
    QVERIFY(add(cat, QStringLiteral("self"), {QStringLiteral("self")}));
    Query q; q.versionId = QStringLiteral("a"); Graph g = Service::build(&cat, q);
    QCOMPARE(g.nodes.size(), 3); QCOMPARE(g.edges.size(), 3); QCOMPARE(ids(g).size(), 3);
    QCOMPARE(Service::selectionClosure(&cat, g, q.versionId).size(), 3);
    DerivationPanel panel; panel.setGraph(g); QCOMPARE(panel.graphView()->nodeCount(), 3);
    QVERIFY(screenshot(panel.graphView(), QStringLiteral("cycle")));
    // 排队点击遇到同 id 的 scene 重建也不能错误定位到新一代节点。
    DerivationGraph eventView; eventView.resize(700, 320); eventView.show(); eventView.loadGraph(g);
    QSignalSpy clicks(&eventView, &DerivationGraph::nodeClicked);
    click(&eventView, q.versionId); eventView.loadGraph(g); QApplication::processEvents(); QCOMPARE(clicks.count(), 0);
    q.versionId = QStringLiteral("self"); g = Service::build(&cat, q); QCOMPARE(g.nodes.size(), 1); QCOMPARE(g.edges.size(), 1);
    panel.setGraph(g);
    QVERIFY(add(cat, QStringLiteral("isolated"))); q.versionId = QStringLiteral("isolated");
    g = Service::build(&cat, q); QCOMPARE(g.message, QStringLiteral("该版本无衍生记录"));
    QCOMPARE(g.nodes.size(), 1); panel.setGraph(g);
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("derivationSummary"))->text().contains(g.message));
    QVERIFY(screenshot(&panel, QStringLiteral("empty")));
    g = Service::build(nullptr, q); QVERIFY(!g.available); panel.setGraph(g);
    QVERIFY(!panel.findChild<QSlider *>()->isEnabled()); QVERIFY(!panel.graphView()->isVisible());
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("derivationSummary"))->text().contains(QStringLiteral("工程未打开")));
    QVERIFY(add(cat, QStringLiteral("dangling"), {QStringLiteral("unknown")})); q.versionId = QStringLiteral("dangling");
    g = Service::build(&cat, q); QCOMPARE(g.nodes.size(), 1); QCOMPARE(g.missingParents, 1);
  }
  void entityScopeAndExternalKind()
  {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); DataCatalog::BatchSave batch(&cat);
    CatalogEntity entity; entity.id = QStringLiteral("well"); entity.name = QStringLiteral("井甲"); entity.entityType = QStringLiteral("well");
    QVERIFY(cat.addEntity(entity)); QVERIFY(add(cat, QStringLiteral("one"))); QVERIFY(add(cat, QStringLiteral("two")));
    for (const QString &id : {QStringLiteral("one"), QStringLiteral("two")})
    { EntityAssetLink link; link.assetId = id; link.entityId = entity.id; link.entityType = entity.entityType; link.role = QStringLiteral("other"); QVERIFY(cat.addLink(link)); }
    Query q; q.entityId = entity.id; Graph g = Service::build(&cat, q); QCOMPARE(g.nodes.size(), 2); QCOMPARE(g.edges.size(), 0);
    DerivationGraph view; view.loadGraph(g);
    QCOMPARE(item(&view, QStringLiteral("one"))->scenePos().x(), item(&view, QStringLiteral("two"))->scenePos().x());
    QVERIFY(item(&view, QStringLiteral("one"))->scenePos().y() != item(&view, QStringLiteral("two"))->scenePos().y());
    q.entityFilter = entity.id; QCOMPARE(Service::build(&cat, q).nodes.size(), 2);
    QVERIFY(cat.addAsset(asset(QStringLiteral("external"))));
    CatalogVersion ext = version(QStringLiteral("ext"), QStringLiteral("external")); ext.managed = false; ext.path = dir.filePath(QStringLiteral("outside.png")); QVERIFY(cat.addVersion(ext));
    q = Query{}; q.versionId = ext.id; q.kindFilter = int(Kind::External);
    g = Service::build(&cat, q); QCOMPARE(g.nodes.size(), 1); QCOMPARE(g.nodes.first().kind, Kind::External);
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")))) return 1;
  TestDerivationGraph test; const int rc = QTest::qExec(&test, argc, argv); QgisRuntime::shutdown(); return rc;
}
#include "tst_derivationgraph.moc"
