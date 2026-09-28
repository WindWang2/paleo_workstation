#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <gdal.h>
#include <cpl_conv.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/types.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"

// Workflow-orchestrator acceptance: real services end to end — a prediction
// run lands a declared "predict.<h>.<ts>" layer, constraints accumulate into a
// "constraints.<h>" declaration, ConstraintIDW declares "factor.<h>.idw",
// FaciesFusion declares "composite.<h>", and validation reports missing
// sources / busy layers / duplicate horizon names.
class TestWorkflows : public QObject
{
  Q_OBJECT

private:
  // Full real-service stack on one temp dir. Declaration order matters:
  // `dir` must precede `manifest` (its path feeds the ctor), and `projectSvc`
  // must precede `layers`/`proc` for the pointer bindings.
  // T26（wave3/derived-publish）：catalog 进 fixture——四个写出产物的路径
  // 都要求绑定，产物落 artifacts/derived 并登记 DERIVED 版本。
  struct Fixture
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService projectSvc;
    PaleoProjectStore store;
    LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
    QgisLayerService layers{ &projectSvc, &manifest };
    QgisProcessingService proc{ &store };
  };

  static bool initFixture( Fixture &f )
  {
    if ( !f.dir.isValid() )
      return false;
    if ( !f.catalog.open( f.dir.path() ) )
      return false;
    if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
      return false;
    if ( !f.manifest.open() )
      return false;
    return true;
  }

  // T26 断言助手：声明的图层源在 artifacts/derived 下，且 catalog 里有该资产
  // 类型指向此文件的 DERIVED 版本（sha 非空）。
  static bool derivedVersionRegistered( DataCatalog &catalog, const QString &assetType,
                                        const QString &absolutePath )
  {
    for ( const CatalogAsset &a : catalog.assets() )
    {
      if ( a.type != assetType )
        continue;
      for ( const CatalogVersion &v : catalog.versionsForAsset( a.id ) )
        if ( absolutePath.contains( QStringLiteral( "artifacts/derived/" ) ) &&
             absolutePath.endsWith( QLatin1Char( '/' ) + v.fileName ) &&
             absolutePath.contains( v.id ) && !v.sha256.isEmpty() )
          return true;
    }
    return false;
  }

  // Write a w x h Float32 GTiff; returns "" on failure (same pattern as
  // tst_algorithms.cpp).
  static QString makeRaster( const QString &path, int w, int h, const QVector<float> &px )
  {
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
    if ( !ds )
      return QString();
    const double gt[6] = { 0.0, 1.0, 0.0, static_cast<double>( h ), 0.0, -1.0 };
    GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h, const_cast<float *>( px.constData() ),
                                     w, h, GDT_Float32, 0, 0 );
    GDALClose( ds );
    return err == CE_None ? path : QString();
  }

  // Three points with a numeric z field, matching the IDW fixture in
  // tst_algorithms.cpp — written as GeoJSON so the "ogr" provider (used by
  // QgisLayerService::instantiate for type "vector") can load it.
  static bool writePointsGeoJson( const QString &path )
  {
    QFile f( path );
    if ( !f.open( QIODevice::WriteOnly ) )
      return false;
    f.write( "{\"type\":\"FeatureCollection\",\"features\":["
             "{\"type\":\"Feature\",\"properties\":{\"z\":0.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,0.0]}},"
             "{\"type\":\"Feature\",\"properties\":{\"z\":8.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[4.0,0.0]}},"
             "{\"type\":\"Feature\",\"properties\":{\"z\":4.0},\"geometry\":{\"type\":\"Point\",\"coordinates\":[0.0,4.0]}}"
             "]}" );
    f.close();
    return QFile::exists( path ) && QFileInfo( path ).size() > 0;
  }

  static LayerDeclaration decl( const QString &layerId, const QString &horizon,
                                const QString &type, const QString &source,
                                const QString &group = QStringLiteral( "00_Test" ) )
  {
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = type;
    d.source = source;
    d.group = group;
    return d;
  }

  static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
  {
    const QVector<LayerDeclaration> decls = layers.declared();
    for ( const LayerDeclaration &d : decls )
      if ( d.layerId == layerId )
        return new LayerDeclaration( d );
    return nullptr;
  }

private slots:

  void initTestCase()
  {
    QVERIFY( QgsApplication::instance() != nullptr );
    // Fixture::proc registers PaleoProvider with the real algorithms.
    Fixture f;
    QVERIFY( initFixture( f ) );
    QVERIFY( f.proc.paleoAlgorithmIds().contains( QStringLiteral( "paleo:paleo_geological_smoothing" ) ) );
    QVERIFY( f.proc.paleoAlgorithmIds().contains( QStringLiteral( "paleo:paleo_constraint_idw" ) ) );
    QVERIFY( f.proc.paleoAlgorithmIds().contains( QStringLiteral( "paleo:paleo_facies_fusion" ) ) );
  }

  void cleanup()
  {
    // Fusion/IDW instantiate into the fixture project (its own QgsProject,
    // dies with the fixture), but the singleton may still hold leftovers if a
    // fallback path was exercised — keep it clean for the next slot.
    QgsProject::instance()->removeAllMapLayers();
  }

  // ① prediction: geological smoothing on a tiny raster → declared layer + signal.
  void predictionRunDeclaresLayer()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QVector<float> px = { 1, 1, 1,
                                1, 2, 1,
                                1, 1, 1 };
    const QString inPath = makeRaster( f.dir.filePath( QStringLiteral( "coded_in.tif" ) ), 3, 3, px );
    QVERIFY( !inPath.isEmpty() );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "input.T1" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), inPath ), &err ), qPrintable( err ) );
    QgsMapLayer *input = f.layers.instantiate( QStringLiteral( "input.T1" ), &err );
    QVERIFY2( input != nullptr, qPrintable( err ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( input ) );
    params.insert( QStringLiteral( "PASSES" ), 1 );

    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ),
                                QStringLiteral( "paleo:paleo_geological_smoothing" ),
                                params, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 1 );
    QCOMPARE( doneSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );

    const QString resultId = doneSpy.at( 0 ).at( 1 ).toString();
    QVERIFY2( resultId.startsWith( QStringLiteral( "predict.T1." ) ), qPrintable( resultId ) );

    const LayerDeclaration *d = findDecl( f.layers, resultId );
    QVERIFY( d != nullptr );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QCOMPARE( d->group, QStringLiteral( "01_Prediction" ) );
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    // T26：产物在 artifacts/derived 下并登记 DERIVED 版本（不再进进程临时池）。
    QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
              qPrintable( d->source ) );
    QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "prediction_raster" ), d->source ),
              qPrintable( d->source ) );

    // Declared source loads back as a valid raster layer.
    QVERIFY( f.layers.instantiate( resultId ) != nullptr );

    delete d;
  }

  // Failure path: unknown algorithm id → predictionFailed + false.
  void predictionFailureSignals()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QString err;
    QVERIFY( !wf.runPrediction( QStringLiteral( "T2" ),
                                QStringLiteral( "paleo:does_not_exist" ),
                                QVariantMap(), &err ) );
    QVERIFY( !err.isEmpty() );
    QCOMPARE( doneSpy.count(), 0 );
    QCOMPARE( failSpy.count(), 1 );
    QCOMPARE( failSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T2" ) );
    QCOMPARE( failSpy.at( 0 ).at( 1 ).toString(), err );
  }

  // m2/mapping-pages(A)：重跑幂等——同一 horizon+algorithmId 再次运行复用同一
  // layerId，manifest 不新增重复行（upsert），source 指向最新一次产物；历史
  // 分组 01_Prediction 保持不动。且无置信度伴生层声明（当前算法栈无置信度
  // 输出——见 PredictionWorkflow::confidenceCompanionAvailable 调查注释）。
  void predictionRerunIsIdempotentAndDeclaresNoConfidence()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QVector<float> px = { 1, 1, 1,
                                1, 2, 1,
                                1, 1, 1 };
    const QString inPath = makeRaster( f.dir.filePath( QStringLiteral( "coded_in.tif" ) ), 3, 3, px );
    QVERIFY( !inPath.isEmpty() );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "input.T1" ), QStringLiteral( "T1" ),
                                      QStringLiteral( "raster" ), inPath ), &err ), qPrintable( err ) );
    QgsMapLayer *input = f.layers.instantiate( QStringLiteral( "input.T1" ), &err );
    QVERIFY2( input != nullptr, qPrintable( err ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );
    QSignalSpy failSpy( &wf, &PredictionWorkflow::predictionFailed );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( input ) );
    params.insert( QStringLiteral( "PASSES" ), 1 );

    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ),
                                QStringLiteral( "paleo:paleo_geological_smoothing" ),
                                params, &err ), qPrintable( err ) );
    QString firstSource;
    {
      const LayerDeclaration *d = nullptr;
      const QString id = doneSpy.at( 0 ).at( 1 ).toString();
      d = findDecl( f.layers, id );
      QVERIFY2( d != nullptr, qPrintable( id ) );
      firstSource = d->source;
      delete d;
    }
    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ),
                                QStringLiteral( "paleo:paleo_geological_smoothing" ),
                                params, &err ), qPrintable( err ) );
    QCOMPARE( failSpy.count(), 0 );
    QCOMPARE( doneSpy.count(), 2 );

    // 同一 horizon+algorithmId → 同一 layerId（稳定段 = 净化后的 algorithmId）。
    const QString id1 = doneSpy.at( 0 ).at( 1 ).toString();
    const QString id2 = doneSpy.at( 1 ).at( 1 ).toString();
    QCOMPARE( id1, id2 );
    QVERIFY2( id1.startsWith( QStringLiteral( "predict.T1." ) ), qPrintable( id1 ) );
    QVERIFY2( id1.contains( QStringLiteral( "paleo_geological_smoothing" ) ), qPrintable( id1 ) );

    // manifest 里该 layerId 只有一行（upsert，不重复），source 为最新产物。
    int rows = 0;
    const QVector<LayerDeclaration> decls = f.layers.declared();
    for ( const LayerDeclaration &d : decls )
      if ( d.layerId == id1 )
        ++rows;
    QCOMPARE( rows, 1 );

    const LayerDeclaration *d = findDecl( f.layers, id1 );
    QVERIFY( d != nullptr );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QCOMPARE( d->group, QStringLiteral( "01_Prediction" ) ); // 历史分组保持
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    QVERIFY( d->source != firstSource ); // 重跑产出新版本目录，声明行已更新
    delete d;

    // 置信度伴生层降级分支：无 confidence.* 声明、无 02_Prediction 分组行。
    int confidenceRows = 0, group02Rows = 0;
    for ( const LayerDeclaration &dd : decls )
    {
      if ( dd.layerId.startsWith( QStringLiteral( "confidence." ) ) )
        ++confidenceRows;
      if ( dd.group == QStringLiteral( "02_Prediction" ) )
        ++group02Rows;
    }
    QCOMPARE( confidenceRows, 0 );
    QCOMPARE( group02Rows, 0 );
  }

  // m2(A)：不同算法各自稳定 id——同层位两个算法互不挤占同一 layerId。
  void predictionDistinctAlgorithmsGetDistinctLayerIds()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QVector<float> px = { 1, 2, 3, 4 };
    const QString top = makeRaster( f.dir.filePath( QStringLiteral( "top.tif" ) ), 2, 2, px );
    const QString base = makeRaster( f.dir.filePath( QStringLiteral( "base.tif" ) ), 2, 2, px );
    QVERIFY( !top.isEmpty() && !base.isEmpty() );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "top.T1" ), QStringLiteral( "T1" ),
                                      QStringLiteral( "raster" ), top ), &err ), qPrintable( err ) );
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "base.T1" ), QStringLiteral( "T1" ),
                                      QStringLiteral( "raster" ), base ), &err ), qPrintable( err ) );
    QgsMapLayer *topL = f.layers.instantiate( QStringLiteral( "top.T1" ), &err );
    QgsMapLayer *baseL = f.layers.instantiate( QStringLiteral( "base.T1" ), &err );
    QVERIFY2( topL && baseL, qPrintable( err ) );

    PredictionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );
    QSignalSpy doneSpy( &wf, &PredictionWorkflow::predictionDone );

    QVariantMap iso;
    iso.insert( QStringLiteral( "INPUT_TOP" ), QVariant::fromValue( topL ) );
    iso.insert( QStringLiteral( "INPUT_BASE" ), QVariant::fromValue( baseL ) );
    iso.insert( QStringLiteral( "NEGATIVE_TO_NODATA" ), false );
    QVERIFY2( wf.runPrediction( QStringLiteral( "T1" ),
                                QStringLiteral( "paleo:paleo_isopach" ), iso, &err ),
              qPrintable( err ) );
    QCOMPARE( doneSpy.count(), 1 );
    const QString isoId = doneSpy.at( 0 ).at( 1 ).toString();
    QVERIFY2( isoId.startsWith( QStringLiteral( "predict.T1." ) ), qPrintable( isoId ) );
    QVERIFY2( isoId != QStringLiteral( "predict.T1.paleo.paleo_geological_smoothing" ),
              qPrintable( isoId ) );

    // 不同层位同样分离。
    QVERIFY2( wf.runPrediction( QStringLiteral( "T2" ),
                                QStringLiteral( "paleo:paleo_isopach" ), iso, &err ),
              qPrintable( err ) );
    const QString t2Id = doneSpy.at( 1 ).at( 1 ).toString();
    QVERIFY( t2Id.startsWith( QStringLiteral( "predict.T2." ) ) );
    QVERIFY( t2Id != isoId );
  }

  // ②a addConstraint: ids c-1, c-2; per-horizon "constraints.<h>" decl holds WKT.
  void addConstraintDeclaresAndSignals()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    ConstraintWorkflow wf( &f.proc, &f.layers );
    QSignalSpy spy( &wf, &ConstraintWorkflow::constraintAdded );

    QString err;
    QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "LINESTRING(0 0, 10 0)" ),
                                QStringLiteral( "line" ), 3, &err ), qPrintable( err ) );
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "c-1" ) );

    QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "LINESTRING(0 0, 0 10)" ),
                                QStringLiteral( "line" ), 4, &err ), qPrintable( err ) );
    QCOMPARE( spy.count(), 2 );
    QCOMPARE( spy.at( 1 ).at( 0 ).toString(), QStringLiteral( "c-2" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "constraints.T1" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QCOMPARE( d->type, QStringLiteral( "vector" ) );
    QCOMPARE( d->group, QStringLiteral( "02_Constraints" ) );
    QVERIFY( d->source.startsWith( QStringLiteral( "memory|" ) ) );
    QVERIFY( d->source.contains( QStringLiteral( "LINESTRING(0 0, 10 0)" ) ) );
    QVERIFY( d->source.contains( QStringLiteral( "LINESTRING(0 0, 0 10)" ) ) );
    delete d;

    // A second horizon gets its own declaration and does not leak WKT.
    QVERIFY( wf.addConstraint( QStringLiteral( "T2" ), QStringLiteral( "POINT(1 1)" ),
                               QStringLiteral( "point" ), 1 ) );
    QCOMPARE( spy.at( 2 ).at( 0 ).toString(), QStringLiteral( "c-3" ) );
    const LayerDeclaration *d2 = findDecl( f.layers, QStringLiteral( "constraints.T2" ) );
    QVERIFY( d2 != nullptr );
    QVERIFY( d2->source.contains( QStringLiteral( "POINT(1 1)" ) ) );
    QVERIFY( !d2->source.contains( QStringLiteral( "LINESTRING" ) ) );
    delete d2;

    // Empty WKT is rejected, does not consume an id, does not signal.
    QVERIFY( !wf.addConstraint( QStringLiteral( "T1" ), QStringLiteral( "  " ),
                                QStringLiteral( "line" ), 5, &err ) );
    QVERIFY( !err.isEmpty() );
    QCOMPARE( spy.count(), 3 );
  }

  // QGIS_NATIVE_ADOPTION：约束几何经原生 QgsGeometryValidator 拓扑门——
  // 自相交蝴蝶结多边形如实拒收（id 不消耗、constraintAdded 不发），
  // 合法多边形照常通过。
  void addConstraintRejectsInvalidGeometry()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    ConstraintWorkflow wf( &f.proc, &f.layers );
    QSignalSpy spy( &wf, &ConstraintWorkflow::constraintAdded );

    QString err;
    const QString bowtie = QStringLiteral( "POLYGON((0 0, 2 2, 2 0, 0 2, 0 0))" );
    QVERIFY2( !wf.addConstraint( QStringLiteral( "T1" ), bowtie,
                                 QStringLiteral( "polygon" ), 3, &err ),
              "self-intersecting ring must not enter the constraint store" );
    QVERIFY( !err.isEmpty() );
    QVERIFY( err.contains( QLatin1String( "invalid" ) ) );
    QCOMPARE( spy.count(), 0 );

    // 合法几何照常（同一 horizon，id 从 c-1 起——拒绝不消耗序号）。
    QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ),
                                QStringLiteral( "POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))" ),
                                QStringLiteral( "polygon" ), 3, &err ),
              qPrintable( err ) );
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "c-1" ) );

    // 空但合法的退化几何（空多边形）也如实放行——空 ≠ 拓扑违例；
    // 「空 WKT」这一档由上面的 trimmed-empty 检查承载。
    QVERIFY2( wf.addConstraint( QStringLiteral( "T1" ),
                                QStringLiteral( "POLYGON EMPTY" ),
                                QStringLiteral( "polygon" ), 3, &err ),
              qPrintable( err ) );
    QCOMPARE( spy.count(), 2 );
  }

  // ②b runConstraintIDW: resolves the declared points layer, declares factor.
  void constraintIdwDeclaresFactor()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QString ptsPath = f.dir.filePath( QStringLiteral( "pts.geojson" ) );
    QVERIFY( writePointsGeoJson( ptsPath ) );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "points.T1" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "vector" ), ptsPath ), &err ), qPrintable( err ) );

    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy spy( &wf, &ConstraintWorkflow::factorDone );

    QVERIFY2( wf.runConstraintIDW( QStringLiteral( "T1" ), QStringLiteral( "points.T1" ),
                                   QStringLiteral( "z" ), 1.0, &err ), qPrintable( err ) );
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "factor.T1.idw" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "factor.T1.idw" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
              qPrintable( d->source ) );
    QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "constraint_idw_raster" ), d->source ),
              qPrintable( d->source ) );
    QVERIFY( f.layers.instantiate( d->layerId ) != nullptr );
    delete d;

    // Unknown points layer id fails cleanly (no factor declared, no signal).
    QVERIFY( !wf.runConstraintIDW( QStringLiteral( "T1" ), QStringLiteral( "ghost.layer" ),
                                   QStringLiteral( "z" ), 1.0, &err ) );
    QVERIFY( !err.isEmpty() );
    QCOMPARE( spy.count(), 1 );
  }

  // ③ fuse two declared factor rasters → "composite.<h>" declared + signal.
  void fusionDeclaresComposite()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QVector<float> a = { 0, 5, 0,
                               0, 5, 0,
                               0, 0, 0 };
    const QVector<float> b = { 9, 9, 9,
                               9, 0, 9,
                               0, 7, 7 };
    const QString pA = makeRaster( f.dir.filePath( QStringLiteral( "fa.tif" ) ), 3, 3, a );
    const QString pB = makeRaster( f.dir.filePath( QStringLiteral( "fb.tif" ) ), 3, 3, b );
    QVERIFY( !pA.isEmpty() && !pB.isEmpty() );

    QString err;
    QVERIFY( f.layers.declare( decl( QStringLiteral( "factor.a.T1" ), QStringLiteral( "T1" ),
                                     QStringLiteral( "raster" ), pA,
                                     QStringLiteral( "04_SingleFactor" ) ), &err ) );
    QVERIFY( f.layers.declare( decl( QStringLiteral( "factor.b.T1" ), QStringLiteral( "T1" ),
                                     QStringLiteral( "raster" ), pB,
                                     QStringLiteral( "04_SingleFactor" ) ), &err ) );

    CompositionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy spy( &wf, &CompositionWorkflow::compositionDone );

    QVERIFY2( wf.fuseFactors( QStringLiteral( "T1" ),
                              { QStringLiteral( "factor.a.T1" ), QStringLiteral( "factor.b.T1" ) },
                              &err ), qPrintable( err ) );
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
    QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "composite.T1" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "composite.T1" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QCOMPARE( d->group, QStringLiteral( "03_Composite" ) );
    QVERIFY2( QFile::exists( d->source ), qPrintable( d->source ) );
    QVERIFY2( d->source.contains( QStringLiteral( "artifacts/derived/" ) ),
              qPrintable( d->source ) );
    QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "facies_fusion_raster" ), d->source ),
              qPrintable( d->source ) );

    QgsMapLayer *composite = f.layers.instantiate( d->layerId );
    QVERIFY( composite != nullptr );
    QVERIFY( composite->isValid() );
    delete d;

    // Undeclared input → false, no new signal.
    QVERIFY( !wf.fuseFactors( QStringLiteral( "T1" ),
                              { QStringLiteral( "factor.a.T1" ), QStringLiteral( "ghost.layer" ) },
                              &err ) );
    QCOMPARE( spy.count(), 1 );
  }

  // ④ validation: SRC_MISSING error on a bogus source, BUSY info on a marked
  // layer, DUP_HORIZON warning on colliding horizon spellings.
  void validationFlagsIssues()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );

    const QVector<float> px = { 1, 2, 3, 4 };
    const QString okPath = makeRaster( f.dir.filePath( QStringLiteral( "ok.tif" ) ), 2, 2, px );
    QVERIFY( !okPath.isEmpty() );
    const QString bogusPath = f.dir.filePath( QStringLiteral( "nope.geojson" ) );

    QString err;
    QVERIFY( f.layers.declare( decl( QStringLiteral( "ok.T1" ), QStringLiteral( "T1" ),
                                     QStringLiteral( "raster" ), okPath ), &err ) );
    QVERIFY( f.layers.declare( decl( QStringLiteral( "ghost.T1" ), QStringLiteral( "T1" ),
                                     QStringLiteral( "vector" ), bogusPath ), &err ) );
    // "t1" collides with "T1" after normalization → DUP_HORIZON.
    QVERIFY( f.layers.declare( decl( QStringLiteral( "other.t1" ), QStringLiteral( "t1" ),
                                     QStringLiteral( "raster" ), okPath ), &err ) );
    // memory-sourced constraint decl must NOT count as a missing file.
    QVERIFY( f.layers.declare( decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ),
                                     QStringLiteral( "vector" ), QStringLiteral( "memory|LINESTRING(0 0, 1 1)" ),
                                     QStringLiteral( "02_Constraints" ) ), &err ) );

    f.store.markLayerBusy( QStringLiteral( "ok.T1" ), QStringLiteral( "task-1" ),
                           QStringLiteral( "exporting" ) );

    ValidationWorkflow wf( &f.layers, &f.store );
    QSignalSpy spy( &wf, &ValidationWorkflow::validationDone );

    const QList<ValidationIssue> issues = wf.validate();
    QCOMPARE( spy.count(), 1 );
    QCOMPARE( spy.at( 0 ).at( 0 ).toInt(), issues.size() );

    int missing = 0, busy = 0, dup = 0;
    for ( const ValidationIssue &v : issues )
    {
      if ( v.code == QStringLiteral( "SRC_MISSING" ) )
      {
        ++missing;
        QCOMPARE( v.severity, ValidationIssue::Error );
        QCOMPARE( v.layerId, QStringLiteral( "ghost.T1" ) );
        QVERIFY( v.message.contains( bogusPath ) );
      }
      else if ( v.code == QStringLiteral( "BUSY" ) )
      {
        ++busy;
        QCOMPARE( v.severity, ValidationIssue::Info );
        QCOMPARE( v.layerId, QStringLiteral( "ok.T1" ) );
        QVERIFY( v.message.contains( QStringLiteral( "exporting" ) ) );
      }
      else if ( v.code == QStringLiteral( "DUP_HORIZON" ) )
      {
        ++dup;
        QCOMPARE( v.severity, ValidationIssue::Warning );
        QVERIFY( v.message.contains( QStringLiteral( "T1" ) ) );
        QVERIFY( v.message.contains( QStringLiteral( "t1" ) ) );
      }
    }
    QCOMPARE( missing, 1 );
    QCOMPARE( busy, 1 );
    QCOMPARE( dup, 1 );

    // Freeing the busy layer and removing the bad declaration clears them.
    f.store.markLayerFree( QStringLiteral( "ok.T1" ) );
    QVERIFY( f.manifest.remove( QStringLiteral( "ghost.T1" ) ) );
    const QList<ValidationIssue> again = wf.validate();
    for ( const ValidationIssue &v : again )
      QVERIFY( v.code != QStringLiteral( "SRC_MISSING" ) && v.code != QStringLiteral( "BUSY" ) );
    QCOMPARE( spy.count(), 2 );
  }

  // A classified raster becomes an editable facies-polygon layer on the manifest.
  void faciesPolygonsDeclaredFromRaster()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QVector<float> px = { 1, 1, 2, 2 };
    const QString path = makeRaster( f.dir.filePath( QStringLiteral( "coded.tif" ) ), 2, 2, px );
    QVERIFY( !path.isEmpty() );

    QString err;
    QVERIFY2( f.layers.declare( decl( QStringLiteral( "composite.T1" ), QStringLiteral( "T1" ),
                                      QStringLiteral( "raster" ), path,
                                      QStringLiteral( "03_Composite" ) ), &err ),
              qPrintable( err ) );

    CompositionWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() ); // T26
    QSignalSpy ready( &wf, &CompositionWorkflow::faciesPolygonsReady );
    QSignalSpy failed( &wf, &CompositionWorkflow::faciesPolygonsFailed );
    QVERIFY2( wf.deriveFaciesPolygons( QStringLiteral( "T1" ), QStringLiteral( "composite.T1" ),
                                       QVariantMap(), &err ),
              qPrintable( err ) );
    QCOMPARE( failed.count(), 0 );
    QCOMPARE( ready.count(), 1 );
    QCOMPARE( ready.at( 0 ).at( 1 ).toString(), QStringLiteral( "facies.T1" ) );

    const LayerDeclaration *d = findDecl( f.layers, QStringLiteral( "facies.T1" ) );
    QVERIFY( d != nullptr );
    QCOMPARE( d->type, QStringLiteral( "vector" ) );
    QCOMPARE( d->group, QStringLiteral( "05_PaleoMap" ) );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    const QString gpkg = d->source.section( QLatin1Char( '|' ), 0, 0 );
    QVERIFY2( QFile::exists( gpkg ), qPrintable( d->source ) );
    QVERIFY2( gpkg.contains( QStringLiteral( "artifacts/derived/" ) ), qPrintable( gpkg ) );
    QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "facies_polygons" ), gpkg ),
              qPrintable( gpkg ) );

    QgsVectorLayer vl( d->source, QStringLiteral( "faces" ), QStringLiteral( "ogr" ) );
    QVERIFY2( vl.isValid(), qPrintable( vl.error().message() ) );
    int n = 0;
    QgsFeature feat;
    QgsFeatureIterator it = vl.getFeatures();
    while ( it.nextFeature( feat ) )
      ++n;
    QCOMPARE( n, 2 );
    delete d;
  }

  // A manifest that cannot be read reports the read failure — the empty
  // declaration set is not authoritative, so "not declared" must not fire.
  void faciesPolygonsManifestReadFailure()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    // Manifest path under a regular file: ensureOpen cannot mkpath through it.
    QFile blocker( f.dir.filePath( QStringLiteral( "blocker" ) ) );
    QVERIFY( blocker.open( QIODevice::WriteOnly ) );
    blocker.close();
    LayerManifest broken( f.dir.filePath( QStringLiteral( "blocker/m.sqlite" ) ) );
    QgisLayerService brokenLayers( nullptr, &broken );
    CompositionWorkflow wf( &f.proc, &brokenLayers );
    QString err;
    QVERIFY( !wf.deriveFaciesPolygons( QStringLiteral( "T1" ), QStringLiteral( "composite.T1" ),
                                       QVariantMap(), &err ) );
    QVERIFY( !err.isEmpty() );
    QVERIFY2( !err.contains( QStringLiteral( "is not declared" ) ), qPrintable( err ) );
  }

  // An unreadable manifest must surface as a validation finding — a corrupt
  // store must not validate as "no issues".
  void validateReportsManifestReadFailure()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    QFile blocker( f.dir.filePath( QStringLiteral( "blocker" ) ) );
    QVERIFY( blocker.open( QIODevice::WriteOnly ) );
    blocker.close();
    LayerManifest broken( f.dir.filePath( QStringLiteral( "blocker/m.sqlite" ) ) );
    QgisLayerService brokenLayers( nullptr, &broken );
    ValidationWorkflow wf( &brokenLayers, &f.store );
    const QList<ValidationIssue> issues = wf.validate();
    bool saw = false;
    for ( const ValidationIssue &v : issues )
      if ( v.code == QLatin1String( "MANIFEST_READ_FAILED" ) )
        saw = v.severity == ValidationIssue::Error;
    QVERIFY( saw );
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestWorkflows tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_workflows.moc"
