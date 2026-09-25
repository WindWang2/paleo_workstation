#include <QtTest>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QListWidget>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

#include "../src/ui/seismicpreviewpanel.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/linkage/seismicmaplink.h"
#include "../src/qgis/qgisruntime.h"

// 地震剖面预览 (seismic section preview) — the data page's linked sub-panel.
// §42.4: never a blank panel — idle and populated states both render a
// visible scene. Selection routes through SelectionContext (origin
// "seismicpanel"); SeismicMapLink applies it to the line layer. Real link +
// real context objects over an in-memory line layer — offscreen, but the
// memory provider still needs initQgis, hence QgisRuntime in main().
class TestSeismicPanel : public QObject
{
  Q_OBJECT

  private:
    // Two seismic lines "L1"/"L2" on an in-memory layer — SeismicMapLink
    // resolves attributes only, but trivial geometries keep the provider
    // honest.
    static QgsVectorLayer *makeLineLayer(QObject *parent)
    {
      auto *layer = new QgsVectorLayer(
          QStringLiteral("LineString?crs=EPSG:4326&field=line_id:string"),
          QStringLiteral("seismic"), QStringLiteral("memory"));
      layer->setParent(parent);

      QgsFeature f1(layer->fields());
      f1.setAttribute(QStringLiteral("line_id"), QStringLiteral("L1"));
      f1.setGeometry(QgsGeometry::fromPolylineXY({QgsPointXY(0, 0), QgsPointXY(10, 0)}));
      QgsFeature f2(layer->fields());
      f2.setAttribute(QStringLiteral("line_id"), QStringLiteral("L2"));
      f2.setGeometry(QgsGeometry::fromPolylineXY({QgsPointXY(0, 1), QgsPointXY(10, 1)}));
      QgsFeatureList feats;
      feats << f1 << f2;
      layer->dataProvider()->addFeatures(feats); // non-const ref — writes back fids
      layer->updateExtents();
      return layer;
    }

    static QgsFeatureId fidOf(QgsVectorLayer *layer, const QString &lineId)
    {
      const int idx = layer->fields().indexOf(QStringLiteral("line_id"));
      QgsFeature f;
      QgsFeatureIterator it = layer->getFeatures();
      while (it.nextFeature(f))
        if (f.attribute(idx).toString() == lineId)
          return f.id();
      return -1;
    }

    // Binds the context the link was constructed with — the documented
    // "paleo.seismic.ctx" dynamic property (same idiom as pagepanels'
    // kLayersProp service bindings).
    static SeismicMapLink *bindLink(SelectionContext *ctx, QgsVectorLayer *layer,
                                    QObject *parent)
    {
      auto *link = new SeismicMapLink(nullptr, ctx, parent);
      link->setProperty("paleo.seismic.ctx", QVariant::fromValue(static_cast<QObject *>(ctx)));
      if (layer)
        link->setSeismicLayer(layer, QStringLiteral("line_id"));
      return link;
    }

    static QGraphicsScene *sceneOf(SeismicPreviewPanel &panel)
    {
      auto *v = panel.findChild<QGraphicsView *>(QStringLiteral("seismicPreview"));
      return v ? v->scene() : nullptr;
    }

  private slots:
    // Empty panel: guidance label visible, list empty, preview already
    // renders a non-blank idle scene. Adding an asset hides the label.
    void emptyPanelShowsGuidance()
    {
      SeismicPreviewPanel panel(nullptr);
      panel.resize(720, 400);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      auto *empty = panel.findChild<QLabel *>(QStringLiteral("seismicEmptyLabel"));
      QVERIFY(empty);
      QVERIFY(empty->isVisible());

      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      QVERIFY(list);
      QCOMPARE(list->count(), 0);

      auto *scene = sceneOf(panel);
      QVERIFY(scene);
      QVERIFY(!scene->items().isEmpty()); // §42.4 — idle scene must not be blank

      panel.addSeismicAsset(QStringLiteral("L1"), QStringLiteral("测线L1"));
      QVERIFY(!empty->isVisible());
      QCOMPARE(list->count(), 1);
    }

    // addSeismicAsset appends labelled rows; setSeismicAssets replaces.
    void assetsPopulateList()
    {
      SeismicPreviewPanel panel(nullptr);
      panel.addSeismicAsset(QStringLiteral("L1"), QStringLiteral("测线1"));
      panel.addSeismicAsset(QStringLiteral("L2"), QStringLiteral("测线2"));

      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      QVERIFY(list);
      QCOMPARE(list->count(), 2);
      QCOMPARE(list->item(0)->text(), QStringLiteral("测线1"));
      QCOMPARE(list->item(1)->text(), QStringLiteral("测线2"));
      QCOMPARE(panel.assetCount(), 2);

      panel.setSeismicAssets({{QStringLiteral("A"), QStringLiteral("线A")},
                              {QStringLiteral("B"), QStringLiteral("线B")},
                              {QStringLiteral("C"), QStringLiteral("线C")}});
      QCOMPARE(list->count(), 3);
      QCOMPARE(panel.assetCount(), 3);
      QCOMPARE(list->item(2)->text(), QStringLiteral("线C"));
    }

    // Row click: panel emits seismicSelected AND the pick broadcasts on the
    // SelectionContext (origin "seismicpanel") so the real SeismicMapLink
    // applies it to the line layer (direction B).
    void rowClickSelectsThroughLink()
    {
      SelectionContext ctx;
      auto *layer = makeLineLayer(&ctx);
      auto *link = bindLink(&ctx, layer, &ctx);
      QCOMPARE(link->seismicLayer(), layer);

      SeismicPreviewPanel panel(link);
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")},
                              {QStringLiteral("L2"), QStringLiteral("测线L2")}});
      panel.resize(720, 400);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));

      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      QVERIFY(list);

      QSignalSpy ctxSpy(&ctx, &SelectionContext::selectionChanged);
      QSignalSpy selSpy(&panel, &SeismicPreviewPanel::seismicSelected);

      auto *item = list->item(1);
      QVERIFY(item);
      QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                        list->visualItemRect(item).center());

      QCOMPARE(selSpy.count(), 1);
      QCOMPARE(selSpy.first().at(0).toString(), QStringLiteral("L2"));

      QCOMPARE(ctxSpy.count(), 1); // one broadcast, no echo storm
      QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("L2")}));
      QCOMPARE(ctx.origin(), QStringLiteral("seismicpanel"));

      // The link received the selection: direction-B apply selected L2's feature.
      const QgsFeatureId fid = fidOf(layer, QStringLiteral("L2"));
      QVERIFY(fid >= 0);
      QCOMPARE(layer->selectedFeatureIds().size(), 1);
      QVERIFY(layer->selectedFeatureIds().contains(fid));

      QCOMPARE(panel.currentAsset(), QStringLiteral("L2"));
    }

    // Foreign-origin selection (canvas pick, tree, well panel) re-renders the
    // preview: item count grows past the idle scaffold and the list row syncs.
    // Deliberately NOT shown — a shown QListWidget auto-selects row 0, which
    // would pollute the idle baseline (Qt's view restores a current index).
    void externalSelectionRerenders()
    {
      SelectionContext ctx;
      auto *layer = makeLineLayer(&ctx);
      auto *link = bindLink(&ctx, layer, &ctx);

      SeismicPreviewPanel panel(link);
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")},
                              {QStringLiteral("L2"), QStringLiteral("测线L2")}});

      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      auto *scene = sceneOf(panel);
      QVERIFY(scene);
      const int idleItems = scene->items().size();
      QVERIFY(idleItems > 0);

      ctx.setSelection({QStringLiteral("L1")}, QStringLiteral("tree"));
      QCOMPARE(panel.currentAsset(), QStringLiteral("L1"));
      QCOMPARE(list->currentRow(), 0);
      QVERIFY(scene->items().size() > idleItems); // profile scene is richer
      QVERIFY(layer->selectedFeatureIds().contains(fidOf(layer, QStringLiteral("L1"))));

      ctx.setSelection({QStringLiteral("L2")}, QStringLiteral("tree"));
      QCOMPARE(panel.currentAsset(), QStringLiteral("L2"));
      QCOMPARE(list->currentRow(), 1);

      // Selection naming no known asset → back to the idle scaffold.
      ctx.setSelection({QStringLiteral("GHOST")}, QStringLiteral("tree"));
      QCOMPARE(panel.currentAsset(), QString());
      QCOMPARE(scene->items().size(), idleItems);

      ctx.clear(QStringLiteral("tree"));
      QCOMPARE(panel.currentAsset(), QString());
      QCOMPARE(scene->items().size(), idleItems);
    }

    // A selection broadcast that predates the asset list is honored once the
    // assets land — mirrors WellCorrelationPanel::setWells.
    void selectionAppliesToAssetsSetLater()
    {
      SelectionContext ctx;
      auto *link = bindLink(&ctx, nullptr, &ctx); // no layer needed for this path
      ctx.setSelection({QStringLiteral("L2")}, QStringLiteral("tree"));

      SeismicPreviewPanel panel(link);
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")},
                              {QStringLiteral("L2"), QStringLiteral("测线L2")}});

      QCOMPARE(panel.currentAsset(), QStringLiteral("L2"));
      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      QCOMPARE(list->currentRow(), 1);
    }

    // Rapid re-selection and asset churn must neither crash nor desync.
    void rapidReselectionAndClearingIsSafe()
    {
      SelectionContext ctx;
      auto *layer = makeLineLayer(&ctx);
      auto *link = bindLink(&ctx, layer, &ctx);

      SeismicPreviewPanel panel(link);
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")},
                              {QStringLiteral("L2"), QStringLiteral("测线L2")}});
      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));

      QSignalSpy ctxSpy(&ctx, &SelectionContext::selectionChanged);
      for (int i = 0; i < 20; ++i)
        list->setCurrentRow(i % 2);
      QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("L2")})); // last iteration (i=19) picks row 1
      QCOMPARE(panel.currentAsset(), QStringLiteral("L2"));

      // Re-selecting the already-current row via a foreign origin is a no-op
      // on the signal count but keeps the panel live.
      ctx.setSelection({QStringLiteral("L1")}, QStringLiteral("tree"));
      QCOMPARE(panel.currentAsset(), QStringLiteral("L1"));

      panel.setSeismicAssets({}); // clear while a selection is in flight
      QCOMPARE(list->count(), 0);
      auto *empty = panel.findChild<QLabel *>(QStringLiteral("seismicEmptyLabel"));
      // Never-shown window: isVisible() stays false — isHidden() tracks the flag.
      QVERIFY(!empty->isHidden());
      QVERIFY(!sceneOf(panel)->items().isEmpty()); // idle scaffold, never blank

      // Churn: re-add, re-select, clear again — still no crash.
      panel.addSeismicAsset(QStringLiteral("L3"), QStringLiteral("测线L3"));
      list->setCurrentRow(0);
      QCOMPARE(panel.currentAsset(), QStringLiteral("L3"));
      panel.setSeismicAssets({});
      panel.setSeismicAssets({{QStringLiteral("L3"), QStringLiteral("测线L3")}});
      ctx.clear(QStringLiteral("tree"));
      QVERIFY(true);
    }

    // Degradation: a link whose context isn't reachable still receives the
    // pick (its onContextSelection slot applies it to the layer) and the
    // panel still emits seismicSelected.
    void unboundLinkStillSelectsLayer()
    {
      SelectionContext ctx;
      auto *layer = makeLineLayer(&ctx);
      SeismicMapLink link(nullptr, &ctx); // no paleo.seismic.ctx property

      SeismicPreviewPanel panel(&link);
      link.setSeismicLayer(layer, QStringLiteral("line_id"));
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")}});
      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));

      QSignalSpy selSpy(&panel, &SeismicPreviewPanel::seismicSelected);
      QSignalSpy ctxSpy(&ctx, &SelectionContext::selectionChanged);
      list->setCurrentRow(0);

      QCOMPARE(selSpy.count(), 1);
      QVERIFY(layer->selectedFeatureIds().contains(fidOf(layer, QStringLiteral("L1"))));
      QCOMPARE(ctxSpy.count(), 0); // hub bypassed — documented degradation
    }

    // Null link: standalone panel still lists, emits and renders.
    void nullLinkIsSafe()
    {
      SeismicPreviewPanel panel(nullptr);
      panel.setSeismicAssets({{QStringLiteral("L1"), QStringLiteral("测线L1")}});
      auto *list = panel.findChild<QListWidget *>(QStringLiteral("seismicList"));
      QSignalSpy selSpy(&panel, &SeismicPreviewPanel::seismicSelected);
      list->setCurrentRow(0);
      QCOMPARE(selSpy.count(), 1);
      QCOMPARE(panel.currentAsset(), QStringLiteral("L1"));
      QVERIFY(!sceneOf(panel)->items().isEmpty());
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  // Offscreen QGIS bootstrap through the runtime that owns init order —
  // QgsApplication is a QApplication, so widgets work under it.
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestSeismicPanel tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_seismicpanel.moc"
