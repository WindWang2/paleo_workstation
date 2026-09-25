#include <QtTest>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfeedback.h>
#include <qgsgeometry.h>
#include <qgslocatorcontext.h>
#include <qgsmapcanvas.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>
#include <qgsvectorlayer.h>

// ---------------------------------------------------------------------------
// Compat shim: the frozen src/ui/locator/paleolocatorfilters.h declares
// fetchResults() taking QgsLocatorFeedback*, but QGIS 4.2 renamed that type to
// QgsFeedback (the base-class signature in qgslocatorfilter.h is
// fetchResults(const QString &, const QgsLocatorContext &, QgsFeedback *)).
// Aliasing keeps the header's override declarations valid. Must precede the
// header include in every TU that pulls it in.
// ---------------------------------------------------------------------------
using QgsLocatorFeedback = QgsFeedback;

#include "../src/ui/locator/paleolocatorfilters.h"

// Golden fixture path: prefer build-provided define, else derive from this
// file so standalone g++ builds work too (mirrors tst_layerservice.cpp).
static QString goldenGpkg()
{
#ifdef GOLDEN_GPKG
  return QStringLiteral( GOLDEN_GPKG );
#else
  const QString testsDir = QFileInfo( QString::fromUtf8( __FILE__ ) ).absolutePath();
  return QDir( testsDir ).absoluteFilePath( QStringLiteral( "../testdata/golden.gpkg" ) );
#endif
}

// Drives fetchResults synchronously and collects what the filter emits via
// resultFetched — no QgsLocator/threading needed.
static QList<QgsLocatorResult> runFetch( QgsLocatorFilter *filter, const QString &query )
{
  QList<QgsLocatorResult> results;
  QObject guard;
  QObject::connect( filter, &QgsLocatorFilter::resultFetched, &guard,
                    [&results]( const QgsLocatorResult &r ) { results << r; } );
  QgsLocatorContext context;
  QgsFeedback feedback;
  filter->fetchResults( query, context, &feedback );
  return results;
}

class TestLocator : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void wellFetchFindsW1();
  void wellFetchNoMatch();
  void wellTriggerZoomsAndSelects();
  void horizonFetchMatchesAndTriggers();
  void issueFetchMatchesAndTriggers();
};

void TestLocator::initTestCase()
{
  QVERIFY( QgsApplication::instance() != nullptr );
  QVERIFY2( QFile::exists( goldenGpkg() ),
            qPrintable( QStringLiteral( "fixture missing: %1" ).arg( goldenGpkg() ) ) );

  QgsVectorLayer probe( goldenGpkg() + QStringLiteral( "|layername=wells" ),
                        QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  QVERIFY2( probe.isValid(), "golden.gpkg wells layer did not load" );
  QCOMPARE( probe.featureCount(), 3 );
}

// (a) well filter: query "W1" hits the wells layer's 'well' id field and
// packages fid + wkt into the result's userData.
void TestLocator::wellFetchFindsW1()
{
  QgsVectorLayer layer( goldenGpkg() + QStringLiteral( "|layername=wells" ),
                        QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  QVERIFY( layer.isValid() );

  WellLocatorFilter filter(
    [&layer]() -> QPair<QgsVectorLayer *, QString> {
      return { &layer, QStringLiteral( "well" ) };
    },
    nullptr );

  const QList<QgsLocatorResult> results = runFetch( &filter, QStringLiteral( "W1" ) );
  QCOMPARE( results.size(), 1 );

  const QgsLocatorResult &r = results.at( 0 );
  QCOMPARE( r.filter, static_cast<QgsLocatorFilter *>( &filter ) );
  QCOMPARE( r.displayString, QStringLiteral( "W1" ) );

  const QVariantMap data = r.userData().toMap();
  QVERIFY( data.contains( QStringLiteral( "fid" ) ) );
  QVERIFY( data.contains( QStringLiteral( "wkt" ) ) );

  const QgsFeatureId fid = data.value( QStringLiteral( "fid" ) ).toLongLong();
  const QgsFeature f = layer.getFeature( fid );
  QVERIFY( f.isValid() );
  QCOMPARE( f.attribute( QStringLiteral( "well" ) ).toString(), QStringLiteral( "W1" ) );
  QCOMPARE( f.geometry().asWkt(), data.value( QStringLiteral( "wkt" ) ).toString() );
  QVERIFY( data.value( QStringLiteral( "wkt" ) ).toString().contains( QStringLiteral( "102" ) ) );
}

// (b) non-matching query yields nothing; a broad query yields all wells.
void TestLocator::wellFetchNoMatch()
{
  QgsVectorLayer layer( goldenGpkg() + QStringLiteral( "|layername=wells" ),
                        QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  QVERIFY( layer.isValid() );

  WellLocatorFilter filter(
    [&layer]() -> QPair<QgsVectorLayer *, QString> {
      return { &layer, QStringLiteral( "well" ) };
    },
    nullptr );

  QCOMPARE( runFetch( &filter, QStringLiteral( "ZZZ" ) ).size(), 0 );
  QCOMPARE( runFetch( &filter, QStringLiteral( "W" ) ).size(), 3 );

  // dead provider → no crash, no results
  WellLocatorFilter dead(
    []() -> QPair<QgsVectorLayer *, QString> { return { nullptr, QString() }; },
    nullptr );
  QCOMPARE( runFetch( &dead, QStringLiteral( "W1" ) ).size(), 0 );
}

// (c) triggering a well result selects the feature and zooms the canvas to it.
void TestLocator::wellTriggerZoomsAndSelects()
{
  QgsVectorLayer layer( goldenGpkg() + QStringLiteral( "|layername=wells" ),
                        QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  QVERIFY( layer.isValid() );

  QgsMapCanvas canvas;
  canvas.setDestinationCrs( layer.crs() );
  canvas.setExtent( QgsRectangle( 0, 0, 10, 10 ) );

  WellLocatorFilter filter(
    [&layer]() -> QPair<QgsVectorLayer *, QString> {
      return { &layer, QStringLiteral( "well" ) };
    },
    &canvas );

  const QList<QgsLocatorResult> results = runFetch( &filter, QStringLiteral( "W2" ) );
  QCOMPARE( results.size(), 1 );

  filter.triggerResult( results.at( 0 ) );

  const QgsFeatureId fid = results.at( 0 ).userData().toMap()
                             .value( QStringLiteral( "fid" ) ).toLongLong();
  QCOMPARE( layer.selectedFeatureIds(), QgsFeatureIds{ fid } );

  // W2 sits at POINT(105 35); canvas must be re-targeted onto it.
  QVERIFY( canvas.extent().contains( QgsPointXY( 105, 35 ) ) );
  QVERIFY( canvas.extent() != QgsRectangle( 0, 0, 10, 10 ) );
}

// (d) horizon filter: case-insensitive contains over provider ids; trigger
// forwards the horizon id to the activate callback.
void TestLocator::horizonFetchMatchesAndTriggers()
{
  const QStringList horizons{ QStringLiteral( "T2" ), QStringLiteral( "T5a" ),
                              QStringLiteral( "T10" ) };
  QStringList activated;

  HorizonLocatorFilter filter( [horizons]() { return horizons; },
                            [&activated]( const QString &id ) { activated << id; } );

  const QList<QgsLocatorResult> results = runFetch( &filter, QStringLiteral( "t5" ) );
  QCOMPARE( results.size(), 1 );
  QCOMPARE( results.at( 0 ).displayString, QStringLiteral( "T5a" ) );
  QCOMPARE( results.at( 0 ).userData().toString(), QStringLiteral( "T5a" ) );

  filter.triggerResult( results.at( 0 ) );
  QCOMPARE( activated, QStringList{ QStringLiteral( "T5a" ) } );

  QCOMPARE( runFetch( &filter, QStringLiteral( "ZZZ" ) ).size(), 0 );
  // "T1" contains-matches T10 only
  QCOMPARE( runFetch( &filter, QStringLiteral( "T1" ) ).size(), 1 );
}

// (e) issue filter: matches on code or message; trigger reconstructs the
// IssueRef from userData and hands it to the locate callback.
void TestLocator::issueFetchMatchesAndTriggers()
{
  const QList<IssueLocatorFilter::IssueRef> issues{
    { QStringLiteral( "E001" ), QStringLiteral( "duplicate well id" ),
      QStringLiteral( "wells" ), QStringLiteral( "POINT(102 32)" ) },
    { QStringLiteral( "W104" ), QStringLiteral( "extent beyond work area" ),
      QStringLiteral( "wells" ), QStringLiteral( "POINT(1 2)" ) },
  };

  bool locatedCalled = false;
  IssueLocatorFilter::IssueRef located;
  IssueLocatorFilter filter( [issues]() { return issues; },
                          [&]( const IssueLocatorFilter::IssueRef &r ) {
                            locatedCalled = true;
                            located = r;
                          } );

  // match on code substring, case-insensitive
  QList<QgsLocatorResult> results = runFetch( &filter, QStringLiteral( "e00" ) );
  QCOMPARE( results.size(), 1 );
  QVERIFY( results.at( 0 ).displayString.contains( QStringLiteral( "E001" ) ) );

  const QVariantMap data = results.at( 0 ).userData().toMap();
  QCOMPARE( data.value( QStringLiteral( "code" ) ).toString(), QStringLiteral( "E001" ) );
  QCOMPARE( data.value( QStringLiteral( "layerId" ) ).toString(), QStringLiteral( "wells" ) );
  QCOMPARE( data.value( QStringLiteral( "wkt" ) ).toString(), QStringLiteral( "POINT(102 32)" ) );

  filter.triggerResult( results.at( 0 ) );
  QVERIFY( locatedCalled );
  QCOMPARE( located.code, QStringLiteral( "E001" ) );
  QCOMPARE( located.layerId, QStringLiteral( "wells" ) );
  QCOMPARE( located.wkt, QStringLiteral( "POINT(102 32)" ) );

  // match on message substring too
  results = runFetch( &filter, QStringLiteral( "work area" ) );
  QCOMPARE( results.size(), 1 );
  QVERIFY( results.at( 0 ).displayString.contains( QStringLiteral( "W104" ) ) );

  QCOMPARE( runFetch( &filter, QStringLiteral( "nope" ) ).size(), 0 );
}

int main( int argc, char *argv[] )
{
  qputenv( "QT_QPA_PLATFORM", "offscreen" ); // before QApplication is built
  QgsApplication app( argc, argv, true );
  app.setPrefixPath( QStringLiteral( "/usr" ), true ); // distro install
  app.initQgis();
  TestLocator tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_locator.moc"
