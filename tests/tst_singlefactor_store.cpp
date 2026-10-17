// 层：数据（测试壳位于 tests/，被测对象为约束参数存储）
#include <QtTest/QtTest>

#include "io/constraintstore.h"

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_spatialref.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

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
    void migrationAbortsWhenWalCheckpointBusy();
    void restoreFileFromBackupRemovesWalResidue();
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

// full-audit F4：外部读者持有读快照时 TRUNCATE 检查点必拿 BUSY——迁移必须
// 如实拒绝且不产生字节级备份（带着缺 WAL 内容的备份继续，校验失败还原时会
// 静默丢最近已提交约束）。
void SingleFactorStoreTests::migrationAbortsWhenWalCheckpointBusy()
{
  GDALAllRegister();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString path = dir.filePath( QStringLiteral( "busy.gpkg" ) );
  QString err;
  QVERIFY2( createLegacyNoSrsStore( path, &err ), qPrintable( err ) );

  // 场景构造：GDAL 自身每次提交后自动 checkpoint（WAL 恒空），无法单独用
  // GDAL 写者制造滞留帧；用系统 sqlite3 CLI（若在）提交一帧留在 WAL，GDAL
  // 读者先锚定旧快照——TRUNCATE 检查点推进不过旧快照必拿 BUSY。环境无
  // sqlite3 CLI 时如实跳过（生产触发面是 QGIS provider 读者 + 帧滞留）。
  {
    const QString sqlite3 = QStandardPaths::findExecutable( QStringLiteral( "sqlite3" ) );
    if ( sqlite3.isEmpty() )
    {
      QSKIP( "sqlite3 CLI 不可用：无法构造 WAL 滞留帧 + 旧读快照的 busy 场景" );
    }
    GDALDatasetH modeSetter = GDALOpenEx( path.toUtf8().constData(),
                                          GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr, nullptr, nullptr );
    QVERIFY( modeSetter );
    OGRLayerH mode = GDALDatasetExecuteSQL( modeSetter, "PRAGMA journal_mode=WAL", nullptr, "SQLITE" );
    bool wal = false;
    if ( mode )
    {
      if ( OGRFeatureH raw = OGR_L_GetNextFeature( mode ) )
      {
        wal = QString::fromUtf8( OGR_F_GetFieldAsString( raw, 0 ) ).compare(
                   QStringLiteral( "wal" ), Qt::CaseInsensitive ) == 0;
        OGR_F_Destroy( raw );
      }
      GDALDatasetReleaseResultSet( modeSetter, mode );
    }
    GDALClose( modeSetter );
    QVERIFY2( wal, "journal_mode must switch to WAL for the busy scenario" );

    GDALDatasetH reader = GDALOpenEx( path.toUtf8().constData(),
                                      GDAL_OF_VECTOR, nullptr, nullptr, nullptr );
    QVERIFY( reader );
    OGRLayerH layer = GDALDatasetGetLayerByName( reader, "constraints" );
    QVERIFY( layer );
    OGR_L_ResetReading( layer );
    if ( OGRFeatureH raw = OGR_L_GetNextFeature( layer ) )
      OGR_F_Destroy( raw ); // 语句保持打开：读快照锚定在当前 WAL 尾
    // 持久 CLI 连接提交一帧后保持打开（连接全关时 sqlite 会自动 checkpoint
    // 清空 WAL——帧就留不住）。constraints 表的几何触发器用 CLI 没有的
    // ST_* 函数，写无触发器的 gpkg_contents 即可制造滞留帧。
    QProcess cli;
    cli.start( sqlite3, { path } );
    QVERIFY2( cli.waitForStarted( 10000 ), "sqlite3 start failed" );
    cli.write( QStringLiteral( "UPDATE gpkg_contents SET last_change = '2001-01-01T00:00:00.000Z';\n" ).toUtf8() );
    cli.waitForBytesWritten( 10000 );
    QThread::msleep( 300 );
    QVERIFY2( QFileInfo::exists( path + QStringLiteral( "-wal" ) ) &&
                  QFileInfo( path + QStringLiteral( "-wal" ) ).size() > 0,
              "CLI 提交应留下滞留 WAL 帧" );

    ConstraintStore store( path );
    QString migrateError;
    QVERIFY( !store.migrateLegacyCrs( &migrateError, nullptr ) );
    QVERIFY2( migrateError.contains( QStringLiteral( "检查点" ) ) ||
                  migrateError.contains( QStringLiteral( "占用" ) ),
              qPrintable( migrateError ) );
    // 拒绝路径不得留下备份（也不动主文件）。
    QCOMPARE( QDir( dir.path() ).entryList( { QStringLiteral( "*.crs-backup-*.bak" ) } ).size(), 0 );
    cli.write( QStringLiteral( ".quit\n" ).toUtf8() );
    cli.waitForFinished( 10000 );
    cli.close();
    GDALClose( reader ); // 释放读快照
  }
}

// full-audit F4：字节级还原必须把 -wal/-shm 与主文件一起换掉——遗留 WAL 帧
// 会在下次打开时重放到还原后的旧主文件上（嵌合库）。
void SingleFactorStoreTests::restoreFileFromBackupRemovesWalResidue()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString main = dir.filePath( QStringLiteral( "m.gpkg" ) );
  const QString backup = dir.filePath( QStringLiteral( "m.bak" ) );
  {
    QFile a( main );
    QVERIFY( a.open( QIODevice::WriteOnly ) );
    a.write( "AAAA" );
    QFile b( backup );
    QVERIFY( b.open( QIODevice::WriteOnly ) );
    b.write( "BBBB" );
    QFile wal( main + QStringLiteral( "-wal" ) );
    QVERIFY( wal.open( QIODevice::WriteOnly ) );
    wal.write( "stale-frames" );
    QFile shm( main + QStringLiteral( "-shm" ) );
    QVERIFY( shm.open( QIODevice::WriteOnly ) );
    shm.write( "stale-index" );
  }
  QString err;
  QVERIFY2( ConstraintStore::restoreFileFromBackup( main, backup, &err ),
            qPrintable( err ) );
  QVERIFY( !QFile::exists( main + QStringLiteral( "-wal" ) ) );
  QVERIFY( !QFile::exists( main + QStringLiteral( "-shm" ) ) );
  QFile restored( main );
  QVERIFY( restored.open( QIODevice::ReadOnly ) );
  QCOMPARE( restored.readAll(), QByteArray( "BBBB" ) );

  // 备份缺失 → 如实失败，不动现场。
  QVERIFY( !ConstraintStore::restoreFileFromBackup( main, dir.filePath( QStringLiteral( "nope.bak" ) ), &err ) );
  QVERIFY( !err.isEmpty() );
  QFile still( main );
  QVERIFY( still.open( QIODevice::ReadOnly ) );
  QCOMPARE( still.readAll(), QByteArray( "BBBB" ) );
}

QTEST_MAIN( SingleFactorStoreTests )
#include "tst_singlefactor_store.moc"
