#include <QtTest>
#include <qgsapplication.h>
#include <qgsprocessingregistry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsexception.h>
#include <gdal.h>
#include <cpl_conv.h>
#include <QTemporaryDir>
#include <memory>
#include "../src/algorithms/paleoalgorithms.h"

// Acceptance for the production Paleo algorithms: provider registers on
// QgsProcessingRegistry; each algorithm runs on synthetic inputs through the
// C++ path and produces a raster with plausible cell values.
class TestAlgorithms : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir mDir;

  // Write a w x h Float32 GTiff with a fixed grid; returns path.
  QString makeRaster( const QString &name, int w, int h, const QVector<float> &px,
                      bool setNodata = false, double nodata = -9999.0 )
  {
    const QString path = mDir.filePath( name );
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
    if ( !ds )
      return QString();
    const double gt[6] = { 0.0, 1.0, 0.0, static_cast<double>( h ), 0.0, -1.0 };
    GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    if ( setNodata )
      GDALSetRasterNoDataValue( band, nodata );
    const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h, const_cast<float *>( px.constData() ),
                                     w, h, GDT_Float32, 0, 0 );
    GDALClose( ds );
    return err == CE_None ? path : QString();
  }

  // Read band 1 of a raster as Float32.
  static bool readRaster( const QString &path, int &w, int &h, QVector<float> &px )
  {
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return false;
    w = GDALGetRasterXSize( ds );
    h = GDALGetRasterYSize( ds );
    px.resize( static_cast<qsizetype>( w ) * h );
    const CPLErr err = GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, w, h,
                                     px.data(), w, h, GDT_Float32, 0, 0 );
    GDALClose( ds );
    return err == CE_None;
  }

private slots:

  void registersAllAlgorithms()
  {
    QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_constraint_idw" ) ) != nullptr );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_facies_fusion" ) ) != nullptr );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_geological_smoothing" ) ) != nullptr );
  }

  // IDW over 3 points; assert weighted-average values at known cell centers.
  void constraintIdw()
  {
    // Points: A(0,0)=0, B(4,0)=8, C(0,4)=4
    auto *pts = new QgsVectorLayer(
        QStringLiteral( "Point?crs=EPSG:4326&field=z:double" ),
        QStringLiteral( "pts" ), QStringLiteral( "memory" ) );
    QVERIFY( pts->isValid() );
    QList<QgsFeature> feats;
    const QList<QPair<QgsPointXY, double>> data = {
        { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 4, 0 ), 8.0 }, { QgsPointXY( 0, 4 ), 4.0 } };
    for ( const auto &d : data )
    {
      QgsFeature f( pts->fields() );
      f.setGeometry( QgsGeometry::fromPointXY( d.first ) );
      f.setAttribute( QStringLiteral( "z" ), d.second );
      feats << f;
    }
    QVERIFY( pts->dataProvider()->addFeatures( feats ) );
    pts->updateExtents();

    // Input extent [0,4]x[0,4] + 10%/side -> [-0.4,4.4]; cellsize 1 -> 5x5 grid.
    // Cell centers x: 0.1,1.1,2.1,3.1,4.1 ; y (top->bottom): 3.9,2.9,1.9,0.9,-0.1
    const QString outPath = mDir.filePath( QStringLiteral( "idw.tif" ) );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( pts ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_constraint_idw" ) )
                                ->run( params, ctx, &fb );
    QVERIFY( !res.isEmpty() );
    QVERIFY( QFile::exists( outPath ) );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( readRaster( outPath, w, h, px ) );
    QCOMPARE( w, 5 );
    QCOMPARE( h, 5 );

    // Cell nearest B (r=4,c=4, center (4.1,-0.1)): B dominates -> ~7.99
    QVERIFY2( px[4 * 5 + 4] > 7.5f, qPrintable( QString::number( px[4 * 5 + 4] ) ) );
    // Cell nearest A (r=4,c=0, center (0.1,-0.1)): A dominates -> ~0.05
    QVERIFY2( px[4 * 5 + 0] < 0.5f, qPrintable( QString::number( px[4 * 5 + 0] ) ) );
    // Center cell (r=2,c=2, center (2.1,1.9)): hand-computed IDW ~4.147
    QVERIFY2( std::fabs( px[2 * 5 + 2] - 4.147f ) < 0.05f,
              qPrintable( QString::number( px[2 * 5 + 2] ) ) );

    // Same run with CONSTRAINTS: closed linestring ring around B only
    // (x in [3,5], y in [-1.3,1.3]) -> hull covers just cell (4,4).
    auto *lines = new QgsVectorLayer(
        QStringLiteral( "LineString?crs=EPSG:4326" ),
        QStringLiteral( "cons" ), QStringLiteral( "memory" ) );
    QVERIFY( lines->isValid() );
    QgsFeature lf;
    lf.setGeometry( QgsGeometry::fromPolylineXY(
        { QgsPointXY( 3, -1.3 ), QgsPointXY( 5, -1.3 ), QgsPointXY( 5, 1.3 ),
          QgsPointXY( 3, 1.3 ), QgsPointXY( 3, -1.3 ) } ) );
    QVERIFY( lines->dataProvider()->addFeatures( QList<QgsFeature>() << lf ) );
    lines->updateExtents();

    const QString outPath2 = mDir.filePath( QStringLiteral( "idw_clipped.tif" ) );
    QVariantMap params2 = params;
    params2.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( lines ) );
    params2.insert( QStringLiteral( "OUTPUT" ), outPath2 );
    const QVariantMap res2 = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_constraint_idw" ) )
                                 ->run( params2, ctx, &fb );
    QVERIFY( !res2.isEmpty() );
    QVector<float> px2;
    QVERIFY( readRaster( outPath2, w, h, px2 ) );
    // Inside hull: (r4,c4) still ~8; outside hull: (r0,c0) = nodata.
    QVERIFY2( px2[4 * 5 + 4] > 7.5f, qPrintable( QString::number( px2[4 * 5 + 4] ) ) );
    QVERIFY2( px2[0] == -9999.0f, qPrintable( QString::number( px2[0] ) ) );

    delete lines;
    delete pts;
  }

  // Two same-grid rasters fused with priority = input order.
  void faciesFusion()
  {
    // A nonzero only in column 1; B nonzero elsewhere.
    const QVector<float> a = { 0, 5, 0,
                               0, 5, 0,
                               0, 0, 0 };
    const QVector<float> b = { 9, 9, 9,
                               9, 0, 9,
                               0, 7, 7 };
    const QString pA = makeRaster( QStringLiteral( "a.tif" ), 3, 3, a );
    const QString pB = makeRaster( QStringLiteral( "b.tif" ), 3, 3, b );
    QVERIFY( !pA.isEmpty() && !pB.isEmpty() );

    auto *rA = new QgsRasterLayer( pA, QStringLiteral( "a" ), QStringLiteral( "gdal" ) );
    auto *rB = new QgsRasterLayer( pB, QStringLiteral( "b" ), QStringLiteral( "gdal" ) );
    QVERIFY( rA->isValid() && rB->isValid() );

    const QString outPath = mDir.filePath( QStringLiteral( "fused.tif" ) );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert( QStringLiteral( "INPUTS" ),
                   QVariantList{ QVariant::fromValue( static_cast<QgsMapLayer *>( rA ) ),
                                 QVariant::fromValue( static_cast<QgsMapLayer *>( rB ) ) } );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_facies_fusion" ) )
                                ->run( params, ctx, &fb );
    QVERIFY( !res.isEmpty() );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( readRaster( outPath, w, h, px ) );
    QCOMPARE( w, 3 );
    QCOMPARE( h, 3 );
    // Expected: A wins where nonzero; B fills the rest; all-zero -> nodata.
    const QVector<float> want = { 9, 5, 9,
                                  9, 5, 9,
                                  -9999, 7, 7 };
    for ( int i = 0; i < 9; ++i )
      QVERIFY2( px[i] == want[i],
                qPrintable( QStringLiteral( "cell %1: got %2 want %3" )
                                .arg( i ).arg( px[i] ).arg( want[i] ) ) );

    // Reverse priority: B's 9 must beat A's 5 at column 1.
    const QString outPathR = mDir.filePath( QStringLiteral( "fused_rev.tif" ) );
    QVariantMap paramsR = params;
    paramsR.insert( QStringLiteral( "INPUTS" ),
                    QVariantList{ QVariant::fromValue( static_cast<QgsMapLayer *>( rB ) ),
                                  QVariant::fromValue( static_cast<QgsMapLayer *>( rA ) ) } );
    paramsR.insert( QStringLiteral( "OUTPUT" ), outPathR );
    const QVariantMap resR = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_facies_fusion" ) )
                                 ->run( paramsR, ctx, &fb );
    QVERIFY( !resR.isEmpty() );
    QVector<float> pxR;
    QVERIFY( readRaster( outPathR, w, h, pxR ) );
    QVERIFY2( pxR[1] == 9.0f, qPrintable( QString::number( pxR[1] ) ) );

    delete rA;
    delete rB;
  }

  // Isolated coded cell is removed by 1 pass; nodata preserved.
  void geologicalSmoothing()
  {
    const QVector<float> in = { -9999, 1, 1, 1, 1,
                                1, 1, 1, 1, 1,
                                1, 1, 2, 1, 1,
                                1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1 };
    const QString pIn = makeRaster( QStringLiteral( "smooth_in.tif" ), 5, 5, in, true, -9999.0 );
    auto *rl = new QgsRasterLayer( pIn, QStringLiteral( "in" ), QStringLiteral( "gdal" ) );
    QVERIFY( rl->isValid() );

    const QString outPath = mDir.filePath( QStringLiteral( "smooth_out.tif" ) );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( rl ) );
    params.insert( QStringLiteral( "PASSES" ), 1 );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_geological_smoothing" ) )
                                ->run( params, ctx, &fb );
    QVERIFY( !res.isEmpty() );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( readRaster( outPath, w, h, px ) );
    QCOMPARE( w, 5 );
    QCOMPARE( h, 5 );
    QVERIFY2( px[2 * 5 + 2] == 1.0f, "isolated 2 must be smoothed away" );
    QVERIFY2( px[0] == -9999.0f, "nodata cell must stay nodata" );
    // Coded exactness: every cell is exactly a code or nodata (no interpolation).
    for ( int i = 0; i < 25; ++i )
      QVERIFY2( px[i] == 1.0f || px[i] == -9999.0f,
                qPrintable( QStringLiteral( "cell %1 = %2" ).arg( i ).arg( px[i] ) ) );

    delete rl;
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true );
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestAlgorithms tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_algorithms.moc"
