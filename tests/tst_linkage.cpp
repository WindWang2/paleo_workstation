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
