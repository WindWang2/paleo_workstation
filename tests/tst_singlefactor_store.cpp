// 层：数据（测试壳位于 tests/，被测对象为约束参数存储）
#include <QtTest/QtTest>

#include "io/constraintstore.h"

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_spatialref.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

class SingleFactorStoreTests : public QObject
{
  Q_OBJECT
  private slots:
    void extendedParametersReopen();
    void readOnlyEnqueueCreatesNothing();
    void missingFileIsNotCreated();
    void newStoreCarriesCrsMetadata();
    void horizonWithPipeIsRejected();
    void legacyStoreDetectionAndExplicitMigration();
    void migrationRefusesForeignSrs();
};

void SingleFactorStoreTests::extendedParametersReopen()
{
  GDALAllRegister();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "constraints.gpkg" ) );
  ConstraintStore store( path );

  QString err;
  QVERIFY2( store.append( QStringLiteral( "D61" ), QStringLiteral( "old" ),
                           QStringLiteral( "LINESTRING(0 0, 1 0)" ), QStringLiteral( "break_line" ), 0, &err ),
            qPrintable( err ) );
  const QString json = QStringLiteral( "{\"schemaVersion\":1,\"semantic\":\"interpretive_boundary\",\"softStrength\":0.35}" );
  QVERIFY2( store.appendExtended( QStringLiteral( "D61" ), QStringLiteral( "soft" ),
                                   QStringLiteral( "LINESTRING(2 0, 2 4)" ),
                                   QStringLiteral( "interpretive_boundary" ), 0, json, 1, &err ),
            qPrintable( err ) );

  const QVector<QVariantMap> first = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( first.size(), 2 );
  QVERIFY( !first[0].contains( QStringLiteral( "params_json" ) ) );
  QCOMPARE( first[1].value( QStringLiteral( "params_json" ) ).toString(), json );
  QCOMPARE( first[1].value( QStringLiteral( "schema_version" ) ).toInt(), 1 );
  QCOMPARE( first[1].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );

  const QString updated = QStringLiteral( "{\"schemaVersion\":1,\"semantic\":\"interpretive_boundary\",\"softStrength\":0.2}" );
  QVERIFY2( store.updateParameters( QStringLiteral( "soft" ), updated, 1, QString(), &err ), qPrintable( err ) );
  QVERIFY2( store.updateParameters( QStringLiteral( "soft" ), updated, 1, QString(), &err ), qPrintable( err ) );
  const QVector<QVariantMap> again = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( again[1].value( QStringLiteral( "params_json" ) ).toString(), updated );
  QCOMPARE( again[0].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "break_line" ) );

  QVERIFY( !store.updateParameters( QStringLiteral( "missing" ), updated, 1, QString(), &err ) );
  QCOMPARE( store.load( QStringLiteral( "D61" ) ).size(), 2 );

  const QFileDevice::Permissions original = QFile::permissions( path );
  QVERIFY( QFile::setPermissions( path, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther ) );
  QVERIFY( !store.updateParameters( QStringLiteral( "soft" ), QStringLiteral( "{}" ), 1, QStringLiteral( "break_line" ),
                                     &err ) );
  QVERIFY( QFile::setPermissions( path, original ) );
  const QVector<QVariantMap> after = store.load( QStringLiteral( "D61" ) );
  QCOMPARE( after[1].value( QStringLiteral( "params_json" ) ).toString(), updated );
  QCOMPARE( after[1].value( QStringLiteral( "type" ) ).toString(), QStringLiteral( "interpretive_boundary" ) );
}

void SingleFactorStoreTests::readOnlyEnqueueCreatesNothing()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "absent.gpkg" ) );
  ConstraintStore store( path, []( const ConstraintStore::WriteFn & ) {
    return PaleoProjectStore::WriteResult{ false, QStringLiteral( "read only" ) };
  } );
  QString err;
  QVERIFY( !store.appendExtended( QStringLiteral( "D61" ), QStringLiteral( "a" ),
                                   QStringLiteral( "LINESTRING(0 0, 1 1)" ), QStringLiteral( "break_line" ), 0,
                                   QStringLiteral( "{}" ), 1, &err ) );
  QCOMPARE( err, QStringLiteral( "read only" ) );
  QVERIFY( !QFile::exists( path ) );
}

void SingleFactorStoreTests::missingFileIsNotCreated()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "missing.gpkg" ) );
  ConstraintStore store( path );
  QString err;
  QVERIFY( !store.updateParameters( QStringLiteral( "a" ), QStringLiteral( "{}" ), 1, QString(), &err ) );
  QVERIFY( !QFile::exists( path ) );
  QVERIFY( err.contains( QStringLiteral( "does not exist" ) ) );
}

namespace {
// 手工搭一个「历史」无 SRS 约束库（P0-2 fixture：constraints 层 + 行、无 SRS）。
bool createLegacyNoSrsStore( const QString &path, QString *error )
{
  GDALDriverH driver = GDALGetDriverByName( "GPKG" );
  if ( !driver )
  {
    *error = QStringLiteral( "GPKG driver missing" );
    return false;
  }
  GDALDatasetH ds = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
  {
    *error = QStringLiteral( "cannot create legacy gpkg" );
    return false;
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "constraints", nullptr, wkbUnknown, nullptr );
  if ( layer )
  {
    for ( const char *name : { "id", "horizon", "type" } )
    {
      OGRFieldDefnH field = OGR_Fld_Create( name, OFTString );
      OGR_L_CreateField( layer, field, TRUE );
      OGR_Fld_Destroy( field );
    }
    OGRFeatureDefnH defn = OGR_L_GetLayerDefn( layer );
    OGRFeatureH feature = OGR_F_Create( defn );
    OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "id" ), "legacy-1" );
    OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "horizon" ), "H1" );
    OGR_F_SetFieldString( feature, OGR_F_GetFieldIndex( feature, "type" ), "break_line" );
    const char *wkt = "LINESTRING(3 4, 5 6)";
    OGRGeometryH geom = nullptr;
    char *text = const_cast<char *>( wkt );
    if ( OGR_G_CreateFromWkt( &text, nullptr, &geom ) == OGRERR_NONE && geom )
      OGR_F_SetGeometry( feature, geom );
    if ( geom )
      OGR_G_DestroyGeometry( geom );
    OGR_L_CreateFeature( layer, feature );
    OGR_F_Destroy( feature );
  }
  GDALClose( ds );
  return layer != nullptr;
}
} // namespace

// P0-2：新建库自带可独立识别的 CRS 元数据；外部 OGR 也能读出同一 SRS。
void SingleFactorStoreTests::newStoreCarriesCrsMetadata()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "fresh.gpkg" ) );
  ConstraintStore store( path );
  QString err;
  QVERIFY2( store.append( QStringLiteral( "D61" ), QStringLiteral( "a" ),
                          QStringLiteral( "LINESTRING(0 0, 1 0)" ), QStringLiteral( "break_line" ), 0, &err ),
            qPrintable( err ) );
  const ConstraintStore::CrsStorageStatus status = store.inspectCrsStorage();
  QVERIFY( status.fileExists );
  QVERIFY( status.layerExists );
  QVERIFY2( status.hasSrs, "new stores must register their CRS in gpkg_spatial_ref_sys" );
  QVERIFY( status.isStoreCrs );
  QVERIFY( !status.legacyNoCrs );

  // 外部 OGR 独立打开：图层 SRS 可读且与库约定一致。
  GDALDatasetH ds = GDALOpenEx( path.toUtf8().constData(), GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr,
                                nullptr );
  QVERIFY( ds );
  OGRLayerH layer = GDALDatasetGetLayerByName( ds, "constraints" );
  QVERIFY( layer );
  QVERIFY( OGR_L_GetSpatialRef( layer ) != nullptr );
  GDALClose( ds );
}

// P0-2：无 SRS 历史库可读、状态如实（legacyNoCrs）；显式迁移补登 CRS，
// 备份生成、fid/身份/层位/几何逐字不变；再迁移幂等。
void SingleFactorStoreTests::legacyStoreDetectionAndExplicitMigration()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "legacy.gpkg" ) );
  QString err;
  QVERIFY2( createLegacyNoSrsStore( path, &err ), qPrintable( err ) );

  ConstraintStore store( path );
  // 旧库照常可读（load 不因缺 SRS 变化）。
  const QVector<QVariantMap> before = store.load();
  QCOMPARE( before.size(), 1 );
  QCOMPARE( before.first().value( QStringLiteral( "id" ) ).toString(), QStringLiteral( "legacy-1" ) );

  const ConstraintStore::CrsStorageStatus legacy = store.inspectCrsStorage();
  QVERIFY( legacy.fileExists );
  QVERIFY( legacy.layerExists );
  QVERIFY( !legacy.hasSrs );
  QVERIFY( legacy.legacyNoCrs );
  QVERIFY( !legacy.isStoreCrs );

  QString backup;
  QVERIFY2( store.migrateLegacyCrs( &err, &backup ), qPrintable( err ) );
  QVERIFY( !backup.isEmpty() );
  QVERIFY( QFile::exists( backup ) );

  const ConstraintStore::CrsStorageStatus migrated = store.inspectCrsStorage();
  QVERIFY( migrated.hasSrs );
  QVERIFY( migrated.isStoreCrs );
  QVERIFY( !migrated.legacyNoCrs );

  // 迁移后行内容逐字不变（fid/身份/层位/几何）。
  const QVector<QVariantMap> after = store.load();
  QCOMPARE( after.size(), before.size() );
  QCOMPARE( after.first().value( QStringLiteral( "fid" ) ).toLongLong(),
            before.first().value( QStringLiteral( "fid" ) ).toLongLong() );
  QCOMPARE( after.first().value( QStringLiteral( "id" ) ).toString(),
            before.first().value( QStringLiteral( "id" ) ).toString() );
  QCOMPARE( after.first().value( QStringLiteral( "horizon" ) ).toString(),
            before.first().value( QStringLiteral( "horizon" ) ).toString() );
  QCOMPARE( after.first().value( QStringLiteral( "wkt" ) ).toString(),
            before.first().value( QStringLiteral( "wkt" ) ).toString() );

  // 备份本身保持迁移前状态（无 SRS）。
  ConstraintStore backupStore( backup );
  const ConstraintStore::CrsStorageStatus backupStatus = backupStore.inspectCrsStorage();
  QVERIFY( !backupStatus.hasSrs );

  // 幂等：已是目标状态再迁移成功且不产生第二个备份（backupPathOut 保持未写）。
  const int backupsBefore = QDir( dir.path() ).entryList( { QStringLiteral( "*.crs-backup-*.bak" ) } ).size();
  QString secondBackup;
  QVERIFY2( store.migrateLegacyCrs( &err, &secondBackup ), qPrintable( err ) );
  const int backupsAfter = QDir( dir.path() ).entryList( { QStringLiteral( "*.crs-backup-*.bak" ) } ).size();
  QCOMPARE( backupsAfter, backupsBefore );
}

// P0-2：文件已登记外来 SRS 时迁移拒绝覆盖（不重新解释历史坐标）。
void SingleFactorStoreTests::migrationRefusesForeignSrs()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "foreign.gpkg" ) );
  GDALDriverH driver = GDALGetDriverByName( "GPKG" );
  QVERIFY( driver );
  GDALDatasetH ds = GDALCreate( driver, path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  QVERIFY( ds );
  OGRSpatialReferenceH srs = OSRNewSpatialReference( nullptr );
  QVERIFY( srs );
  QVERIFY( OSRImportFromEPSG( srs, 4326 ) == OGRERR_NONE );
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "constraints", srs, wkbUnknown, nullptr );
  QVERIFY( layer );
  OSRDestroySpatialReference( srs );
  GDALClose( ds );

  ConstraintStore store( path );
  const ConstraintStore::CrsStorageStatus status = store.inspectCrsStorage();
  QVERIFY( status.hasSrs );
  QVERIFY( !status.isStoreCrs );
  QString err;
  QVERIFY( !store.migrateLegacyCrs( &err, nullptr ) );
  QVERIFY( !err.isEmpty() );
  // 拒绝后文件原样（仍是外来 SRS）。
  QCOMPARE( store.inspectCrsStorage().isStoreCrs, false );
}

// V8（复审）：层位名带 '|' 会破坏 GPKG datasource URI——入口拒绝。
void SingleFactorStoreTests::horizonWithPipeIsRejected()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  ConstraintStore store( dir.filePath( QStringLiteral( "p.gpkg" ) ) );
  QString err;
  QVERIFY( !store.append( QStringLiteral( "A|layername=evil" ), QStringLiteral( "c" ),
                          QStringLiteral( "LINESTRING(0 0, 1 0)" ), QStringLiteral( "break_line" ), 0, &err ) );
  QVERIFY( !err.isEmpty() );
  QVERIFY( !QFile::exists( dir.filePath( QStringLiteral( "p.gpkg" ) ) ) );
}

QTEST_MAIN( SingleFactorStoreTests )
#include "tst_singlefactor_store.moc"
