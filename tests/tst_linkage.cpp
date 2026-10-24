#include <QtTest>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfeatureid.h>
#include <qgsfeaturerequest.h>
#include <qgsvectorlayer.h>

#include "../src/linkage/selectioncontext.h"
#include "../src/linkage/wellmaplink.h"

// linkage/ acceptance (§41.3 broadcast hub + §31 well–map linkage).
// Fixture: testdata/golden.gpkg 'wells' — fid 1..3, id="1..3", well="W1..W3".
// The well-name field is the link id field, so setWellLayer uses "well".

static QString goldenGpkg()
{
#ifdef GOLDEN_GPKG
  return QStringLiteral(GOLDEN_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/golden.gpkg"));
#endif
}

static QString wellsSource()
{
  return goldenGpkg() + QStringLiteral("|layername=wells");
}

class TestLinkage : public QObject
{
  Q_OBJECT
private slots:
  void typedTargetsKeepLegacySelectionAndRejectOtherHorizon()
  {
    SelectionContext context;
    context.setActiveHorizon(QStringLiteral("D61"));
    context.setSelection({QStringLiteral("W1")}, QStringLiteral("well"));
    SelectionTarget target;
    target.projectId = "project"; target.horizon = "D61";
    target.layerId = "product.version"; target.featureUid = "stable-feature";
    QSignalSpy changed(&context, &SelectionContext::targetChanged);
    QSignalSpy legacy(&context, &SelectionContext::selectionChanged);
    context.setTarget(target, "map");
    context.setTarget(target, "graph");
    QCOMPARE(changed.count(), 1);
    QCOMPARE(legacy.count(), 0);
    QCOMPARE(context.selectedIds(), QStringList{"W1"});
    QCOMPARE(context.targetGeneration(), quint64(1));
    context.setActiveHorizon("D62");
    QVERIFY(context.target().isEmpty());
    QCOMPARE(context.targetGeneration(), quint64(2));
    context.setTarget(target, "late response");
    QVERIFY(context.target().isEmpty());
    QCOMPARE(changed.count(), 2);
  }
  void typedTargetsCoalesceLatestAndBoundEcho()
  {
    SelectionContext context;
    int count = 0;
    const auto connection = connect(&context, &SelectionContext::targetChanged,
        &context, [&](const SelectionTarget &, const QString &, quint64) {
      QVERIFY(context.broadcasting());
      SelectionTarget next; next.evidenceId = QString::number(++count);
      context.setTarget(next, "echo");
    });
    QTest::ignoreMessage(QtWarningMsg,
        "SelectionContext::setTarget: settle iteration limit reached, dropped runaway pending selection");
    SelectionTarget target; target.featureUid = "start";
    context.setTarget(target, "map");
    QCOMPARE(count, 5);
    QVERIFY(!context.broadcasting());
    disconnect(connection);
    context.clearTarget("close");
    QVERIFY(context.target().isEmpty());
  }
  void horizonAndTargetEchoShareOneBoundedBroadcast()
  {
    SelectionContext context;
    int count = 0;
    const auto connection = connect(&context, &SelectionContext::activeHorizonChanged,
        &context, [&](const QString &) {
      QVERIFY(context.broadcasting());
      context.setActiveHorizon(QString::number(++count));
    });
    QTest::ignoreMessage(QtWarningMsg,
        "SelectionContext::setActiveHorizon: settle iteration limit reached, dropped runaway pending selection");
    context.setActiveHorizon("initial"); QCOMPARE(count, 5);
    QVERIFY(!context.broadcasting()); disconnect(connection);
    context.setActiveHorizon("D61");
    SelectionTarget target; target.horizon = "D61"; target.featureUid = "feature";
    context.setTarget(target, "map");
    connect(&context, &SelectionContext::targetChanged, &context,
        [&](const SelectionTarget &changed, const QString &, quint64) {
      if (changed.isEmpty()) context.setActiveHorizon("D63");
    });
    context.setActiveHorizon("D62");
    QCOMPARE(context.activeHorizon(), QString("D63"));
    QVERIFY(context.target().isEmpty());
  }
  void rebindRestoresBusinessWellSelection()
  {
    SelectionContext context;
    context.setSelection({"W2"}, "graph");
    WellMapLink link(nullptr, &context);
    auto *first = new QgsVectorLayer(wellsSource(), "wells", "ogr");
    QVERIFY(first->isValid());
    link.setWellLayer(first, "well");
    QCOMPARE(first->selectedFeatureIds(), QgsFeatureIds{2});
    delete first;
    QVERIFY(!link.wellLayer());
    QgsVectorLayer reloaded(wellsSource(), "wells", "ogr");
    QSignalSpy changed(&context, &SelectionContext::selectionChanged);
    link.setWellLayer(&reloaded, "well");
    QCOMPARE(reloaded.selectedFeatureIds(), QgsFeatureIds{2});
    QCOMPARE(changed.count(), 0);
  }
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    QVERIFY2(QFile::exists(goldenGpkg()), qPrintable(goldenGpkg()));
  }

  // (a) SelectionContext: a slot that re-sets selection during handling is
  // coalesced — exactly 2 emissions, no recursion, final state = merged payload.
  void coalescedRebroadcast()
  {
    SelectionContext ctx;
    int handled = 0;
    connect(&ctx, &SelectionContext::selectionChanged, &ctx,
            [&ctx, &handled](const QStringList &, const QString &) {
              ++handled;
              if (handled == 1) // re-set only once — unguarded recursion would loop forever
                ctx.setSelection({QStringLiteral("W9")}, QStringLiteral("slot"));
            });
    QSignalSpy spy(&ctx, &SelectionContext::selectionChanged);

    ctx.setSelection({QStringLiteral("W1")}, QStringLiteral("init"));

    QCOMPARE(spy.count(), 2); // initial broadcast + one coalesced settle pass
    QCOMPARE(handled, 2);     // both emissions delivered, depth-bounded
    QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("W9")}));
    QCOMPARE(ctx.origin(), QStringLiteral("slot"));
    QVERIFY(!ctx.broadcasting());
  }

  // PW-062：嵌套 set 回退到当前广播值时，须取消先前的 pending（最后写入者胜）。
  void nestedRevertCancelsEarlierPending()
  {
    SelectionContext ctx;
    bool fired = false;
    connect(&ctx, &SelectionContext::selectionChanged, &ctx,
            [&ctx, &fired](const QStringList &, const QString &) {
              if (fired)
                return;
              fired = true;
              ctx.setSelection({QStringLiteral("B")}, QStringLiteral("x"));
              ctx.setSelection({QStringLiteral("A")}, QStringLiteral("y"));
            });
    ctx.setSelection({QStringLiteral("A")}, QStringLiteral("init"));
    QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("A")}));
    QVERIFY(!ctx.broadcasting());
  }

  void horizonSelectionIsCoalesced()
  {
    SelectionContext ctx;
    QSignalSpy spy(&ctx, &SelectionContext::selectionChanged);
    connect(&ctx, &SelectionContext::activeHorizonChanged, &ctx, [&] {
      QVERIFY(ctx.broadcasting());
      ctx.setSelection({QStringLiteral("W1")}, QStringLiteral("first"));
      ctx.setSelection({QStringLiteral("W2")}, QStringLiteral("last"));
      QCOMPARE(spy.count(), 0);
    });
    ctx.setActiveHorizon(QStringLiteral("D61"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(ctx.selectedIds(), QStringList{QStringLiteral("W2")});
    QCOMPARE(ctx.origin(), QStringLiteral("last"));
    QVERIFY(!ctx.broadcasting());
  }

  void runawaySelectionIsBounded_data()
  {
    QTest::addColumn<bool>("fromHorizon");
    QTest::newRow("selection") << false;
    QTest::newRow("horizon") << true;
  }

  void runawaySelectionIsBounded()
  {
    QFETCH(bool, fromHorizon);
    SelectionContext ctx;
    int broadcasts = 0;
    const auto echo = connect(&ctx, &SelectionContext::selectionChanged, &ctx, [&] {
      QVERIFY(ctx.broadcasting());
      ctx.setSelection({QString::number(++broadcasts)}, QStringLiteral("echo"));
    });
    connect(&ctx, &SelectionContext::activeHorizonChanged, &ctx, [&] {
      ctx.setSelection({QStringLiteral("initial")}, QStringLiteral("horizon"));
    });
    QTest::ignoreMessage(QtWarningMsg, fromHorizon
        ? "SelectionContext::setActiveHorizon: settle iteration limit reached, dropped runaway pending selection"
        : "SelectionContext::setSelection: settle iteration limit reached, dropped runaway pending selection");
    if (fromHorizon)
      ctx.setActiveHorizon(QStringLiteral("D61"));
    else
      ctx.setSelection({QStringLiteral("initial")}, QStringLiteral("test"));
    QCOMPARE(broadcasts, fromHorizon ? 4 : 5);
    QVERIFY(!ctx.broadcasting());

    disconnect(echo);
    QSignalSpy spy(&ctx, &SelectionContext::selectionChanged);
    ctx.setSelection({QStringLiteral("settled")}, QStringLiteral("test"));
    QCOMPARE(spy.count(), 1); // The discarded pending payload must not leak.
    QCOMPARE(ctx.selectedIds(), QStringList{QStringLiteral("settled")});
  }

  // (b) WellMapLink direction B: ctx -> layer. setSelection from a foreign
  // origin selects the matching features on the wells layer.
  void directionBSelectsWells()
  {
    QgsVectorLayer layer(wellsSource(), QStringLiteral("wells"), QStringLiteral("ogr"));
    QVERIFY2(layer.isValid(), qPrintable(layer.error().message()));
    QCOMPARE(layer.featureCount(), 3);

    SelectionContext ctx;
    WellMapLink link(nullptr, &ctx); // direction A hooks the layer; canvas unused
    link.setWellLayer(&layer, QStringLiteral("well"));
    QCOMPARE(link.wellLayer(), &layer);

    ctx.setSelection(QStringList({QStringLiteral("W1"), QStringLiteral("W3")}),
                     QStringLiteral("wellpanel"));

    const QgsFeatureIds sel = layer.selectedFeatureIds();
    QCOMPARE(sel.size(), 2);
    QVERIFY2(sel.contains(1), "W1 (fid 1) must be selected");
    QVERIFY2(!sel.contains(2), "W2 (fid 2) must not be selected");
    QVERIFY2(sel.contains(3), "W3 (fid 3) must be selected");

    QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("W1"), QStringLiteral("W3")}));
    QCOMPARE(ctx.origin(), QStringLiteral("wellpanel"));
  }

  // (c) no ping-pong: applying direction B must not echo back into ctx —
  // no extra selectionChanged beyond the first, selectedIds unchanged.
  void noPingPong()
  {
    QgsVectorLayer layer(wellsSource(), QStringLiteral("wells"), QStringLiteral("ogr"));
    QVERIFY2(layer.isValid(), qPrintable(layer.error().message()));

    SelectionContext ctx;
    WellMapLink link(nullptr, &ctx);
    link.setWellLayer(&layer, QStringLiteral("well"));

    QSignalSpy spy(&ctx, &SelectionContext::selectionChanged);
    ctx.setSelection({QStringLiteral("W2")}, QStringLiteral("wellpanel"));

    QCOMPARE(spy.count(), 1); // layer->selectionChanged must not re-emit into ctx
    QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("W2")}));
    QCOMPARE(ctx.origin(), QStringLiteral("wellpanel"));
    QCOMPARE(layer.selectedFeatureIds().size(), 1);
    QVERIFY(layer.selectedFeatureIds().contains(2));

    // guard holds across repeated applies
    ctx.setSelection({QStringLiteral("W3")}, QStringLiteral("wellpanel"));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(ctx.selectedIds(), QStringList({QStringLiteral("W3")}));
    QCOMPARE(layer.selectedFeatureIds().size(), 1);
    QVERIFY(layer.selectedFeatureIds().contains(3));
  }

  // (d) direction A sanity: a programmatic layer selection resolves fids to
  // well ids and broadcasts with origin "well_map".
  void directionABroadcastsCanvasSelection()
  {
    QgsVectorLayer layer(wellsSource(), QStringLiteral("wells"), QStringLiteral("ogr"));
    QVERIFY2(layer.isValid(), qPrintable(layer.error().message()));

    SelectionContext ctx;
    WellMapLink link(nullptr, &ctx);
    link.setWellLayer(&layer, QStringLiteral("well"));

    QSignalSpy spy(&ctx, &SelectionContext::selectionChanged);
    layer.selectByIds(QgsFeatureIds({1, 3}));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(ctx.origin(), QStringLiteral("well_map"));
    const QStringList ids = ctx.selectedIds();
    QCOMPARE(ids.size(), 2);
    QVERIFY(ids.contains(QStringLiteral("W1")));
    QVERIFY(ids.contains(QStringLiteral("W3")));
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestLinkage tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_linkage.moc"
