// 层：测试壳
// 单因素公开 API 的故障：等值线失败不声明、只读产物目录留下上一版、清单只读不新增图层、孤儿文件不是成果。
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVector>

#include <qgsapplication.h>
#include <qgsproject.h>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <sys/stat.h>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/metastore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/workflows.h"

namespace
{

QByteArray readBytes( const QString &path )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return QByteArray();
  return file.readAll();
}

bool writeBytes( const QString &path, const QByteArray &bytes )
{
  QFile file( path );
  if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    return false;
  return file.write( bytes ) == bytes.size();
}

bool canCreateFile( const QString &directory )
{
  const QString probe = QDir( directory ).filePath( QStringLiteral( ".paleo_write_probe" ) );
  QFile file( probe );
  const bool opened = file.open( QIODevice::WriteOnly | QIODevice::Truncate );
  if ( opened )
    file.close();
  file.remove();
  return opened;
}

// 已打开的 sqlite fd 不吃文件 mode；目录去掉写位才能挡住 journal 创建。
// 析构恢复，避免临时目录删不掉。
struct ModeRestore
{
  QString path;
  // 读写位数走 Qt 的 permission 面而不是裸 POSIX mode_t：Windows 也有
  // QFile::Permissions 语义（映射到只读属性），同一份断言在两侧同义。
  QFile::Permissions mode = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
  bool armed = false;

  void release()
  {
    if ( !armed )
      return;
    QFile::setPermissions( path, mode );
    armed = false;
  }

  ~ModeRestore() { release(); }

  bool restrictWrites( const QString &target, QString *error )
  {
    release();
    if ( !QFileInfo::exists( target ) )
    {
      if ( error )
        *error = QStringLiteral( "stat failed: %1" ).arg( target );
      return false;
    }
    path = target;
    mode = QFile::permissions( target );
    const QFile::Permissions writeBits =
        QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther;
    const QFile::Permissions restricted = mode & ~writeBits;
    if ( !QFile::setPermissions( target, restricted ) )
    {
      if ( error )
        *error = QStringLiteral( "chmod failed: %1" ).arg( target );
      return false;
    }
    armed = true;
    return true;
  }
};

} // namespace

// QSKIP 的 return 必须留在测试函数里，不能包进帮手。
#define PALEO_SKIP_NOT_A_PASS( messageExpr ) \
  do \
  { \
    const QByteArray paleoSkipMessage = ( messageExpr ).toUtf8(); \
    QSKIP( paleoSkipMessage.constData() ); \
  } while ( false )

class TestSingleFactorFaults : public QObject
{
  Q_OBJECT

private:
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

  static LayerDeclaration decl( const QString &layerId, const QString &horizon, const QString &type,
                                const QString &source, const QString &group = QStringLiteral( "00_Test" ) )
  {
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = type;
    d.source = source;
    d.group = group;
    return d;
  }

  static bool findDecl( const QVector<LayerDeclaration> &decls, const QString &layerId, LayerDeclaration *out )
  {
    for ( const LayerDeclaration &d : decls )
    {
      if ( d.layerId != layerId )
        continue;
      if ( out )
        *out = d;
      return true;
    }
    return false;
  }

  static OGRSpatialReferenceH makeSpatialRef( int epsg )
  {
    if ( epsg <= 0 )
      return nullptr;
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

  // 列号即值的 north-up 斜坡。与 tst_factorworkflow 的固定级别夹具同一写法。
  static bool writeRampTiff( const QString &path, int epsg, int cols, int rows, double originX, double originY,
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
    OGRSpatialReferenceH srs = makeSpatialRef( epsg );
    if ( !srs )
    {
      GDALClose( dataset );
      return false;
    }
    char *wkt = nullptr;
    OSRExportToWkt( srs, &wkt );
    OSRDestroySpatialReference( srs );
    if ( !wkt )
    {
      GDALClose( dataset );
      return false;
    }
    const CPLErr projected = GDALSetProjection( dataset, wkt );
    CPLFree( wkt );
    if ( projected != CE_None )
    {
      GDALClose( dataset );
      return false;
    }
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

  // 写入成功并删掉探针行才算清单仍可写。拒绝写入时不留 probe.readonly。
  static bool manifestWriteLands( Fixture &f, QString *error )
  {
    LayerDeclaration probe = decl( QStringLiteral( "probe.readonly" ), QStringLiteral( "T1" ),
                                   QStringLiteral( "raster" ), QStringLiteral( "memory:probe" ) );
    if ( !f.layers.declare( probe, error ) )
      return false;
    QString removeErr;
    if ( !f.manifest.remove( probe.layerId, &removeErr ) && error )
      *error = removeErr;
    return true;
  }

private slots:
  void initTestCase()
  {
    QVERIFY( QgsApplication::instance() != nullptr );
    QVERIFY( GDALGetDriverByName( "GTiff" ) != nullptr );
  }

  void cleanup() { QgsProject::instance()->removeAllMapLayers(); }

  void gdalContourFailureDeclaresNothing();
  void readOnlyArtifactsLeavePriorVersion();
  void manifestReadOnlyDeclareFailsWithoutNewLayer();
  void orphanFileIsNotAProduct();
};

void TestSingleFactorFaults::gdalContourFailureDeclaresNothing()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QString rasterPath = f.dir.filePath( QStringLiteral( "analysis-not-raster.tif" ) );
  QVERIFY( writeBytes( rasterPath, QByteArray( "placeholder\n" ) ) );
  QString err;
  QVERIFY2( f.layers.declare( decl( QStringLiteral( "factor.T1.sandthick" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), rasterPath,
                                    QStringLiteral( "04_SingleFactor" ) ),
                              &err ),
            qPrintable( err ) );
  // 声明之后源仍不是栅格。生成不得改这批字节，也不得声明等值线。
  const QByteArray payload( "not-a-geotiff\n" );
  QVERIFY( writeBytes( rasterPath, payload ) );
  const QByteArray before = readBytes( rasterPath );
  QCOMPARE( before, payload );

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QVERIFY2( !wf.generateContours( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ), 1.0, &err ),
            qPrintable( err ) );
  QVERIFY2( err.contains( rasterPath ), qPrintable( err ) );
  QCOMPARE( readBytes( rasterPath ), before );

  QVector<LayerDeclaration> decls;
  QString readErr;
  QVERIFY2( f.layers.tryDeclared( &decls, &readErr ), qPrintable( readErr ) );
  LayerDeclaration factor;
  QVERIFY( findDecl( decls, QStringLiteral( "factor.T1.sandthick" ), &factor ) );
  QCOMPARE( factor.source, rasterPath );
  QVERIFY( !findDecl( decls, QStringLiteral( "contours.T1.sandthick" ), nullptr ) );
}

void TestSingleFactorFaults::readOnlyArtifactsLeavePriorVersion()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QString catalogFile = QFileInfo( f.catalog.catalogPath() ).absoluteFilePath();
  const QString metadataDir = QFileInfo( catalogFile ).dir().absolutePath();
  const QString artifacts = QFileInfo( metadataDir ).dir().absolutePath();
  const QString inspected = QStringLiteral( "%1 (catalog file %2)" ).arg( artifacts, catalogFile );
  if ( !QFileInfo( artifacts ).isDir() || QFileInfo( artifacts ).fileName() != QLatin1String( "artifacts" ) ||
       !canCreateFile( artifacts ) )
  {
    PALEO_SKIP_NOT_A_PASS(
        QStringLiteral( "catalog artifact directory is not writable; inspected %1. This skip is not a pass." )
            .arg( inspected ) );
  }

  const QString rasterPath = f.dir.filePath( QStringLiteral( "ramp.tif" ) );
  QVERIFY( writeRampTiff( rasterPath, 3857, 80, 40, 500000.0, 4001000.0, 10.0 ) );
  QVERIFY( !QDir::cleanPath( rasterPath ).startsWith( QDir::cleanPath( artifacts ) + QLatin1Char( '/' ) ) );
  const QByteArray before = readBytes( rasterPath );
  QVERIFY( !before.isEmpty() );
  QString err;
  QVERIFY2( f.layers.declare( decl( QStringLiteral( "factor.T1.sandthick" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), rasterPath,
                                    QStringLiteral( "04_SingleFactor" ) ),
                              &err ),
            qPrintable( err ) );
  QVector<LayerDeclaration> declaredBefore;
  QVERIFY2( f.layers.tryDeclared( &declaredBefore, &err ), qPrintable( err ) );
  LayerDeclaration prior;
  QVERIFY( findDecl( declaredBefore, QStringLiteral( "factor.T1.sandthick" ), &prior ) );

  ModeRestore guard;
  QString modeErr;
  QVERIFY2( guard.restrictWrites( artifacts, &modeErr ), qPrintable( modeErr ) );
  QVERIFY( guard.path != rasterPath );
  if ( canCreateFile( artifacts ) )
  {
    // root（POSIX）忽略目录 mode 位，chmod 挡不住写——此时跳过而非伪通过。
    // Windows 无 geteuid，按「非特权」处理（只读属性对普通用户有效）。
    bool elevated = false;
#ifndef Q_OS_WIN
    elevated = (::geteuid() == 0);
#endif
    const QString message =
        elevated
            ? QStringLiteral( "cannot force a read-only artifact directory at %1 because euid is 0 and mode bits "
                              "are ignored. This skip is not a pass." )
                  .arg( artifacts )
            : QStringLiteral( "chmod did not make catalog artifact directory read-only: %1. This skip is not a pass." )
                  .arg( artifacts );
    PALEO_SKIP_NOT_A_PASS( message );
  }

  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QVERIFY2( !wf.generateContoursAtLevels( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                          QVector<double>{ 2.0, 4.0 }, &err ),
            qPrintable( err ) );
  QVERIFY2( err.contains( QStringLiteral( "无法创建派生产物目录" ) ), qPrintable( err ) );
  guard.release();
  QCOMPARE( readBytes( rasterPath ), before );

  const QString manifestPath = QFileInfo( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) ).absoluteFilePath();
  MetaStore::closeConnectionsFor( manifestPath );
  LayerManifest reopened( manifestPath );
  QString openErr;
  QVERIFY2( reopened.open( &openErr ), qPrintable( openErr ) );
  QVector<LayerDeclaration> decls;
  QString readErr;
  QVERIFY2( reopened.readAll( &decls, &readErr ), qPrintable( readErr ) );
  LayerDeclaration after;
  QVERIFY( findDecl( decls, QStringLiteral( "factor.T1.sandthick" ), &after ) );
  QCOMPARE( after.layerId, prior.layerId );
  QCOMPARE( after.source, prior.source );
  QCOMPARE( after.type, prior.type );
  QCOMPARE( after.group, prior.group );
  QVERIFY( !findDecl( decls, QStringLiteral( "contours.T1.sandthick" ), nullptr ) );
  QCOMPARE( readBytes( rasterPath ), before );
}

void TestSingleFactorFaults::manifestReadOnlyDeclareFailsWithoutNewLayer()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QString manifestPath = QFileInfo( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) ).absoluteFilePath();
  const QString catalogFile = QFileInfo( f.catalog.catalogPath() ).absoluteFilePath();
  if ( manifestPath == catalogFile )
  {
    PALEO_SKIP_NOT_A_PASS(
        QStringLiteral( "layer manifest and catalog share one sqlite (%1); cannot separate declare from "
                        "the catalog. This skip is not a pass." )
            .arg( manifestPath ) );
  }

  const QString analysisDir = f.dir.filePath( QStringLiteral( "analysis" ) );
  QVERIFY( QDir().mkpath( analysisDir ) );
  const QString rasterPath = QDir( analysisDir ).filePath( QStringLiteral( "ramp.tif" ) );
  QVERIFY( rasterPath != manifestPath );
  QVERIFY( rasterPath != catalogFile );
  QVERIFY( writeRampTiff( rasterPath, 3857, 80, 40, 500000.0, 4001000.0, 10.0 ) );
  const QByteArray before = readBytes( rasterPath );
  QVERIFY( !before.isEmpty() );
  const QFile::Permissions rasterMode = QFile::permissions( rasterPath );
  QString err;
  QVERIFY2( f.layers.declare( decl( QStringLiteral( "factor.T1.sandthick" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), rasterPath,
                                    QStringLiteral( "04_SingleFactor" ) ),
                              &err ),
            qPrintable( err ) );
  QString probeErr;
  QVERIFY2( manifestWriteLands( f, &probeErr ), qPrintable( probeErr ) );
  QVector<LayerDeclaration> baseline;
  QVERIFY2( f.layers.tryDeclared( &baseline, &err ), qPrintable( err ) );
  QVERIFY( !findDecl( baseline, QStringLiteral( "probe.readonly" ), nullptr ) );

  ModeRestore guard;
  QString modeErr;
  QVERIFY2( guard.restrictWrites( f.dir.path(), &modeErr ), qPrintable( modeErr ) );
  QVERIFY( guard.path != rasterPath );
  QVERIFY( QFileInfo( rasterPath ).isReadable() );
  QCOMPARE( QFile::permissions( rasterPath ), rasterMode );
  if ( manifestWriteLands( f, &probeErr ) )
  {
    QVector<LayerDeclaration> afterProbe;
    QVERIFY2( f.layers.tryDeclared( &afterProbe, &err ), qPrintable( err ) );
    QVERIFY2( !findDecl( afterProbe, QStringLiteral( "probe.readonly" ), nullptr ), qPrintable( probeErr ) );
    guard.release();
    // 目录仍可写时，关掉已打开的连接再收文件 mode，让下一次打开真正被拒绝。
    MetaStore::closeConnectionsFor( manifestPath );
    QVERIFY2( guard.restrictWrites( manifestPath, &modeErr ), qPrintable( modeErr ) );
    QVERIFY( guard.path != rasterPath );
    QCOMPARE( QFile::permissions( rasterPath ), rasterMode );
    if ( manifestWriteLands( f, &probeErr ) )
    {
      PALEO_SKIP_NOT_A_PASS(
          QStringLiteral( "cannot force layer manifest %1 read-only (directory mode and file mode still "
                          "accept declare). This skip is not a pass." )
              .arg( manifestPath ) );
    }
  }

  QVERIFY( QFileInfo( rasterPath ).isReadable() );
  QCOMPARE( QFile::permissions( rasterPath ), rasterMode );
  ConstraintWorkflow wf( &f.proc, &f.layers );
  wf.setCatalog( &f.catalog, f.dir.path() );
  QVERIFY2( !wf.generateContoursAtLevels( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                          QVector<double>{ 10.0, 30.0 }, &err ),
            qPrintable( err ) );
  guard.release();
  QCOMPARE( readBytes( rasterPath ), before );
  QCOMPARE( QFile::permissions( rasterPath ), rasterMode );

  MetaStore::closeConnectionsFor( manifestPath );
  LayerManifest reopened( manifestPath );
  QString openErr;
  QVERIFY2( reopened.open( &openErr ), qPrintable( openErr ) );
  QVector<LayerDeclaration> decls;
  QString readErr;
  QVERIFY2( reopened.readAll( &decls, &readErr ), qPrintable( readErr ) );
  LayerDeclaration factor;
  QVERIFY( findDecl( decls, QStringLiteral( "factor.T1.sandthick" ), &factor ) );
  QCOMPARE( factor.source, rasterPath );
  QVERIFY( !findDecl( decls, QStringLiteral( "contours.T1.sandthick" ), nullptr ) );
  QVERIFY( !findDecl( decls, QStringLiteral( "probe.readonly" ), nullptr ) );
  QCOMPARE( readBytes( rasterPath ), before );
}

void TestSingleFactorFaults::orphanFileIsNotAProduct()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QString keptDir = f.dir.filePath( QStringLiteral( "artifacts/derived/ast-real/ver-1" ) );
  const QString orphanDir = f.dir.filePath( QStringLiteral( "artifacts/derived/killed-leftover" ) );
  QVERIFY( QDir().mkpath( keptDir ) );
  QVERIFY( QDir().mkpath( orphanDir ) );
  const QString keptPath = QDir( keptDir ).filePath( QStringLiteral( "KEPT.tif" ) );
  const QString orphanPath = QDir( orphanDir ).filePath( QStringLiteral( "ORPHAN_T1.tif" ) );
  QVERIFY( writeRampTiff( keptPath, 3857, 4, 4, 0.0, 40.0, 10.0 ) );
  QVERIFY( writeRampTiff( orphanPath, 3857, 4, 4, 0.0, 40.0, 10.0 ) );
  const QString keptRel = QDir( f.dir.path() ).relativeFilePath( keptPath );
  const QString orphanRel = QDir( f.dir.path() ).relativeFilePath( orphanPath );
  QCOMPARE( orphanRel, QStringLiteral( "artifacts/derived/killed-leftover/ORPHAN_T1.tif" ) );

  QString err;
  CatalogAsset asset;
  asset.id = f.catalog.nextAssetId();
  asset.type = QStringLiteral( "single_factor_raster" );
  asset.format = QStringLiteral( "tif" );
  asset.displayName = QStringLiteral( "KEPT.tif" );
  QVERIFY2( f.catalog.addAsset( asset, &err ), qPrintable( err ) );
  CatalogVersion version;
  version.id = f.catalog.nextVersionId();
  version.assetId = asset.id;
  version.stage = QStringLiteral( "DERIVED" );
  version.versionNumber = 1;
  version.managed = true;
  version.path = keptRel;
  version.fileName = QStringLiteral( "KEPT.tif" );
  version.sha256 = DataCatalog::sha256FileHex( keptPath );
  QVERIFY( !version.sha256.isEmpty() );
  QVERIFY2( f.catalog.addVersion( version, &err ), qPrintable( err ) );
  QVERIFY2( f.layers.declare( decl( QStringLiteral( "factor.T1.sandthick" ), QStringLiteral( "T1" ),
                                    QStringLiteral( "raster" ), keptPath, QStringLiteral( "04_SingleFactor" ) ),
                              &err ),
            qPrintable( err ) );

  DataCatalog reopened;
  QVERIFY2( reopened.open( f.dir.path(), &err ), qPrintable( err ) );
  bool sawKept = false;
  bool sawOrphan = false;
  const auto note = [&]( const CatalogVersion &candidate ) {
    if ( QDir::cleanPath( candidate.path ) == QDir::cleanPath( orphanRel ) )
      sawOrphan = true;
    if ( QDir::cleanPath( candidate.path ) == QDir::cleanPath( keptRel ) )
      sawKept = true;
    const QString resolved = DataCatalog::resolvedVersionPath( f.dir.path(), candidate );
    if ( resolved.isEmpty() )
      return;
    const QString resolvedCanonical = QFileInfo( resolved ).canonicalFilePath();
    const QString orphanCanonical = QFileInfo( orphanPath ).canonicalFilePath();
    if ( !resolvedCanonical.isEmpty() && resolvedCanonical == orphanCanonical )
      sawOrphan = true;
  };
  for ( const CatalogVersion &candidate : reopened.versions() )
    note( candidate );
  for ( const CatalogAsset &candidate : reopened.assets() )
    note( reopened.currentVersion( candidate.id ) );
  QVERIFY( sawKept );
  QVERIFY( !sawOrphan );
  QVERIFY( QFile::exists( orphanPath ) );

  const QString manifestPath = QFileInfo( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) ).absoluteFilePath();
  MetaStore::closeConnectionsFor( manifestPath );
  LayerManifest reopenedManifest( manifestPath );
  QVERIFY2( reopenedManifest.open( &err ), qPrintable( err ) );
  QgisLayerService layers( &f.projectSvc, &reopenedManifest );
  QVector<LayerDeclaration> decls;
  QVERIFY2( layers.tryDeclared( &decls, &err ), qPrintable( err ) );
  bool listedOrphan = false;
  bool sawFactor = false;
  for ( const LayerDeclaration &d : decls )
  {
    const QString source = QDir::cleanPath( d.source.section( QLatin1Char( '|' ), 0, 0 ) );
    if ( source == QDir::cleanPath( orphanPath ) || d.source.contains( QStringLiteral( "ORPHAN_T1.tif" ) ) )
      listedOrphan = true;
    if ( d.layerId == QLatin1String( "factor.T1.sandthick" ) )
      sawFactor = true;
  }
  QVERIFY( sawFactor );
  QVERIFY( !listedOrphan );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true ); // distro install
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestSingleFactorFaults tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_singlefactor_faults.moc"
