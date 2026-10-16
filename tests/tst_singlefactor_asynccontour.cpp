// 层：测试壳
#include "helpers/workflowfixture.h"
#include "../src/qgis/layervocabulary.h"
#include <QtTest>
#include <QDir>

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <qgsapplication.h>
#include <qgsproject.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include "../src/algorithms/singlefactor/contourlevels.h" // kMaxContourLevels（U9 上限闸口径）
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"

// 分析等值线与解释性等值线的准备 / 计算 / 发布。不测性能。

using paleo::tests::initFixture;

class TestSingleFactorAsyncContour : public QObject
{
  Q_OBJECT

private:
  using Fixture = paleo::tests::WorkflowFixture;

  static OGRSpatialReferenceH makeSpatialRef( int epsg )
  {
    OGRSpatialReferenceH srs = OSRNewSpatialReference( nullptr );
    if ( !srs || OSRImportFromEPSG( srs, epsg ) != OGRERR_NONE )
    {
      if ( srs )
        OSRDestroySpatialReference( srs );
      return nullptr;
    }
    OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
    return srs;
  }

  // 北向上 Float32，nodata -9999。列号即值，等值线是竖线。
  static bool writeNorthUpFloat32( const QString &path, int cols, int rows, double originX, double originY,
                                   double cell )
  {
    GDALDriverH driver = GDALGetDriverByName( "GTiff" );
    if ( !driver )
      return false;
    GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
    if ( !dataset )
      return false;
    double geoTransform[6] = { originX, cell, 0.0, originY, 0.0, -cell };
    if ( GDALSetGeoTransform( dataset, geoTransform ) != CE_None )
    {
      GDALClose( dataset );
      return false;
    }
    OGRSpatialReferenceH srs = makeSpatialRef( 3857 );
    if ( !srs )
    {
      GDALClose( dataset );
      return false;
    }
    char *wkt = nullptr;
    OSRExportToWkt( srs, &wkt );
    OSRDestroySpatialReference( srs );
    if ( !wkt || GDALSetProjection( dataset, wkt ) != CE_None )
    {
      CPLFree( wkt );
      GDALClose( dataset );
      return false;
    }
    CPLFree( wkt );
    GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    QVector<float> row( cols );
    for ( int r = 0; r < rows; ++r )
    {
      for ( int c = 0; c < cols; ++c )
        row[c] = static_cast<float>( c );
      if ( GDALRasterIO( band, GF_Write, 0, r, cols, 1, row.data(), cols, 1, GDT_Float32, 0, 0 ) != CE_None )
      {
        GDALClose( dataset );
        return false;
      }
    }
    GDALClose( dataset );
    return QFile::exists( path );
  }

  static bool writeConstantFloat32( const QString &path, int cols, int rows, double originX, double originY,
                                    double cell, double value )
  {
    GDALDriverH driver = GDALGetDriverByName( "GTiff" );
    if ( !driver )
      return false;
    GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
    if ( !dataset )
      return false;
    double geoTransform[6] = { originX, cell, 0.0, originY, 0.0, -cell };
    if ( GDALSetGeoTransform( dataset, geoTransform ) != CE_None )
    {
      GDALClose( dataset );
      return false;
    }
    OGRSpatialReferenceH srs = makeSpatialRef( 3857 );
    if ( !srs )
    {
      GDALClose( dataset );
      return false;
    }
    char *wkt = nullptr;
    OSRExportToWkt( srs, &wkt );
    OSRDestroySpatialReference( srs );
    if ( !wkt || GDALSetProjection( dataset, wkt ) != CE_None )
    {
      CPLFree( wkt );
      GDALClose( dataset );
      return false;
    }
    CPLFree( wkt );
    GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    const QVector<float> row( cols, static_cast<float>( value ) );
    for ( int r = 0; r < rows; ++r )
    {
      if ( GDALRasterIO( band, GF_Write, 0, r, cols, 1, const_cast<float *>( row.constData() ), cols, 1,
                         GDT_Float32, 0, 0 ) != CE_None )
      {
        GDALClose( dataset );
        return false;
      }
    }
    GDALClose( dataset );
    return QFile::exists( path );
  }

  static bool writeStopLine( const QString &path, double x0, double y0, double x1, double y1 )
  {
    GDALDriverH driver = GDALGetDriverByName( "GPKG" );
    if ( !driver )
      return false;
    GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
    if ( !dataset )
      return false;
    OGRSpatialReferenceH srs = makeSpatialRef( 3857 );
    if ( !srs )
    {
      GDALClose( dataset );
      return false;
    }
    OGRLayerH layer = GDALDatasetCreateLayer( dataset, "lines", srs, wkbLineString, nullptr );
    OSRDestroySpatialReference( srs );
    if ( !layer )
    {
      GDALClose( dataset );
      return false;
    }
    OGRFieldDefnH idField = OGR_Fld_Create( "id", OFTString );
    OGR_L_CreateField( layer, idField, true );
    OGR_Fld_Destroy( idField );
    OGRFieldDefnH typeField = OGR_Fld_Create( "type", OFTString );
    OGR_L_CreateField( layer, typeField, true );
    OGR_Fld_Destroy( typeField );
    OGRFeatureH feature = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
    OGR_F_SetFieldString( feature, 0, "stop-cross" );
    OGR_F_SetFieldString( feature, 1, "contour_stop" );
    OGRGeometryH geometry = OGR_G_CreateGeometry( wkbLineString );
    OGR_G_AddPoint_2D( geometry, x0, y0 );
    OGR_G_AddPoint_2D( geometry, x1, y1 );
    OGR_F_SetGeometry( feature, geometry );
    OGR_G_DestroyGeometry( geometry );
    const OGRErr written = OGR_L_CreateFeature( layer, feature );
    OGR_F_Destroy( feature );
    GDALClose( dataset );
    return written == OGRERR_NONE && QFile::exists( path );
  }

  static bool declareRaster( Fixture &f, const QString &path, QString *error )
  {
    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "factor.T1.sandthick" );
    decl.horizon = QStringLiteral( "T1" );
    decl.type = QStringLiteral( "raster" );
    decl.source = path;
    decl.group = QStringLiteral( "04_SingleFactor" );
    return f.layers.declare( decl, error );
  }

  static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
  {
    const QVector<LayerDeclaration> decls = layers.declared();
    for ( const LayerDeclaration &d : decls )
    {
      if ( d.layerId == layerId )
        return new LayerDeclaration( d );
    }
    return nullptr;
  }

  // U8 读侧口径：直接数落盘 GPKG 的 contours 图层要素数，作为 extra 里
  // contour_feature_count 的独立对照（不经被测代码）。
  static int countContourFeatures( const QString &gpkgPath )
  {
    GDALDatasetH dataset = GDALOpenEx( gpkgPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                       nullptr, nullptr, nullptr );
    if ( !dataset )
      return -1;
    OGRLayerH layer = GDALDatasetGetLayerByName( dataset, "contours" );
    const int count = layer ? static_cast<int>( OGR_L_GetFeatureCount( layer, TRUE ) ) : -1;
    GDALClose( dataset );
    return count;
  }

  // 按 extra.kind 定位本次发布登记的版本（fixture 各用例独立 catalog）。
  static QVariantMap findVersionExtra( DataCatalog &catalog, const QString &kind )
  {
    for ( const CatalogAsset &asset : catalog.assets() )
    {
      for ( const CatalogVersion &version : catalog.versionsForAsset( asset.id ) )
      {
        if ( version.extra.value( QStringLiteral( "kind" ) ).toString() == kind )
          return version.extra;
      }
    }
    return QVariantMap();
  }

private slots:
  void initTestCase()
  {
    QVERIFY( QgsApplication::instance() != nullptr );
  }

  void cleanup()
  {
    QgsProject::instance()->removeAllMapLayers();
  }

  void fixedLevelsPublishContourLayer()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::AnalysisContourJob job;
    QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 2.0, 4.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeAnalysisContourJob( &job ), qPrintable( job.error ) );
    QVERIFY( job.outputPath.contains( QStringLiteral( "paleo-sf-contour-" ) ) );
    QVERIFY( job.outputPath.endsWith( QStringLiteral( "contours.gpkg" ) ) );
    QVERIFY2( wf.publishAnalysisContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( contours.count(), 1 );
    QCOMPARE( contours.at( 0 ).at( 2 ).toString(), QStringLiteral( "contours.T1.sandthick" ) );
    const LayerDeclaration *lines = findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) );
    QVERIFY( lines != nullptr );
    QCOMPARE( lines->group, QStringLiteral( "04_SingleFactor/Contours" ) );
    // U8：等值线版本 extra 统一含 contour_feature_count——与分析等值线既有
    // analysis_sha256 并列，值须如实等于落盘 GPKG 要素数。
    const QVariantMap extra = findVersionExtra( f.catalog, QStringLiteral( "contour_lines" ) );
    QVERIFY2( !extra.isEmpty(), "contour_lines version must be registered" );
    QVERIFY( !extra.value( QStringLiteral( "analysis_sha256" ) ).toString().isEmpty() );
    const int actual = countContourFeatures( lines->source.section( QLatin1Char( '|' ), 0, 0 ) );
    QVERIFY2( actual > 0, "ramp with in-range levels must have contour features" );
    QCOMPARE( extra.value( QStringLiteral( "contour_feature_count" ) ).toInt(), actual );
    delete lines;
  }

  void staleGenerationDropsFirstContourPublish()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::AnalysisContourJob first;
    ConstraintWorkflow::AnalysisContourJob second;
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 2.0, 4.0 }, true, &first, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeAnalysisContourJob( &first ), qPrintable( first.error ) );
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 2.0, 4.0 }, true, &second, &err ),
              qPrintable( err ) );
    QVERIFY( second.generation != first.generation );
    QVERIFY( !wf.publishAnalysisContourJob( first, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "发布代次" ) ), qPrintable( err ) );
    QVERIFY( findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) ) == nullptr );
  }

  void rewrittenAnalysisDropsContourPublish()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::AnalysisContourJob job;
    QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 2.0, 4.0 }, true, &job, &err ),
              qPrintable( err ) );
    // #421：analysisSha 在 compute 阶段随指纹读取盖章（prepare 只钉 mtime/size
    // 快照）——断言顺序对齐现行契约（#349 起 prepare 不在 GUI 线程算 sha）。
    QVERIFY2( wf.computeAnalysisContourJob( &job ), qPrintable( job.error ) );
    QCOMPARE( job.analysisSha, DataCatalog::sha256FileHex( rasterPath ) );
    QVERIFY( !job.analysisSha.isEmpty() );
    QVERIFY( QFile::remove( rasterPath ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 6, 500000.0, 4001000.0, 10.0 ) );
    QVERIFY( DataCatalog::sha256FileHex( rasterPath ) != job.analysisSha );
    QVERIFY( !wf.publishAnalysisContourJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "分析场在等值线期间被改写" ) ), qPrintable( err ) );
    QCOMPARE( contours.count(), 0 );
    QVERIFY( findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) ) == nullptr );
  }

  void interpretiveCrossingKeepsAnalysisSha()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const double originX = 500000.0;
    const double originY = 4002000.0;
    const double cell = 50.0;
    const int cols = 24;
    const int rows = 8;
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, cols, rows, originX, originY, cell ) );
    const QString sha = DataCatalog::sha256FileHex( rasterPath );
    QVERIFY( !sha.isEmpty() );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    const QString linesPath = f.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
    const double y = originY - 4.0 * cell;
    QVERIFY( writeStopLine( linesPath, originX - cell, y, originX + cols * cell + cell, y ) );
    LayerDeclaration stop;
    stop.layerId = QStringLiteral( "constraints.T1" );
    stop.horizon = QStringLiteral( "T1" );
    stop.type = QStringLiteral( "vector" );
    stop.source = linesPath + QStringLiteral( "|layername=lines" );
    stop.group = PaleoLayerVocabulary::kConstraintsGroup;
    QVERIFY2( f.layers.declare( stop, &err ), qPrintable( err ) );

    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );
    ConstraintWorkflow::InterpretiveContourJob job;
    QSignalSpy interpretive( &wf, &ConstraintWorkflow::interpretiveContoursGenerated );
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                               QVector<double>{ 4.0, 8.0, 16.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeInterpretiveContourJob( &job ), qPrintable( job.error ) );
    QVERIFY( job.workPath.contains( QStringLiteral( "paleo-sf-carto-" ) ) );
    QVERIFY2( wf.publishInterpretiveContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( interpretive.count(), 1 );
    QVERIFY( interpretive.at( 0 ).at( 2 ).toString().startsWith( QStringLiteral( "cartographic." ) ) );
    QCOMPARE( DataCatalog::sha256FileHex( rasterPath ), sha );
    const LayerDeclaration *work = findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
    QVERIFY( work != nullptr );
    const QFileInfo workInfo( work->source.section( QLatin1Char( '|' ), 0, 0 ) );
    QFile qcFile( workInfo.absolutePath() + QLatin1Char( '/' ) + workInfo.completeBaseName() +
                  QStringLiteral( ".qc.json" ) );
    QVERIFY2( qcFile.open( QIODevice::ReadOnly ), qPrintable( qcFile.fileName() ) );
    const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
    QVERIFY2( qc.value( QStringLiteral( "modified_cells" ) ).toInt() > 0, "stop line should cross the ramp" );
    QCOMPARE( qc.value( QStringLiteral( "unresolved_crossings" ) ).toInt(), 0 );
    delete work;
    // U8：解释性等值线版本 extra 补 analysis_sha256 + contour_feature_count，
    // 键契约与分析等值线版本统一（读侧按同一键消费）。本夹具是穿线 QC：
    // 工作场向级别间隙值弯折，等值线贴线处本就无要素（0 条是如实计数）。
    const QVariantMap contourExtra = findVersionExtra( f.catalog, QStringLiteral( "single_factor_cartographic_contour" ) );
    QVERIFY2( !contourExtra.isEmpty(), "cartographic contour version must be registered" );
    QVERIFY2( contourExtra.contains( QStringLiteral( "contour_feature_count" ) ),
              "contour_feature_count must be present" );
    QCOMPARE( contourExtra.value( QStringLiteral( "analysis_sha256" ) ).toString(), sha );
    const LayerDeclaration *contourLines = findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) );
    QVERIFY( contourLines != nullptr );
    QCOMPARE( contourExtra.value( QStringLiteral( "contour_feature_count" ) ).toInt(),
              countContourFeatures( contourLines->source.section( QLatin1Char( '|' ), 0, 0 ) ) );
    delete contourLines;
  }

  void retargetedFactorDropsAnalysisContourPublish()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::AnalysisContourJob job;
    QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 2.0, 4.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeAnalysisContourJob( &job ), qPrintable( job.error ) );
    const QString copied = f.dir.filePath( QStringLiteral( "ramp-retarget.tif" ) );
    QVERIFY( QFile::copy( rasterPath, copied ) );
    QCOMPARE( DataCatalog::sha256FileHex( copied ), job.analysisSha );
    QVERIFY2( declareRaster( f, copied, &err ), qPrintable( err ) );
    QVERIFY( !wf.publishAnalysisContourJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "分析场在等值线期间被改写" ) ), qPrintable( err ) );
    QCOMPARE( contours.count(), 0 );
    QVERIFY( findDecl( f.layers, QStringLiteral( "contours.T1.sandthick" ) ) == nullptr );
  }

  void retargetedFactorDropsInterpretiveContourPublish()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::InterpretiveContourJob job;
    QSignalSpy interpretive( &wf, &ConstraintWorkflow::interpretiveContoursGenerated );
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                               QVector<double>{ 2.0, 4.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeInterpretiveContourJob( &job ), qPrintable( job.error ) );
    const QString copied = f.dir.filePath( QStringLiteral( "ramp-retarget.tif" ) );
    QVERIFY( QFile::copy( rasterPath, copied ) );
    QCOMPARE( DataCatalog::sha256FileHex( copied ), job.analysisSha );
    QCOMPARE( DataCatalog::sha256FileHex( rasterPath ), job.analysisSha );
    QVERIFY2( declareRaster( f, copied, &err ), qPrintable( err ) );
    QVERIFY( !wf.publishInterpretiveContourJob( job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "分析场在制图期间被改写" ) ), qPrintable( err ) );
    QCOMPARE( interpretive.count(), 0 );
    QVERIFY( findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick" ) ) == nullptr );
    QVERIFY( findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) ) == nullptr );
  }

  void interpretiveComputeOnWorkerThreadPublishes()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const double originX = 500000.0;
    const double originY = 4002000.0;
    const double cell = 50.0;
    const int cols = 24;
    const int rows = 8;
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, cols, rows, originX, originY, cell ) );
    const QString sha = DataCatalog::sha256FileHex( rasterPath );
    QVERIFY( !sha.isEmpty() );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    const QString linesPath = f.dir.filePath( QStringLiteral( "constraints.gpkg" ) );
    const double y = originY - 4.0 * cell;
    QVERIFY( writeStopLine( linesPath, originX - cell, y, originX + cols * cell + cell, y ) );
    LayerDeclaration stop;
    stop.layerId = QStringLiteral( "constraints.T1" );
    stop.horizon = QStringLiteral( "T1" );
    stop.type = QStringLiteral( "vector" );
    stop.source = linesPath + QStringLiteral( "|layername=lines" );
    stop.group = PaleoLayerVocabulary::kConstraintsGroup;
    QVERIFY2( f.layers.declare( stop, &err ), qPrintable( err ) );

    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );
    ConstraintWorkflow::InterpretiveContourJob job;
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                               QVector<double>{ 4.0, 8.0, 16.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY( !job.constraintLines.empty() );

    bool ran = false;
    QString computeError;
    QThread *thread = QThread::create( [&] {
      ran = wf.computeInterpretiveContourJob( &job );
      computeError = job.error;
    } );
    thread->start();
    QVERIFY( thread->wait( 60000 ) );
    delete thread;
    QVERIFY2( ran, qPrintable( computeError ) );
    QVERIFY2( wf.publishInterpretiveContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( DataCatalog::sha256FileHex( rasterPath ), sha );
    const LayerDeclaration *work = findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick" ) );
    QVERIFY( work != nullptr );
    const QFileInfo workInfo( work->source.section( QLatin1Char( '|' ), 0, 0 ) );
    QFile qcFile( workInfo.absolutePath() + QLatin1Char( '/' ) + workInfo.completeBaseName() +
                  QStringLiteral( ".qc.json" ) );
    QVERIFY2( qcFile.open( QIODevice::ReadOnly ), qPrintable( qcFile.fileName() ) );
    const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
    QVERIFY2( qc.value( QStringLiteral( "modified_cells" ) ).toInt() > 0, "stop line should cross the ramp" );
    QCOMPARE( qc.value( QStringLiteral( "unresolved_crossings" ) ).toInt(), 0 );
    delete work;
  }

  // U8：常数场 + fixed levels 全越域 → 仍成功发布（0 条是既有成功语义），
  // 但 extra.contour_feature_count 必须如实为 0、analysis_sha256 如实盖章。
  void constantFieldPublishesZeroContourFeatureCount()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "constant.tif" ) );
    QVERIFY( writeConstantFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0, 5.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::AnalysisContourJob job;
    QSignalSpy contours( &wf, &ConstraintWorkflow::contoursGenerated );
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            QVector<double>{ 1.0, 3.0, 7.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeAnalysisContourJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishAnalysisContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( contours.count(), 1 );
    const QVariantMap extra = findVersionExtra( f.catalog, QStringLiteral( "contour_lines" ) );
    QVERIFY2( !extra.isEmpty(), "contour_lines version must be registered" );
    QVERIFY2( extra.contains( QStringLiteral( "contour_feature_count" ) ),
              "contour_feature_count must be present even when zero" );
    QVERIFY( !extra.value( QStringLiteral( "analysis_sha256" ) ).toString().isEmpty() );
    QCOMPARE( extra.value( QStringLiteral( "contour_feature_count" ) ).toInt(), 0 );
  }

  // U8 解释性正向计数：无约束斜坡场 + 域内级别 → 等值线要素数如实入
  // extra（工作场不变更，计数与分析等值线路径同源同口径）。
  void interpretiveRampCountsContourFeatures()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    const QString sha = DataCatalog::sha256FileHex( rasterPath );
    QVERIFY( !sha.isEmpty() );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::InterpretiveContourJob job;
    QSignalSpy interpretive( &wf, &ConstraintWorkflow::interpretiveContoursGenerated );
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                               QVector<double>{ 2.0, 4.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeInterpretiveContourJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishInterpretiveContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( interpretive.count(), 1 );
    const QVariantMap contourExtra =
        findVersionExtra( f.catalog, QStringLiteral( "single_factor_cartographic_contour" ) );
    QVERIFY2( !contourExtra.isEmpty(), "cartographic contour version must be registered" );
    QCOMPARE( contourExtra.value( QStringLiteral( "analysis_sha256" ) ).toString(), sha );
    const LayerDeclaration *contourLines = findDecl( f.layers, QStringLiteral( "cartographic.T1.sandthick.contours" ) );
    QVERIFY( contourLines != nullptr );
    const int actual = countContourFeatures( contourLines->source.section( QLatin1Char( '|' ), 0, 0 ) );
    QVERIFY2( actual > 0, "ramp with in-range levels must have contour features" );
    QCOMPARE( contourExtra.value( QStringLiteral( "contour_feature_count" ) ).toInt(), actual );
    delete contourLines;
  }

  // U8 解释性对偶：常数场发布成功，等值线版本 analysis_sha256 存在且
  // contour_feature_count==0。
  void interpretiveConstantFieldCountsZeroContourFeatures()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "constant.tif" ) );
    QVERIFY( writeConstantFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0, 5.0 ) );
    const QString sha = DataCatalog::sha256FileHex( rasterPath );
    QVERIFY( !sha.isEmpty() );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    ConstraintWorkflow::InterpretiveContourJob job;
    QSignalSpy interpretive( &wf, &ConstraintWorkflow::interpretiveContoursGenerated );
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                               QVector<double>{ 1.0, 3.0, 7.0 }, true, &job, &err ),
              qPrintable( err ) );
    QVERIFY2( wf.computeInterpretiveContourJob( &job ), qPrintable( job.error ) );
    QVERIFY2( wf.publishInterpretiveContourJob( job, &err ), qPrintable( err ) );

    QCOMPARE( interpretive.count(), 1 );
    const QVariantMap contourExtra =
        findVersionExtra( f.catalog, QStringLiteral( "single_factor_cartographic_contour" ) );
    QVERIFY2( !contourExtra.isEmpty(), "cartographic contour version must be registered" );
    QVERIFY2( contourExtra.contains( QStringLiteral( "contour_feature_count" ) ),
              "contour_feature_count must be present even when zero" );
    QCOMPARE( contourExtra.value( QStringLiteral( "analysis_sha256" ) ).toString(), sha );
    QCOMPARE( contourExtra.value( QStringLiteral( "contour_feature_count" ) ).toInt(), 0 );
  }

  // U9：分析等值线 fixed levels 超上限在 prepare 即拒（GDAL 之前），
  // 等于上限是合法边界。
  void fixedLevelsAboveCapRejectedAtPrepare()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    const std::size_t cap = paleo::singlefactor::kMaxContourLevels;
    QVector<double> tooMany;
    tooMany.reserve( static_cast<int>( cap ) + 1 );
    for ( std::size_t i = 0; i <= cap; ++i )
      tooMany << 1.0 + static_cast<double>( i ) * 1e-6;
    ConstraintWorkflow::AnalysisContourJob job;
    QVERIFY( !wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            tooMany, true, &job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "级别数超过上限" ) ), qPrintable( err ) );
    QVERIFY2( err.contains( QString::number( static_cast<qulonglong>( cap ) ) ), qPrintable( err ) );

    // 边界：恰为上限个级别 → prepare 通过（不进 compute，避免真跑 GDAL）。
    QVector<double> atCap;
    atCap.reserve( static_cast<int>( cap ) );
    for ( std::size_t i = 0; i < cap; ++i )
      atCap << 1.0 + static_cast<double>( i ) * 1e-6;
    err.clear();
    QVERIFY2( wf.prepareAnalysisContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 0.0,
                                            atCap, true, &job, &err ),
              qPrintable( err ) );
  }

  // U9：解释性等值线同一上限闸（prepare 级，防脚本直调工作流层）。
  void interpretiveLevelsAboveCapRejectedAtPrepare()
  {
    Fixture f;
    QVERIFY( initFixture( f ) );
    const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
    QVERIFY( writeNorthUpFloat32( rasterPath, 8, 4, 500000.0, 4001000.0, 10.0 ) );
    QString err;
    QVERIFY2( declareRaster( f, rasterPath, &err ), qPrintable( err ) );
    ConstraintWorkflow wf( &f.proc, &f.layers );
    wf.setCatalog( &f.catalog, f.dir.path() );

    const std::size_t cap = paleo::singlefactor::kMaxContourLevels;
    QVector<double> tooMany;
    tooMany.reserve( static_cast<int>( cap ) + 1 );
    for ( std::size_t i = 0; i <= cap; ++i )
      tooMany << 1.0 + static_cast<double>( i ) * 1e-6;
    ConstraintWorkflow::InterpretiveContourJob job;
    QVERIFY( !wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                                tooMany, true, &job, &err ) );
    QVERIFY2( err.contains( QStringLiteral( "级别数超过上限" ) ), qPrintable( err ) );
    QVERIFY2( err.contains( QString::number( static_cast<qulonglong>( cap ) ) ), qPrintable( err ) );

    QVector<double> atCap;
    atCap.reserve( static_cast<int>( cap ) );
    for ( std::size_t i = 0; i < cap; ++i )
      atCap << 1.0 + static_cast<double>( i ) * 1e-6;
    err.clear();
    QVERIFY2( wf.prepareInterpretiveContourJob( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                                atCap, true, &job, &err ),
              qPrintable( err ) );
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestSingleFactorAsyncContour tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_singlefactor_asynccontour.moc"
