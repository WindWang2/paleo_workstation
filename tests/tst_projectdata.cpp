#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gdal.h>
#include <cpl_conv.h>

#include "../src/catalog/datacatalog.h"
#include "../src/services/projectdata.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgisruntime.h"

// wave/mapping-pipeline — 读侧门面 ProjectDataFacade 契约测试。
// fixture 用真 DataCatalog + 真 SMI 井文本（多井一份 tops、每井一份 TD），
// 与 wave/data-foundation 的受管路径布局一致；合并后接口不变。

class TestProjectData : public QObject
{
    Q_OBJECT

  private:
    static bool writeText( const QString &path, const QString &text )
    {
        QDir().mkpath( QFileInfo( path ).absolutePath() );
        QFile f( path );
        if ( !f.open( QIODevice::WriteOnly ) )
            return false;
        f.write( text.toUtf8() );
        f.close();
        return QFile::exists( path ) && QFileInfo( path ).size() > 0;
    }

    // Tiny 10x8 Float32 GTiff over local meters with the DERIVED-horizon
    // inline range recorded as dataset metadata.
    static QString makeRaster( const QString &path )
    {
        GDALAllRegister();
        GDALDriverH drv = GDALGetDriverByName( "GTiff" );
        GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), 10, 8, 1, GDT_Float32, nullptr );
        if ( !ds )
            return QString();
        const double gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
        GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
        GDALSetMetadataItem( ds, "PALEO_INLINE_MIN", "1315", nullptr );
        GDALSetMetadataItem( ds, "PALEO_INLINE_MAX", "1725", nullptr );
        QVector<float> px( 10 * 8, 2100.0f );
        GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
        GDALSetRasterNoDataValue( band, -9999.0 );
        const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, 10, 8,
                                         const_cast<float *>( px.constData() ), 10, 8,
                                         GDT_Float32, 0, 0 );
        GDALClose( ds );
        return err == CE_None ? path : QString();
    }

    // 真 SMI 井文本：一份多井 tops + 每井一份 TD。A2 缺 D62 分层，
    // A3 无时深关联。受管相对路径与 DataCatalog::managedPath 布局一致。
    static bool buildCatalog( DataCatalog &catalog, const QDir &dir )
    {
        auto addWell = [&catalog]( const QString &id, const QString &name, double x, double y ) {
            CatalogEntity e;
            e.id = id;
            e.entityType = QStringLiteral( "well" );
            e.name = name;
            e.surfaceX = x;
            e.surfaceY = y;
            e.hasSurface = true;
            e.coordinateStatus = QStringLiteral( "untransformed" );
            return catalog.addEntity( e );
        };
        if ( !addWell( QStringLiteral( "well-1" ), QStringLiteral( "A1" ), 5288.67, 8219.94 ) ||
             !addWell( QStringLiteral( "well-2" ), QStringLiteral( "A2" ), 6000.0, 9000.0 ) ||
             !addWell( QStringLiteral( "well-3" ), QStringLiteral( "A3" ), 7000.0, 7500.0 ) )
            return false;

        // tops：一份多井文件 = 一个资产 + 每井一条关联。
        CatalogAsset topsAsset;
        topsAsset.id = QStringLiteral( "ast-1" );
        topsAsset.type = QStringLiteral( "well_stratification" );
        topsAsset.format = QStringLiteral( "dat" );
        topsAsset.displayName = QStringLiteral( "DC.dat" );
        if ( !catalog.addAsset( topsAsset ) )
            return false;
        CatalogVersion topsVer;
        topsVer.id = QStringLiteral( "ver-1" );
        topsVer.assetId = topsAsset.id;
        topsVer.stage = QStringLiteral( "RAW" );
        topsVer.path = DataCatalog::managedPath( QStringLiteral( "raw" ), topsAsset.id,
                                                 topsVer.id, QStringLiteral( "DC.dat" ) );
        if ( !catalog.addVersion( topsVer ) )
            return false;
        const QString topsText = QStringLiteral(
            "#WellTops File From SMI\n"
            "#WellName    Name         MD           X            Y            Z            TVD          Time(ms)\n"
            "A1           C6           1900.000     5288.670     8219.940     -1885.000    1885.000     -99999.000\n"
            "A1           D61          2148.000     5288.670     8219.940     -2125.000    2125.000     -99999.000\n"
            "A1           D62          2200.000     5288.670     8219.940     -2180.000    2180.000     -99999.000\n"
            "A2           D61          2050.000     6000.000     9000.000     -2030.000    2030.000     -99999.000\n"
            "A3           D61          2400.000     7000.000     7500.000     -2380.000    2380.000     -99999.000\n"
            "A3           D62          2450.000     7000.000     7500.000     -2430.000    2430.000     -99999.000\n" );
        if ( !writeText( dir.filePath( topsVer.path ), topsText ) )
            return false;

        auto link = [&catalog]( const QString &entityId, const QString &assetId, const QString &role ) {
            EntityAssetLink l;
            l.entityType = QStringLiteral( "well" );
            l.entityId = entityId;
            l.assetId = assetId;
            l.role = role;
            l.isPrimary = true;
            l.unresolved = false;
            return catalog.addLink( l );
        };
        if ( !link( QStringLiteral( "well-1" ), topsAsset.id, QStringLiteral( "tops" ) ) ||
             !link( QStringLiteral( "well-2" ), topsAsset.id, QStringLiteral( "tops" ) ) ||
             !link( QStringLiteral( "well-3" ), topsAsset.id, QStringLiteral( "tops" ) ) )
            return false;

        // TD：A1 一份（A2/A3 无时深在本测试里）。
        CatalogAsset tdAsset;
        tdAsset.id = QStringLiteral( "ast-2" );
        tdAsset.type = QStringLiteral( "time_depth" );
        tdAsset.format = QStringLiteral( "dat" );
        tdAsset.displayName = QStringLiteral( "A1_TD.dat" );
        if ( !catalog.addAsset( tdAsset ) )
            return false;
        CatalogVersion tdVer;
        tdVer.id = QStringLiteral( "ver-2" );
        tdVer.assetId = tdAsset.id;
        tdVer.stage = QStringLiteral( "RAW" );
        tdVer.path = DataCatalog::managedPath( QStringLiteral( "raw" ), tdAsset.id,
                                               tdVer.id, QStringLiteral( "A1_TD.dat" ) );
        if ( !catalog.addVersion( tdVer ) )
            return false;
        // 行故意倒序、末尾一行 -99999：门面按文件顺序交付、不排序
        // （PROJECT_AREA_PLAN §3），哨兵以 NaN 透传，留给 TimeDepthTool 判。
        const QString tdText = QStringLiteral(
            "#TimeDepth File From SMI\n"
            "# Well : A1\n"
            "#TIME            TVDSS            TVD            MD\n"
            "2400.000          -2400.000         2400.000         2400.000\n"
            "2200.000          -2100.000         2100.000         2100.000\n"
            "2000.000          -1800.000         1800.000         1800.000\n"
            "2500.000          -99999.000        -99999.000       -99999.000\n" );
        if ( !writeText( dir.filePath( tdVer.path ), tdText ) )
            return false;
        return link( QStringLiteral( "well-1" ), tdAsset.id, QStringLiteral( "time_depth" ) );
    }

  private slots:

    void wellsTopsTdRead()
    {
        QTemporaryDir dir;
        DataCatalog catalog;
        QVERIFY( catalog.open( dir.path() ) );
        QVERIFY( buildCatalog( catalog, QDir( dir.path() ) ) );

        ProjectDataFacade pd;
        pd.setCatalog( &catalog, dir.path() );

        const QVector<ProjectWell> wells = pd.wells();
        QCOMPARE( wells.size(), 3 );
        QCOMPARE( wells.at( 0 ).id, QStringLiteral( "well-1" ) );
        QCOMPARE( wells.at( 0 ).name, QStringLiteral( "A1" ) );
        QVERIFY( qAbs( wells.at( 0 ).surfaceX - 5288.67 ) < 0.01 );
        QVERIFY( qAbs( wells.at( 0 ).surfaceY - 8219.94 ) < 0.01 );
        QCOMPARE( wells.at( 0 ).coordinateStatus, QStringLiteral( "untransformed" ) );

        const QVector<WellTop> tops = pd.topsFor( QStringLiteral( "well-1" ) );
        QCOMPARE( tops.size(), 3 );
        QCOMPARE( tops.at( 1 ).horizon, QStringLiteral( "D61" ) );
        QVERIFY( qAbs( tops.at( 1 ).tvd - 2125.0 ) < 0.01 );
        QVERIFY( qAbs( tops.at( 1 ).md - 2148.0 ) < 0.01 );

        const QVector<TdSample> td = pd.tdTableFor( QStringLiteral( "well-1" ) );
        QCOMPARE( td.size(), 4 );
        // 文件顺序交付：按 TIME 排序会排成 2000/2200/2400/2500，这里第一条
        // 必须是文件首行 2400。TVD 与 MD 两列都带上（MD 兜底要用）。
        QVERIFY( qAbs( td.at( 0 ).timeMs - 2400.0 ) < 0.01 );
        QVERIFY( qAbs( td.at( 0 ).tvd - 2400.0 ) < 0.01 );
        QVERIFY( qAbs( td.at( 0 ).md - 2400.0 ) < 0.01 );
        QVERIFY( qAbs( td.at( 1 ).timeMs - 2200.0 ) < 0.01 );
        QVERIFY( qAbs( td.at( 2 ).timeMs - 2000.0 ) < 0.01 );
        // -99999 行：时间保留，深度以 NaN 透传（插值端剔除，不进排序检查）。
        QVERIFY( qAbs( td.at( 3 ).timeMs - 2500.0 ) < 0.01 );
        QVERIFY( qIsNaN( td.at( 3 ).tvd ) );
        QVERIFY( qIsNaN( td.at( 3 ).md ) );

        // No time_depth link → empty, not fabricated.
        QCOMPARE( pd.tdTableFor( QStringLiteral( "well-3" ) ).size(), 0 );
        // Unknown well → empty tops.
        QCOMPARE( pd.topsFor( QStringLiteral( "well-XX" ) ).size(), 0 );

        // setProjectDir round-trips the same data from disk.
        ProjectDataFacade fromDisk;
        QVERIFY2( fromDisk.setProjectDir( dir.path() ), qPrintable( fromDisk.lastError() ) );
        QCOMPARE( fromDisk.wells().size(), 3 );
        QCOMPARE( fromDisk.topsFor( QStringLiteral( "well-2" ) ).size(), 1 );
    }

    void horizonRasterFromManifest()
    {
        QTemporaryDir dir;
        DataCatalog catalog;
        QVERIFY( catalog.open( dir.path() ) );
        QVERIFY( buildCatalog( catalog, QDir( dir.path() ) ) );

        const QString tif = makeRaster( dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );

        LayerManifest manifest( dir.filePath( QStringLiteral( "project.sqlite" ) ) );
        QVERIFY( manifest.open() );
        LayerDeclaration d;
        d.layerId = QStringLiteral( "horizon.D61.derived" );
        d.horizon = QStringLiteral( "D61" );
        d.type = QStringLiteral( "raster" );
        d.source = tif;
        d.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( manifest.upsert( d, &err ), qPrintable( err ) );

        ProjectDataFacade pd;
        pd.setCatalog( &catalog, dir.path() );
        pd.setManifest( &manifest );

        const HorizonRasterInfo info = pd.horizonRasterDecl( QStringLiteral( "D61" ) );
        QVERIFY( info.valid );
        QCOMPARE( info.layerId, QStringLiteral( "horizon.D61.derived" ) );
        QCOMPARE( info.cols, 10 );
        QCOMPARE( info.rows, 8 );
        QVERIFY( qAbs( info.cellSize - 100.0 ) < 0.01 );
        QVERIFY( qAbs( info.xmin ) < 0.01 );
        QVERIFY( qAbs( info.ymax - 800.0 ) < 0.01 );
        QCOMPARE( info.inlineMin, 1315 );
        QCOMPARE( info.inlineMax, 1725 );

        // Unknown horizon / no manifest → invalid, no throw.
        QVERIFY( !pd.horizonRasterDecl( QStringLiteral( "D99" ) ).valid );
        ProjectDataFacade bare;
        bare.setCatalog( &catalog, dir.path() );
        QVERIFY( !bare.horizonRasterDecl( QStringLiteral( "D61" ) ).valid );
    }

    void emptyAndBrokenProjectDir()
    {
        QTemporaryDir dir; // empty dir — DataCatalog initializes a fresh catalog
        ProjectDataFacade pd;
        QVERIFY2( pd.setProjectDir( dir.path() ), qPrintable( pd.lastError() ) );
        QCOMPARE( pd.wells().size(), 0 ); // no entities yet — empty, not an error

        // A file where a directory belongs → open fails with an error.
        const QString filePath = dir.filePath( QStringLiteral( "not_a_dir" ) );
        QFile f( filePath );
        QVERIFY( f.open( QIODevice::WriteOnly ) );
        f.close();
        ProjectDataFacade broken;
        QVERIFY( !broken.setProjectDir( filePath ) );
        QVERIFY( !broken.lastError().isEmpty() );
        QCOMPARE( broken.wells().size(), 0 );
    }
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
        qFatal( "QgisRuntime::initialize failed" );
    TestProjectData tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgisRuntime::shutdown();
    return rc;
}

#include "tst_projectdata.moc"
