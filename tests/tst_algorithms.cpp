#include <QtTest>
#include <qgsapplication.h>
#include <qgsprocessingregistry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsfeedback.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsexception.h>
#include <qgsvectorlayer.h>
#include <gdal.h>
#include <cpl_conv.h>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
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
  QVariantMap mLast;

  void ensurePaleo()
  {
    if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "paleo" ) ) )
      QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
  }

  // Runs facies polygonize. Sets mLast. Returns the output path, empty on failure
  // (failure text is written to *log when provided).
  QString runFacies( const QString &name, int w, int h, const QVector<float> &px, QVariantMap params,
                     QgsVectorLayer *constraints = nullptr, QString *log = nullptr )
  {
    ensurePaleo();
    const QString rasPath = makeRaster( name + QStringLiteral( ".tif" ), w, h, px, true, -9999.0 );
    auto *ras = new QgsRasterLayer( rasPath, QStringLiteral( "r" ), QStringLiteral( "gdal" ) );
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( static_cast<QgsMapLayer *>( ras ) ) );
    params.insert( QStringLiteral( "OUTPUT" ), mDir.filePath( name + QStringLiteral( ".gpkg" ) ) );
    if ( constraints )
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( constraints ) ) );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_facies_polygonize" ) );
    mLast = alg ? alg->run( params, ctx, &fb ) : QVariantMap();
    delete ras;
    if ( log )
      *log = fb.textLog();
    return mLast.value( QStringLiteral( "OUTPUT" ) ).toString();
  }

  struct Face
  {
    int code = 0;
    double area = 0;
    QgsGeometry geom;
  };

  static QVector<Face> readFaces( const QString &path )
  {
    const QString uri = path.contains( QStringLiteral( "layername=" ) )
                            ? path
                            : path + QStringLiteral( "|layername=facies_polygons" );
    QgsVectorLayer vl( uri, QStringLiteral( "faces" ), QStringLiteral( "ogr" ) );
    QVector<Face> faces;
    if ( !vl.isValid() )
      return faces;
    QgsFeature f;
    QgsFeatureIterator it = vl.getFeatures();
    while ( it.nextFeature( f ) )
    {
      Face face;
      face.code = f.attribute( QStringLiteral( "facies_code" ) ).toInt();
      face.geom = f.geometry();
      face.area = face.geom.area();
      faces.append( face );
    }
    return faces;
  }

  static const Face *faceByCode( const QVector<Face> &faces, int code )
  {
    for ( const Face &f : faces )
      if ( f.code == code )
        return &f;
    return nullptr;
  }

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
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_isopach" ) ) != nullptr );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_min_curvature" ) ) != nullptr );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_facies_polygonize" ) ) != nullptr );
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

  // Stress-test BIZ-13: exact duplicate query points (d2 == 0), near-duplicate (d2 < 1e-12),
  // duplicate sample points, anisotropy near-singularity, ensuring no NaN or Inf occurs.
  void constraintIdwSingularityAndDuplicates()
  {
    ensurePaleo();
    auto *pts = new QgsVectorLayer(
        QStringLiteral( "Point?crs=EPSG:4326&field=z:double" ),
        QStringLiteral( "singularity_pts" ), QStringLiteral( "memory" ) );
    QVERIFY( pts->isValid() );

    // Points layout with cellSize = 1.0:
    // Raw extent: [0.5, 10.5] x [0.5, 10.5]
    // 10% padding -> extent [-0.5, 11.5] x [-0.5, 11.5] -> 12x12 grid.
    // Cell centers: x(c) = -0.5 + (c + 0.5) = c; y(r) = 11.5 - (r + 0.5) = 11 - r.
    // Cell (r=10, c=1) center is exactly (1.0, 1.0).
    // Cell (r=9, c=2) center is exactly (2.0, 2.0).
    QList<QgsFeature> feats;
    const QList<QPair<QgsPointXY, double>> data = {
        { QgsPointXY( 0.5, 0.5 ), 10.0 },                     // Sets extent lower bound
        { QgsPointXY( 10.5, 10.5 ), 50.0 },                   // Sets extent upper bound
        { QgsPointXY( 1.0, 1.0 ), 100.0 },                    // Exact hit on cell (10, 1) center: d2 == 0.0
        { QgsPointXY( 1.0, 1.0 ), 100.0 },                    // Duplicate sample point at exact same coords
        { QgsPointXY( 2.0 + 1e-7, 2.0 + 1e-7 ), 200.0 }      // Near-singularity hit on cell (9, 2): d2 = 2e-14 < 1e-12
    };
    for ( const auto &d : data )
    {
      QgsFeature f( pts->fields() );
      f.setGeometry( QgsGeometry::fromPointXY( d.first ) );
      f.setAttribute( QStringLiteral( "z" ), d.second );
      feats << f;
    }
    QVERIFY( pts->dataProvider()->addFeatures( feats ) );
    pts->updateExtents();

    // 1. Isotropic IDW run
    const QString outPath = mDir.filePath( QStringLiteral( "idw_singularity.tif" ) );
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
    QCOMPARE( w, 12 );
    QCOMPARE( h, 12 );

    // Verify cell (r=10, c=1) gets exact value 100.0f
    QCOMPARE( px[10 * 12 + 1], 100.0f );

    // Verify cell (r=9, c=2) gets near-exact value 200.0f (d2 < 1e-12 threshold hit)
    QCOMPARE( px[9 * 12 + 2], 200.0f );

    // Adversarial verification: NO cell in the grid should be NaN or Inf
    for ( int i = 0; i < px.size(); ++i )
    {
      const float val = px[i];
      QVERIFY2( !std::isnan( val ), qPrintable( QStringLiteral( "cell %1 is NaN" ).arg( i ) ) );
      QVERIFY2( !std::isinf( val ), qPrintable( QStringLiteral( "cell %1 is Inf" ).arg( i ) ) );
      QVERIFY2( val > 0.0f, qPrintable( QStringLiteral( "cell %1 value %2 invalid" ).arg( i ).arg( val ) ) );
    }

    // 2. Anisotropic IDW run with direction line
    auto *lines = new QgsVectorLayer(
        QStringLiteral( "LineString?crs=EPSG:4326&field=type:string" ),
        QStringLiteral( "cons_aniso" ), QStringLiteral( "memory" ) );
    QVERIFY( lines->isValid() );
    QgsFeature lf( lines->fields() );
    lf.setAttribute( QStringLiteral( "type" ), QStringLiteral( "direction_line" ) );
    lf.setGeometry( QgsGeometry::fromPolylineXY( { QgsPointXY( 0, 0 ), QgsPointXY( 10, 10 ) } ) );
    QVERIFY( lines->dataProvider()->addFeatures( QList<QgsFeature>() << lf ) );
    lines->updateExtents();

    const QString outPathAniso = mDir.filePath( QStringLiteral( "idw_singularity_aniso.tif" ) );
    QVariantMap paramsAniso = params;
    paramsAniso.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( lines ) );
    paramsAniso.insert( QStringLiteral( "ANISOTROPY_RATIO" ), 2.5 );
    paramsAniso.insert( QStringLiteral( "OUTPUT" ), outPathAniso );
    const QVariantMap resAniso = QgsApplication::processingRegistry()
                                     ->algorithmById( QStringLiteral( "paleo:paleo_constraint_idw" ) )
                                     ->run( paramsAniso, ctx, &fb );
    QVERIFY( !resAniso.isEmpty() );
    QVector<float> pxAniso;
    QVERIFY( readRaster( outPathAniso, w, h, pxAniso ) );

    // Cell with d2 == 0 remains exact 100.0f under anisotropy
    QCOMPARE( pxAniso[10 * 12 + 1], 100.0f );
    // Cell with d2 < 1e-12 remains exact 200.0f under anisotropy
    QCOMPARE( pxAniso[9 * 12 + 2], 200.0f );

    // Verify all cells under anisotropy have no NaN or Inf
    for ( int i = 0; i < pxAniso.size(); ++i )
    {
      const float val = pxAniso[i];
      QVERIFY2( !std::isnan( val ), qPrintable( QStringLiteral( "aniso cell %1 is NaN" ).arg( i ) ) );
      QVERIFY2( !std::isinf( val ), qPrintable( QStringLiteral( "aniso cell %1 is Inf" ).arg( i ) ) );
    }

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

  // top - base cell-wise; nodata propagates; NEGATIVE_TO_NODATA masks
  // inverted cells; grid mismatch fails.
  void isopach()
  {
    const QVector<float> top = { 100, 200, 300,
                                 150, 250, 50,
                                 10,  20,  30 };
    const QVector<float> base = { 50,  150, 250,
                                  100, 300, -9999, // (1,1) inverted; (1,2) nodata
                                  5,   10,  15 };
    const QString pTop = makeRaster( QStringLiteral( "top.tif" ), 3, 3, top );
    const QString pBase = makeRaster( QStringLiteral( "base.tif" ), 3, 3, base, true, -9999.0 );
    QVERIFY( !pTop.isEmpty() && !pBase.isEmpty() );

    auto *rTop = new QgsRasterLayer( pTop, QStringLiteral( "top" ), QStringLiteral( "gdal" ) );
    auto *rBase = new QgsRasterLayer( pBase, QStringLiteral( "base" ), QStringLiteral( "gdal" ) );
    QVERIFY( rTop->isValid() && rBase->isValid() );

    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;

    // Default: raw subtraction — inverted cell stays negative.
    const QString outPath = mDir.filePath( QStringLiteral( "iso.tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT_TOP" ), QVariant::fromValue( rTop ) );
    params.insert( QStringLiteral( "INPUT_BASE" ), QVariant::fromValue( rBase ) );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_isopach" ) )
                                ->run( params, ctx, &fb );
    QVERIFY( !res.isEmpty() );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( readRaster( outPath, w, h, px ) );
    QCOMPARE( w, 3 );
    QCOMPARE( h, 3 );
    const QVector<float> want = { 50, 50, 50,
                                  50, -50, -9999, // inverted kept; nodata propagates
                                  5,  10, 15 };
    for ( int i = 0; i < 9; ++i )
      QVERIFY2( px[i] == want[i],
                qPrintable( QStringLiteral( "cell %1: got %2 want %3" )
                                .arg( i ).arg( px[i] ).arg( want[i] ) ) );

    // NEGATIVE_TO_NODATA masks the inverted cell.
    const QString outPath2 = mDir.filePath( QStringLiteral( "iso_mask.tif" ) );
    QVariantMap params2 = params;
    params2.insert( QStringLiteral( "NEGATIVE_TO_NODATA" ), true );
    params2.insert( QStringLiteral( "OUTPUT" ), outPath2 );
    const QVariantMap res2 = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_isopach" ) )
                                 ->run( params2, ctx, &fb );
    QVERIFY( !res2.isEmpty() );
    QVector<float> px2;
    QVERIFY( readRaster( outPath2, w, h, px2 ) );
    QVERIFY2( px2[4] == -9999.0f,
              qPrintable( QStringLiteral( "inverted cell should be nodata, got %1" ).arg( px2[4] ) ) );
    QVERIFY2( px2[0] == 50.0f, "valid thickness unchanged" );

    // Grid mismatch → algorithm run fails (exception inside run surfaces as
    // empty results map or thrown QgsProcessingException handled by run()).
    const QVector<float> odd( 2 * 2, 1.0f );
    const QString pOdd = makeRaster( QStringLiteral( "odd.tif" ), 2, 2, odd );
    auto *rOdd = new QgsRasterLayer( pOdd, QStringLiteral( "odd" ), QStringLiteral( "gdal" ) );
    QVERIFY( rOdd->isValid() );
    QVariantMap params3 = params;
    params3.insert( QStringLiteral( "INPUT_BASE" ), QVariant::fromValue( rOdd ) );
    params3.insert( QStringLiteral( "OUTPUT" ), mDir.filePath( QStringLiteral( "iso_bad.tif" ) ) );
    const QVariantMap res3 = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_isopach" ) )
                                 ->run( params3, ctx, &fb );
    QVERIFY( res3.isEmpty() || !QFile::exists( params3.value( "OUTPUT" ).toString() ) );

    delete rOdd;
    delete rTop;
    delete rBase;
  }

  // paleo:paleo_min_curvature — plane scattered samples reproduce the plane
  // (RMS < 0.5% of range at zero tension); QC metadata lands in the GeoTIFF;
  // break_line CONSTRAINTS produce nodata barrier cells + count metadata.
  void minCurvaturePlane()
  {
    ensurePaleo();
    auto *pts = new QgsVectorLayer(
        QStringLiteral( "Point?crs=EPSG:3857&field=z:double" ),
        QStringLiteral( "mc_pts" ), QStringLiteral( "memory" ) );
    QVERIFY( pts->isValid() );
    QList<QgsFeature> feats;
    // plane z = 100 + 0.4x − 0.3y over [0,100]²；8×8 散点 @12.5（带半格偏移，
    // 不与输出格心对齐——散点语义）。z 范围 [70, 140] → range 70。
    for ( int i = 0; i < 8; ++i )
      for ( int j = 0; j < 8; ++j )
      {
        const double x = 6.25 + 12.5 * i, y = 6.25 + 12.5 * j;
        QgsFeature f( pts->fields() );
        f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( x, y ) ) );
        f.setAttribute( QStringLiteral( "z" ), 100.0 + 0.4 * x - 0.3 * y );
        feats << f;
      }
    QVERIFY( pts->dataProvider()->addFeatures( feats ) );
    pts->updateExtents();

    const QString outPath = mDir.filePath( QStringLiteral( "mc_plane.tif" ) );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( pts ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "TENSION" ), 0.0 );
    params.insert( QStringLiteral( "CELL_SIZE" ), 2.5 );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_min_curvature" ) )
                                ->run( params, ctx, &fb );
    QVERIFY2( !res.isEmpty(), qPrintable( fb.textLog() ) );
    QVERIFY( res.value( QStringLiteral( "CONVERGED" ) ).toBool() );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( readRaster( outPath, w, h, px ) );
    // 点层范围 [6.25,93.75]² + 10% 外推 → [-2.5,102.5]² @2.5 → 42×42。
    QCOMPARE( w, 42 );
    QCOMPARE( h, 42 );
    double sumSq = 0;
    int m = 0;
    for ( int r = 0; r < h; ++r )
      for ( int c = 0; c < w; ++c )
      {
        const double cx = -2.5 + ( c + 0.5 ) * 2.5;
        const double cy = 102.5 - ( r + 0.5 ) * 2.5;
        if ( cx < 6.25 || cx > 93.75 || cy < 6.25 || cy > 93.75 )
          continue; // 只在数据域内断言（边缘外是外推）
        const double v = px[r * w + c];
        if ( v == -9999.0f )
          continue;
        const double d = v - ( 100.0 + 0.4 * cx - 0.3 * cy );
        sumSq += d * d;
        ++m;
      }
    const double rms = std::sqrt( sumSq / m );
    QVERIFY2( rms < 0.35, qPrintable( QStringLiteral( "plane RMS %1" ).arg( rms ) ) );

    // QC 元数据。
    GDALDatasetH ds = GDALOpen( outPath.toUtf8().constData(), GA_ReadOnly );
    QVERIFY( ds );
    QCOMPARE( GDALGetMetadataItem( ds, "PALEO_ALGORITHM", nullptr ),
              QStringLiteral( "min_curvature" ) );
    QCOMPARE( GDALGetMetadataItem( ds, "PALEO_CONVERGED", nullptr ), QStringLiteral( "1" ) );
    QCOMPARE( GDALGetMetadataItem( ds, "PALEO_TENSION", nullptr ), QStringLiteral( "0" ) );
    QVERIFY( GDALGetMetadataItem( ds, "PALEO_SWEEPS", nullptr ) != nullptr );
    QVERIFY( GDALGetMetadataItem( ds, "PALEO_DIST_TO_DATA_MAX", nullptr ) != nullptr );
    GDALClose( ds );

    // ---- break_line 屏障：屏障格 nodata + 计数元数据 ----------------------
    auto *lines = new QgsVectorLayer(
        QStringLiteral( "LineString?crs=EPSG:3857&field=type:string" ),
        QStringLiteral( "mc_cons" ), QStringLiteral( "memory" ) );
    QVERIFY( lines->isValid() );
    QgsFeature lf( lines->fields() );
    lf.setGeometry( QgsGeometry::fromPolylineXY(
        { QgsPointXY( 50, -10 ), QgsPointXY( 50, 110 ) } ) );
    lf.setAttribute( QStringLiteral( "type" ), QStringLiteral( "break_line" ) );
    QVERIFY( lines->dataProvider()->addFeatures( QList<QgsFeature>() << lf ) );
    lines->updateExtents();

    const QString outPath2 = mDir.filePath( QStringLiteral( "mc_barrier.tif" ) );
    QVariantMap params2 = params;
    params2.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( lines ) );
    params2.insert( QStringLiteral( "OUTPUT" ), outPath2 );
    const QVariantMap res2 = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_min_curvature" ) )
                                 ->run( params2, ctx, &fb );
    QVERIFY2( !res2.isEmpty(), qPrintable( fb.textLog() ) );
    QVector<float> px2;
    QVERIFY( readRaster( outPath2, w, h, px2 ) );
    int nodataCells = 0;
    for ( float v : px2 )
      if ( v == -9999.0f )
        ++nodataCells;
    QVERIFY2( nodataCells >= h, // 竖直墙至少覆盖一列
              qPrintable( QStringLiteral( "nodata cells %1" ).arg( nodataCells ) ) );
    ds = GDALOpen( outPath2.toUtf8().constData(), GA_ReadOnly );
    QVERIFY( ds );
    QCOMPARE( GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr ), QStringLiteral( "1" ) );
    GDALClose( ds );

    delete lines;
    delete pts;
  }

  // paleo:paleo_min_curvature 守卫：超规模 CELL_SIZE 拒绝（Issue #33 算法侧
  // 同口径——拒绝而非 OOM 分配）；进度单调。
  void minCurvatureGuardAndProgress()
  {
    ensurePaleo();
    auto *pts = new QgsVectorLayer(
        QStringLiteral( "Point?crs=EPSG:3857&field=z:double" ),
        QStringLiteral( "mc_guard_pts" ), QStringLiteral( "memory" ) );
    QVERIFY( pts->isValid() );
    QList<QgsFeature> feats;
    for ( int i = 0; i < 4; ++i )
      for ( int j = 0; j < 4; ++j )
      {
        QgsFeature f( pts->fields() );
        f.setGeometry( QgsGeometry::fromPointXY(
            QgsPointXY( 1000.0 * i / 3, 1000.0 * j / 3 ) ) );
        f.setAttribute( QStringLiteral( "z" ), 10.0 * i + j );
        feats << f;
      }
    QVERIFY( pts->dataProvider()->addFeatures( feats ) );
    pts->updateExtents();

    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( pts ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "OUTPUT" ),
                   mDir.filePath( QStringLiteral( "mc_oversize.tif" ) ) );
    // 1000m 跨度 @1e-5 → ~1e8 × 1e8 格，远超 1 亿预算 → run 失败、不落盘。
    params.insert( QStringLiteral( "CELL_SIZE" ), 1e-5 );
    const QVariantMap res = QgsApplication::processingRegistry()
                                ->algorithmById( QStringLiteral( "paleo:paleo_min_curvature" ) )
                                ->run( params, ctx, &fb );
    QVERIFY( res.isEmpty() );
    QVERIFY( !QFile::exists( params.value( "OUTPUT" ).toString() ) );

    // 进度单调（Oracle 5）：正常跑记录进度序列（progressChanged 信号），
    // 断言非降。
    QgsProcessingFeedback rf;
    QVector<double> progressLog;
    QObject::connect( &rf, &QgsFeedback::progressChanged, &rf,
                      [&progressLog]( double p ) { progressLog.append( p ); } );
    QVariantMap params2 = params;
    params2.insert( QStringLiteral( "CELL_SIZE" ), 40.0 );
    params2.insert( QStringLiteral( "OUTPUT" ),
                    mDir.filePath( QStringLiteral( "mc_progress.tif" ) ) );
    const QVariantMap res2 = QgsApplication::processingRegistry()
                                 ->algorithmById( QStringLiteral( "paleo:paleo_min_curvature" ) )
                                 ->run( params2, ctx, &rf );
    QVERIFY2( !res2.isEmpty(), qPrintable( rf.textLog() ) );
    QVERIFY2( progressLog.size() >= 2,
              qPrintable( QStringLiteral( "progress reports: %1" ).arg( progressLog.size() ) ) );
    for ( int i = 1; i < progressLog.size(); ++i )
      QVERIFY2( progressLog[i] >= progressLog[i - 1],
                qPrintable( QStringLiteral( "progress %1 < %2" )
                                .arg( progressLog[i] )
                                .arg( progressLog[i - 1] ) ) );

    delete pts;
  }

  // Three facies, shared edges, no overlap. Areas are the cell counts.
  void faciesPolygonsCoverage()
  {
    const QVector<float> px = { 1, 1, 2, 2,
                                1, 1, 2, 2,
                                3, 3, 3, 3,
                                3, 3, 3, 3 };
    QString log;
    const QString out = runFacies( QStringLiteral( "three" ), 4, 4, px, {}, nullptr, &log );
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    QCOMPARE( faces.size(), 3 );
    const Face *a = faceByCode( faces, 1 );
    const Face *b = faceByCode( faces, 2 );
    const Face *c = faceByCode( faces, 3 );
    QVERIFY( a && b && c );
    QVERIFY( std::fabs( a->area - 4.0 ) < 1e-4 );
    QVERIFY( std::fabs( b->area - 4.0 ) < 1e-4 );
    QVERIFY( std::fabs( c->area - 8.0 ) < 1e-4 );
    QVERIFY( a->geom.intersection( b->geom ).area() < 1e-6 );
    QVERIFY( a->geom.intersection( c->geom ).area() < 1e-6 );
    QVERIFY( b->geom.intersection( c->geom ).area() < 1e-6 );

    const QJsonObject prov = QJsonDocument::fromJson( mLast.value( QStringLiteral( "PROVENANCE" ) ).toString().toUtf8() ).object();
    QCOMPARE( prov.value( QStringLiteral( "algorithm" ) ).toString(),
              QStringLiteral( "paleo:paleo_facies_polygonize" ) );
    QCOMPARE( prov.value( QStringLiteral( "faces" ) ).toInt(), 3 );
  }

  // 1.4 rounds to 1, 1.6 rounds to 2.（单像元精度语义——显式关平滑/聚合）
  void faciesPolygonsRecode()
  {
    QString log;
    QVariantMap params;
    params.insert( QStringLiteral( "SMOOTH" ), 0 );
    params.insert( QStringLiteral( "MIN_CELLS" ), 0 );
    const QString out = runFacies( QStringLiteral( "recode" ), 2, 1, { 1.4f, 1.6f }, params, nullptr, &log );
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    QCOMPARE( faces.size(), 2 );
    QVERIFY( faceByCode( faces, 1 ) );
    QVERIFY( faceByCode( faces, 2 ) );
  }

  // A one-cell speck is absorbed into the surrounding facies.（关平滑，专注聚合）
  void faciesPolygonsAbsorbSliver()
  {
    const QVector<float> px = { 1, 1, 1, 1,
                                1, 2, 1, 1,
                                1, 1, 1, 1,
                                1, 1, 1, 1 };
    QVariantMap params;
    params.insert( QStringLiteral( "SMOOTH" ), 0 );
    params.insert( QStringLiteral( "MIN_AREA" ), 1.5 );
    QString log;
    const QString out = runFacies( QStringLiteral( "sliver" ), 4, 4, px, params, nullptr, &log );
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    QCOMPARE( faces.size(), 1 );
    QVERIFY( faceByCode( faces, 1 ) );
    QVERIFY( std::fabs( faces.at( 0 ).area - 16.0 ) < 1e-4 );
    const QJsonObject prov = QJsonDocument::fromJson( mLast.value( QStringLiteral( "PROVENANCE" ) ).toString().toUtf8() ).object();
    QVERIFY( prov.value( QStringLiteral( "slivers_merged" ) ).toInt() >= 1 );
  }

  // 默认开启的 3×3 多数滤波把椒盐噪点平滑成单一相面（用户要求：
  // 相栅格转相矢量一定要做平滑和聚合，不然太细了）。
  void faciesPolygonsSmoothSpeckleByDefault()
  {
    const QVector<float> px = { 1, 1, 1, 1, 1,
                                1, 2, 1, 2, 1,
                                1, 1, 1, 1, 1,
                                1, 2, 1, 1, 1,
                                1, 1, 1, 1, 1 };
    QString log;
    const QString out = runFacies( QStringLiteral( "speckle" ), 5, 5, px, {}, nullptr, &log );
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    QCOMPARE( faces.size(), 1 );
    QCOMPARE( faces.at( 0 ).code, 1 );
    QVERIFY( std::fabs( faces.at( 0 ).area - 25.0 ) < 1e-4 );
    const QJsonObject prov = QJsonDocument::fromJson( mLast.value( QStringLiteral( "PROVENANCE" ) ).toString().toUtf8() ).object();
    QCOMPARE( prov.value( QStringLiteral( "smooth_passes" ) ).toInt(), 1 );
    QVERIFY( prov.value( QStringLiteral( "smoothed_cells" ) ).toInt() >= 3 );
  }

  // 默认 MIN_CELLS：小于阈值（像元数）的图斑被吸收进最大邻相。
  void faciesPolygonsMinCellsAbsorbsByDefault()
  {
    const QVector<float> px = { 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1,
                                1, 1, 2, 2, 1, 1,
                                1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1 };
    QVariantMap params;
    params.insert( QStringLiteral( "SMOOTH" ), 0 ); // 关平滑——专注像元数聚合
    QString log;
    const QString out = runFacies( QStringLiteral( "mincells" ), 6, 5, px, params, nullptr, &log );
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    QCOMPARE( faces.size(), 1 );
    QCOMPARE( faces.at( 0 ).code, 1 );
    const QJsonObject prov = QJsonDocument::fromJson( mLast.value( QStringLiteral( "PROVENANCE" ) ).toString().toUtf8() ).object();
    QVERIFY( prov.value( QStringLiteral( "slivers_merged" ) ).toInt() >= 1 );
  }

  // 平滑次数可叠加：3×2 噪声条一遍后缩成 2 像元竖条，两遍清干净。
  void faciesPolygonsSmoothTwoPasses()
  {
    const QVector<float> px = { 1, 1, 1, 1, 1,
                                1, 2, 2, 2, 1,
                                1, 2, 2, 2, 1,
                                1, 1, 1, 1, 1 };
    QVariantMap one;
    one.insert( QStringLiteral( "SMOOTH" ), 1 );
    one.insert( QStringLiteral( "MIN_CELLS" ), 0 );
    QString log;
    const QString out1 = runFacies( QStringLiteral( "smooth1" ), 5, 4, px, one, nullptr, &log );
    QVERIFY2( !out1.isEmpty(), qPrintable( log ) );
    QCOMPARE( readFaces( out1 ).size(), 2 ); // 一遍后仍残留一个 2 像元

    QVariantMap two;
    two.insert( QStringLiteral( "SMOOTH" ), 2 );
    two.insert( QStringLiteral( "MIN_CELLS" ), 0 );
    const QString out2 = runFacies( QStringLiteral( "smooth2" ), 5, 4, px, two, nullptr, &log );
    QVERIFY2( !out2.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out2 );
    QCOMPARE( faces.size(), 1 );
    QCOMPARE( faces.at( 0 ).code, 1 );
    QVERIFY( std::fabs( faces.at( 0 ).area - 20.0 ) < 1e-4 );
  }

  // Coverage simplify removes staircase vertices without opening a gap.
  void faciesPolygonsSimplifyKeepsCoverage()
  {
    const QVector<float> px = { 1, 1, 1, 2,
                                1, 1, 2, 2,
                                1, 2, 2, 2,
                                2, 2, 2, 2 };
    QString log;
    const QString rawPath = runFacies( QStringLiteral( "stair" ), 4, 4, px, {}, nullptr, &log );
    QVERIFY2( !rawPath.isEmpty(), qPrintable( log ) );
    const QVector<Face> rawFaces = readFaces( rawPath );
    const Face *raw = faceByCode( rawFaces, 1 );
    QVERIFY( raw );
    const auto exteriorVerts = []( const QgsGeometry &g ) -> int {
      const QgsPolygonXY poly = g.asPolygon();
      if ( !poly.isEmpty() )
        return static_cast<int>( poly.at( 0 ).size() );
      int n = 0;
      const QVector<QgsGeometry> parts = g.asGeometryCollection();
      for ( const QgsGeometry &part : parts )
      {
        const QgsPolygonXY pp = part.asPolygon();
        if ( !pp.isEmpty() )
          n += static_cast<int>( pp.at( 0 ).size() );
      }
      return n;
    };
    const int rawVerts = exteriorVerts( raw->geom );

    QVariantMap params;
    params.insert( QStringLiteral( "SIMPLIFY" ), 1.5 );
    const QString simPath = runFacies( QStringLiteral( "stair_s" ), 4, 4, px, params, nullptr, &log );
    QVERIFY2( !simPath.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( simPath );
    const Face *a = faceByCode( faces, 1 );
    const Face *b = faceByCode( faces, 2 );
    QVERIFY( a && b );
    const int simVerts = exteriorVerts( a->geom );
    QVERIFY2( simVerts > 0 && simVerts < rawVerts,
              qPrintable( QStringLiteral( "verts %1 -> %2 wkb %3 wkt %4" )
                              .arg( rawVerts )
                              .arg( simVerts )
                              .arg( static_cast<int>( raw->geom.wkbType() ) )
                              .arg( raw->geom.asWkt().left( 160 ) ) ) );
    QVERIFY( a->geom.intersection( b->geom ).area() < 1e-4 );
    QVERIFY( a->area > 1.0 );
    QVERIFY( b->area > 1.0 );
  }

  // A vertical shared edge snaps onto a nearby parallel constraint.
  void faciesPolygonsConflateSharedEdge()
  {
    const QVector<float> px = { 1, 1, 2, 2,
                                1, 1, 2, 2,
                                1, 1, 2, 2,
                                1, 1, 2, 2 };
    auto *line = new QgsVectorLayer( QStringLiteral( "LineString?crs=EPSG:4326" ),
                                    QStringLiteral( "cons" ), QStringLiteral( "memory" ) );
    QVERIFY( line->isValid() );
    QgsFeature lf( line->fields() );
    lf.setGeometry( QgsGeometry::fromPolylineXY(
        { QgsPointXY( 2.3, -1 ), QgsPointXY( 2.3, 5 ) } ) );
    QgsFeatureList feats;
    feats << lf;
    QVERIFY( line->dataProvider()->addFeatures( feats ) );

    QVariantMap params;
    params.insert( QStringLiteral( "SNAP_TOLERANCE" ), 0.5 );
    params.insert( QStringLiteral( "ANGLE_TOLERANCE" ), 20.0 );
    QString log;
    const QString out = runFacies( QStringLiteral( "snap" ), 4, 4, px, params, line, &log );
    delete line;
    QVERIFY2( !out.isEmpty(), qPrintable( log ) );
    const QVector<Face> faces = readFaces( out );
    const Face *a = faceByCode( faces, 1 );
    const Face *b = faceByCode( faces, 2 );
    QVERIFY( a && b );
    QVERIFY2( std::fabs( a->geom.boundingBox().xMaximum() - 2.3 ) < 0.05,
              qPrintable( QString::number( a->geom.boundingBox().xMaximum() ) ) );
    QVERIFY2( std::fabs( b->geom.boundingBox().xMinimum() - 2.3 ) < 0.05,
              qPrintable( QString::number( b->geom.boundingBox().xMinimum() ) ) );
    QVERIFY( a->geom.intersection( b->geom ).area() < 1e-4 );
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestAlgorithms tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_algorithms.moc"
