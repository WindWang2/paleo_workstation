#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include <tuple>
#include <QTemporaryDir>

#include <gdal.h>
#include <cpl_conv.h>

#include <qgsproject.h>
#include <qgslayout.h>
#include <qgsprintlayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/types.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/mapversionstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/projectdata.h"
#include "../src/workflow/mappingworkflow.h"
#include "../src/workflow/mapexport.h"
#include "../src/workflow/mapversioncontroller.h"
#include "../src/workflow/workflows.h"

// wave/mapping-pipeline 阶段C — 只编 D61（autoplan §5C 新口径）：
//   层位栅格 D62−D61 等时差 × 井点层间速度 IDW²(Vint) → 「D61–D62 等厚（米）」，
//   写在 D61 既有网格上（凸包外 −9999）；本链不产相多边形。
//   验证侧：D61 逐井时间残差（10ms 阈值，每口井一行）。
// 全链走真实服务栈（同 tst_workflows 的 Fixture 形态），fixture 是合成
// catalog.json + 小时间栅格。

class TestMapping : public QObject
{
    Q_OBJECT

  private:
    struct Fixture
    {
        QTemporaryDir dir;
        DataCatalog catalog;   // 真 catalog（受管路径 + SMI 井文本）
        QgisProjectService projectSvc;
        PaleoProjectStore store;
        LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
        QgisLayerService layers{ &projectSvc, &manifest };
        QgisProcessingService proc{ &store };
        ProjectDataFacade pd;
        ConstraintWorkflow constraint{ &proc, &layers };
        CompositionWorkflow compose{ &proc, &layers };
        MappingWorkflow mapping{ &constraint, &compose, &layers };

        bool init()
        {
            if ( !dir.isValid() )
                return false;
            if ( !catalog.open( dir.path() ) )
                return false;
            if ( !buildCatalog( catalog, QDir( dir.path() ) ) )
                return false;
            if ( !projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
                return false;
            if ( !manifest.open() )
                return false;
            store.setProjectPaths( dir.filePath( QStringLiteral( "proj.qgz" ) ),
                                   dir.filePath( QStringLiteral( "proj.gpkg" ) ),
                                   dir.filePath( QStringLiteral( "meta.sqlite" ) ) );
            // T26：厚度链的派生产物登记走 fixture 自己的 catalog（工程目录 = 临时目录）。
            mapping.setCatalog( &catalog, dir.path() );
            return true;
        }
    };

    // 7 wells inside a 0..1000 x 0..800 local-meter grid:
    //   A1 D61/D62/D63 + TD（D61→D62 层间速度 3000 m/s；D61 残差 ~+215ms 超限）
    //   A2 只有 D61（厚度跳过）；TD 时间与栅格吻合（残差 ~0）
    //   A3 D61/D62 无 TD 链接（样本「无时深表」，残差行同因）
    //   A5 D61/D62 + TD（vint 2000 m/s；残差 ~−0.8ms 通过）
    //   A6 D61/D62 + TD（vint 1500 m/s；残差 ~−100ms 超限）
    //   A7 无任何数据链接（残差行「无 D61 分层」）
    // A1 额外带 D63 → D62→D63 只有 1 口样本井（「不足以成面」用例）。
    static bool writeText( const QString &path, const QString &text )
    {
        QDir().mkpath( QFileInfo( path ).absolutePath() );
        QFile f( path );
        if ( !f.open( QIODevice::WriteOnly ) )
            return false;
        f.write( text.toUtf8() );
        f.close();
        return QFile::exists( path );
    }

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
        if ( !addWell( QStringLiteral( "well-1" ), QStringLiteral( "A1" ), 300.0, 400.0 ) ||
             !addWell( QStringLiteral( "well-2" ), QStringLiteral( "A2" ), 500.0, 300.0 ) ||
             !addWell( QStringLiteral( "well-3" ), QStringLiteral( "A3" ), 700.0, 600.0 ) ||
             !addWell( QStringLiteral( "well-5" ), QStringLiteral( "A5" ), 650.0, 700.0 ) ||
             !addWell( QStringLiteral( "well-6" ), QStringLiteral( "A6" ), 350.0, 700.0 ) ||
             !addWell( QStringLiteral( "well-7" ), QStringLiteral( "A7" ), 150.0, 150.0 ) )
            return false;

        // 一份多井 tops = 一个资产 + 每井一条关联（数据底座纪律）。
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
            "A1           D61          2148.000     300.000      400.000      -2125.000    2125.000     -99999.000\n"
            "A1           D62          2200.000     300.000      400.000      -2180.000    2180.000     -99999.000\n"
            "A1           D63          2230.000     300.000      400.000      -2210.000    2210.000     -99999.000\n"
            "A2           D61          2050.000     500.000      300.000      -2030.000    2030.000     -99999.000\n"
            "A3           D61          2400.000     700.000      600.000      -2380.000    2380.000     -99999.000\n"
            "A3           D62          2450.000     700.000      600.000      -2430.000    2430.000     -99999.000\n"
            "A5           D61          2100.000     650.000      700.000      -2000.000    2000.000     -99999.000\n"
            "A5           D62          2160.000     650.000      700.000      -2100.000    2100.000     -99999.000\n"
            "A6           D61          1950.000     350.000      700.000      -1900.000    1900.000     -99999.000\n"
            "A6           D62          2010.000     350.000      700.000      -1960.000    1960.000     -99999.000\n" );
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
        for ( const QString &id : { QStringLiteral( "well-1" ), QStringLiteral( "well-2" ),
                                    QStringLiteral( "well-3" ), QStringLiteral( "well-5" ),
                                    QStringLiteral( "well-6" ) } )
            if ( !link( id, topsAsset.id, QStringLiteral( "tops" ) ) )
                return false;

        auto addTd = [&dir, &catalog, &link]( const QString &wellId, const QString &assetId,
                                              const QString &verId, const QString &fileName,
                                              const QString &rows ) {
            CatalogAsset a;
            a.id = assetId;
            a.type = QStringLiteral( "time_depth" );
            a.format = QStringLiteral( "dat" );
            a.displayName = fileName;
            if ( !catalog.addAsset( a ) )
                return false;
            CatalogVersion v;
            v.id = verId;
            v.assetId = a.id;
            v.stage = QStringLiteral( "RAW" );
            v.path = DataCatalog::managedPath( QStringLiteral( "raw" ), a.id, v.id, fileName );
            if ( !catalog.addVersion( v ) )
                return false;
            const QString text = QStringLiteral(
                "#TimeDepth File From SMI\n# Well : %1\n"
                "#TIME            TVDSS            TVD            MD\n%2" )
                .arg( fileName.left( 2 ), rows );
            return writeText( dir.filePath( v.path ), text ) &&
                   link( wellId, a.id, QStringLiteral( "time_depth" ) );
        };
        return addTd( QStringLiteral( "well-1" ), QStringLiteral( "ast-2" ),
                      QStringLiteral( "ver-2" ), QStringLiteral( "A1_TD.dat" ),
                      QStringLiteral(
                          "2000.000          -1800.000         1800.000         1800.000\n"
                          "2200.000          -2100.000         2100.000         2100.000\n"
                          "2400.000          -2400.000         2400.000         2400.000\n" ) ) &&
               addTd( QStringLiteral( "well-2" ), QStringLiteral( "ast-3" ),
                      QStringLiteral( "ver-3" ), QStringLiteral( "A2_TD.dat" ),
                      QStringLiteral(
                          "2000.000          -2028.500         2028.500         2028.500\n"
                          "2200.000          -2228.500         2228.500         2228.500\n" ) ) &&
               // A5：t(D61=2000)=2000ms、t(D62=2100)=2100ms → dt=100 → vint=2000。
               addTd( QStringLiteral( "well-5" ), QStringLiteral( "ast-10" ),
                      QStringLiteral( "ver-10" ), QStringLiteral( "A5_TD.dat" ),
                      QStringLiteral(
                          "2000.000          -2000.000         2000.000         2000.000\n"
                          "2400.000          -2400.000         2400.000         2400.000\n" ) ) &&
               // A6：t(1900)=1900、t(1960)=1980 → dt=80 → vint=60/0.04=1500。
               addTd( QStringLiteral( "well-6" ), QStringLiteral( "ast-11" ),
                      QStringLiteral( "ver-11" ), QStringLiteral( "A6_TD.dat" ),
                      QStringLiteral(
                          "1900.000          -1900.000         1900.000         1900.000\n"
                          "2100.000          -2050.000         2050.000         2050.000\n" ) );
    }

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
        QVector<float> px;
        for ( int y = 0; y < 8; ++y )
            for ( int x = 0; x < 10; ++x )
                // smooth time surface ~2000ms + dip, so sampling is never nodata
                px.append( 2000.0f + 0.1f * x + 0.2f * y );
        GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
        GDALSetRasterNoDataValue( band, -9999.0 );
        const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, 10, 8,
                                         const_cast<float *>( px.constData() ), 10, 8,
                                         GDT_Float32, 0, 0 );
        GDALClose( ds );
        return err == CE_None ? path : QString();
    }

    // 常值 Float32 GTiff，同 makeRaster 的网格/元数据（10×8 @100m，
    // 0–1000 × 0–800）。dims 可覆盖（栅格不匹配用例）。
    static QString makeConstRaster( const QString &path, float value, int w = 10, int h = 8 )
    {
        GDALAllRegister();
        GDALDriverH drv = GDALGetDriverByName( "GTiff" );
        GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr );
        if ( !ds )
            return QString();
        const double gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
        GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
        QVector<float> px( w * h, value );
        GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
        GDALSetRasterNoDataValue( band, -9999.0 );
        const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, w, h,
                                         const_cast<float *>( px.constData() ), w, h,
                                         GDT_Float32, 0, 0 );
        GDALClose( ds );
        return err == CE_None ? path : QString();
    }

    // Constraint polygon covering the LEFT half of the wells extent; IDW
    // output is clipped to its convex hull, so right-half cells go nodata.
    static bool writeConstraintPolygon( const QString &path )
    {
        return writeText( path, "{\"type\":\"FeatureCollection\",\"features\":["
          "{\"type\":\"Feature\",\"properties\":{\"facies_code\":1},"
          "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":"
          "[[[100,100],[500,100],[500,700],[100,700],[100,100]]]}}]}" );
    }

    static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
    {
        const QVector<LayerDeclaration> decls = layers.declared();
        for ( const LayerDeclaration &d : decls )
            if ( d.layerId == layerId )
                return new LayerDeclaration( d );
        return nullptr;
    }

    // Samples the Float32 cell containing world (x,y) — NaN on error/nodata.
    static double sampleAt( const QString &path, double x, double y )
    {
        GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
        if ( !ds )
            return qQNaN();
        double gt[6] = { 0, 0, 0, 0, 0, 0 };
        GDALGetGeoTransform( ds, gt );
        const int col = static_cast<int>( std::floor( ( x - gt[0] ) / gt[1] ) );
        const int row = static_cast<int>( std::floor( ( gt[3] - y ) / -gt[5] ) );
        GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
        if ( col < 0 || row < 0 || col >= GDALGetRasterXSize( ds ) || row >= GDALGetRasterYSize( ds ) )
        {
            GDALClose( ds );
            return qQNaN();
        }
        float v = 0.0f;
        const CPLErr err = GDALRasterIO( band, GF_Read, col, row, 1, 1, &v, 1, 1, GDT_Float32, 0, 0 );
        int hasNodata = 0;
        const double nodata = GDALGetRasterNoDataValue( band, &hasNodata );
        GDALClose( ds );
        if ( err != CE_None )
            return qQNaN();
        if ( hasNodata && qAbs( static_cast<double>( v ) - nodata ) < 1e-6 )
            return qQNaN();
        return v;
    }

  private slots:

    void initTestCase()
    {
        // 阶段E 的 8 界面有序集合 + 厚度基面推导。
        QCOMPARE( mappingHorizons(),
                  QStringList( { QStringLiteral( "C3" ), QStringLiteral( "C6" ),
                                 QStringLiteral( "D53" ), QStringLiteral( "D61" ),
                                 QStringLiteral( "D62" ), QStringLiteral( "D63" ),
                                 QStringLiteral( "D71" ), QStringLiteral( "D72" ) } ) );
        QVERIFY( isMappingHorizon( QStringLiteral( "D61" ) ) );
        QVERIFY( !isMappingHorizon( QStringLiteral( "D61x" ) ) );
        QCOMPARE( baseHorizonFor( QStringLiteral( "D61" ) ), QStringLiteral( "D62" ) );
        QVERIFY( baseHorizonFor( QStringLiteral( "D72" ) ).isEmpty() );
    }

    void cleanup()
    {
        QgsProject::instance()->removeAllMapLayers();
    }

    // tops → 厚度：A1=55、A3=50、A5=100、A6=60 m；A2 缺 D62、A7 无分层跳过计数。
    void thicknessFromTopsSkipsMissingBase()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.mapping.setProjectData( &f.pd );

        int skipped = -1;
        QString err;
        const QVector<ThicknessPoint> pts = f.mapping.computeThickness(
            QStringLiteral( "D61" ), QStringLiteral( "D62" ), &skipped, &err );
        QVERIFY2( err.isEmpty(), qPrintable( err ) );
        QCOMPARE( pts.size(), 4 );
        QCOMPARE( skipped, 2 );

        QCOMPARE( pts.at( 0 ).wellId, QStringLiteral( "well-1" ) );
        QCOMPARE( pts.at( 0 ).wellName, QStringLiteral( "A1" ) );
        QVERIFY( qAbs( pts.at( 0 ).x - 300.0 ) < 0.01 );
        QVERIFY( qAbs( pts.at( 0 ).y - 400.0 ) < 0.01 );
        QVERIFY( qAbs( pts.at( 0 ).thickness - 55.0 ) < 0.01 );

        // Facade 未绑定 → 有错误文案，返回空。
        MappingWorkflow orphan( &f.constraint, &f.compose, &f.layers );
        const QVector<ThicknessPoint> none = orphan.computeThickness(
            QStringLiteral( "D61" ), QStringLiteral( "D62" ), &skipped, &err );
        QCOMPARE( none.size(), 0 );
        QVERIFY( !err.isEmpty() );
    }

    // 逐井样本（autoplan §5C）：贡献井给出 Vint，非贡献井给出原因。
    // A1→3000、A5→2000、A6→1500 m/s；A2 无 D62；A3 无时深表；A7 无 D61。
    void thicknessSamplesCarryVintOrReason()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.mapping.setProjectData( &f.pd );

        QString err;
        const QVector<ThicknessSample> samples = f.mapping.computeThicknessSamples(
            QStringLiteral( "D61" ), QStringLiteral( "D62" ), &err );
        QVERIFY2( err.isEmpty(), qPrintable( err ) );
        QCOMPARE( samples.size(), 6 );

        const ThicknessSample &a1 = samples.at( 0 );
        QCOMPARE( a1.wellName, QStringLiteral( "A1" ) );
        QVERIFY( a1.contributing );
        QVERIFY( qAbs( a1.tvdTop - 2125.0 ) < 0.01 );
        QVERIFY( qAbs( a1.tvdBase - 2180.0 ) < 0.01 );
        // dt = 2253.33 − 2216.67 = 36.67ms → 55/(36.67/2000) = 3000 m/s
        QVERIFY( qAbs( a1.dtMs - 36.6667 ) < 0.01 );
        QVERIFY( qAbs( a1.vint - 3000.0 ) < 1.0 );

        for ( const ThicknessSample &s : samples )
        {
            if ( s.wellName == QLatin1String( "A5" ) )
            {
                QVERIFY( s.contributing );
                QVERIFY( qAbs( s.vint - 2000.0 ) < 1.0 );
            }
            else if ( s.wellName == QLatin1String( "A6" ) )
            {
                QVERIFY( s.contributing );
                QVERIFY( qAbs( s.vint - 1500.0 ) < 1.0 );
            }
            else if ( s.wellName == QLatin1String( "A2" ) )
            {
                QVERIFY( !s.contributing );
                QVERIFY( s.reason.contains( QStringLiteral( "D62" ) ) );
            }
            else if ( s.wellName == QLatin1String( "A3" ) )
            {
                QVERIFY( !s.contributing );
                QCOMPARE( s.reason, QStringLiteral( "无时深表" ) );
            }
            else if ( s.wellName == QLatin1String( "A7" ) )
            {
                QVERIFY( !s.contributing );
                QVERIFY( s.reason.contains( QStringLiteral( "分层" ) ) );
            }
        }
    }

    // 期望米数（autoplan §5C 公式）：等时差/2000 × 像元处 IDW²(Vint)。
    // 贡献井：A1(300,400)vint=3000、A5(650,700)vint=2000、A6(350,700)vint=1500。
    static double expectedIsochronThickness( double isoMs, double x, double y )
    {
        const double xs[3] = { 300.0, 650.0, 350.0 };
        const double ys[3] = { 400.0, 700.0, 700.0 };
        const double vs[3] = { 3000.0, 2000.0, 1500.0 };
        double wsum = 0.0, vsum = 0.0;
        for ( int i = 0; i < 3; ++i )
        {
            const double dx = xs[i] - x, dy = ys[i] - y;
            const double d2 = dx * dx + dy * dy;
            if ( d2 == 0.0 )
                return isoMs / 2000.0 * vs[i];
            const double w = 1.0 / d2;
            wsum += w;
            vsum += w * vs[i];
        }
        return isoMs / 2000.0 * ( vsum / wsum );
    }

    // 造井实体 + D61 分层（分层 X/Y 可与井口不同）+ 恒等斜率 TD
    // （TIME=TVD，1ms/1m）——发布门/验证口径对比用。
    static bool addWellWithOffsetTop( Fixture &f, int seq, const QString &name,
                                      double sx, double sy, double topX,
                                      double topY, double tvd )
    {
        const QString id = QStringLiteral( "well-x%1" ).arg( seq );
        CatalogEntity e;
        e.id = id;
        e.entityType = QStringLiteral( "well" );
        e.name = name;
        e.surfaceX = sx;
        e.surfaceY = sy;
        e.hasSurface = true;
        e.coordinateStatus = QStringLiteral( "untransformed" );
        if ( !f.catalog.addEntity( e ) )
            return false;
        const QString astT = QStringLiteral( "ast-x%1t" ).arg( seq );
        const QString astD = QStringLiteral( "ast-x%1d" ).arg( seq );
        const QString verT = QStringLiteral( "ver-x%1t" ).arg( seq );
        const QString verD = QStringLiteral( "ver-x%1d" ).arg( seq );
        auto add = [&]( const QString &assetId, const QString &verId,
                        const QString &type, const QString &file,
                        const QString &text, const QString &role ) {
            CatalogAsset a;
            a.id = assetId;
            a.type = type;
            a.format = QStringLiteral( "dat" );
            a.displayName = file;
            if ( !f.catalog.addAsset( a ) )
                return false;
            CatalogVersion v;
            v.id = verId;
            v.assetId = a.id;
            v.stage = QStringLiteral( "RAW" );
            v.path = DataCatalog::managedPath( QStringLiteral( "raw" ), a.id,
                                               v.id, file );
            if ( !f.catalog.addVersion( v ) )
                return false;
            EntityAssetLink l;
            l.entityType = QStringLiteral( "well" );
            l.entityId = id;
            l.assetId = a.id;
            l.role = role;
            l.isPrimary = true;
            return f.catalog.addLink( l ) &&
                   writeText( f.dir.filePath( v.path ), text );
        };
        // tops 列：WellName Name MD X Y Z TVD Time —— X/Y 写分层点（≠井口）。
        const QString tops = QStringLiteral(
            "#WellTops File From SMI\n%1 D61 %2 %3 %4 %5 %5 -99999.000\n" )
            .arg( name )
            .arg( tvd, 0, 'f', 3 )
            .arg( topX, 0, 'f', 3 )
            .arg( topY, 0, 'f', 3 )
            .arg( tvd, 0, 'f', 3 );
        const QString td = QStringLiteral(
            "#TimeDepth File From SMI\n# Well : %1\n"
            "%2 -%3 %3 %3\n%4 -%5 %5 %5\n" )
            .arg( name )
            .arg( tvd, 0, 'f', 3 )
            .arg( tvd, 0, 'f', 3 )
            .arg( tvd + 100.0, 0, 'f', 3 )
            .arg( tvd + 100.0, 0, 'f', 3 );
        return add( astT, verT, QStringLiteral( "well_stratification" ),
                    QStringLiteral( "DCX%1.dat" ).arg( seq ), tops,
                    QStringLiteral( "tops" ) ) &&
               add( astD, verD, QStringLiteral( "time_depth" ),
                    QStringLiteral( "AX%1_TD.dat" ).arg( seq ), td,
                    QStringLiteral( "time_depth" ) );
    }

    // 阶段C 厚度链：等厚 = (D62−D61)/2000 × IDW²(Vint)，逐像元写在 D61
    // 既有网格上（不扩界），贡献井凸包外 −9999，约束线不参与，不转相面。
    void thicknessRasterIsIsochronTimesIdwVint()
    {
        Fixture f;
        QVERIFY( f.init() );
        // 常值时间面：D62 − D61 = 100ms 全图。
        const QString d61 = makeConstRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ), 2200.0f );
        const QString d62 = makeConstRaster( f.dir.filePath( QStringLiteral( "d62.tif" ) ), 2300.0f );
        QVERIFY( !d61.isEmpty() && !d62.isEmpty() );
        // 约束面声明仍在，但新链不得把它当裁剪多边形（证明点见下）。
        QVERIFY( writeConstraintPolygon( f.dir.filePath( QStringLiteral( "cons.geojson" ) ) ) );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        for ( const QString &h : { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } )
        {
            LayerDeclaration rasterDecl;
            rasterDecl.layerId = QStringLiteral( "horizon.%1.derived" ).arg( h );
            rasterDecl.horizon = h;
            rasterDecl.type = QStringLiteral( "raster" );
            rasterDecl.source = h == QLatin1String( "D61" ) ? d61 : d62;
            rasterDecl.group = QStringLiteral( "00_Horizon" );
            QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        }
        LayerDeclaration consDecl;
        consDecl.layerId = QStringLiteral( "constraints.D61" );
        consDecl.horizon = QStringLiteral( "D61" );
        consDecl.type = QStringLiteral( "vector" );
        consDecl.source = f.dir.filePath( QStringLiteral( "cons.geojson" ) );
        consDecl.group = QStringLiteral( "02_Constraints" );
        QVERIFY2( f.layers.declare( consDecl, &err ), qPrintable( err ) );

        QSignalSpy doneSpy( &f.mapping, &MappingWorkflow::chainDone );
        QSignalSpy failSpy( &f.mapping, &MappingWorkflow::chainFailed );

        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ), qPrintable( err ) );
        QCOMPARE( failSpy.count(), 0 );
        QCOMPARE( doneSpy.count(), 1 );
        QCOMPARE( doneSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "D61" ) );
        QCOMPARE( doneSpy.at( 0 ).at( 1 ).toString(),
                  QStringLiteral( "factor.D61.idw" ) );

        // --- 等厚栅格：声明 + 落盘；网格必须是 D61 网格本身 -----------------
        const LayerDeclaration *thick =
            findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY2( thick != nullptr, "等厚图层未声明" );
        QCOMPARE( thick->type, QStringLiteral( "raster" ) );
        QCOMPARE( thick->title, QStringLiteral( "D61–D62 等厚（米）" ) );
        QCOMPARE( thick->horizon, QStringLiteral( "D61" ) );
        QVERIFY( QFile::exists( thick->source ) );

        GDALDatasetH ds = GDALOpen( thick->source.toUtf8().constData(), GA_ReadOnly );
        QVERIFY( ds != nullptr );
        QCOMPARE( GDALGetRasterXSize( ds ), 10 ); // D61 原尺寸，不外扩
        QCOMPARE( GDALGetRasterYSize( ds ), 8 );
        double gt[6] = { 0, 0, 0, 0, 0, 0 };
        GDALGetGeoTransform( ds, gt );
        const double d61gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
        for ( int i = 0; i < 6; ++i )
            QCOMPARE( gt[i], d61gt[i] );
        int hasNd = 0;
        QCOMPARE( GDALGetRasterNoDataValue( GDALGetRasterBand( ds, 1 ), &hasNd ), -9999.0 );
        QVERIFY( hasNd );
        GDALClose( ds );

        // 凸包内（且约束多边形外）：值 = 100/2000 × IDW²(vint)。
        const double inHull = sampleAt( thick->source, 550.0, 650.0 );
        QVERIFY2( !qIsNaN( inHull ), "hull 内像元不应是 nodata（约束线不参与裁剪）" );
        QVERIFY( qAbs( inHull - expectedIsochronThickness( 100.0, 550.0, 650.0 ) ) < 0.5 );
        const double inHull2 = sampleAt( thick->source, 350.0, 450.0 );
        QVERIFY( qAbs( inHull2 - expectedIsochronThickness( 100.0, 350.0, 450.0 ) ) < 0.5 );
        // 凸包外 → nodata。
        QVERIFY( qIsNaN( sampleAt( thick->source, 50.0, 50.0 ) ) );
        QVERIFY( qIsNaN( sampleAt( thick->source, 950.0, 750.0 ) ) );
        delete thick;

        // 不产相多边形；井点层 wells.thickness 是 PDF 井位/井名的数据源，应在。
        QVERIFY( findDecl( f.layers, QStringLiteral( "facies.D61" ) ) == nullptr );
        const LayerDeclaration *wells =
            findDecl( f.layers, QStringLiteral( "wells.thickness.D61" ) );
        QVERIFY2( wells != nullptr, "井点层未声明" );
        QVERIFY( QFile::exists( wells->source ) );
        delete wells;
    }

    // T26（wave3/derived-publish）：厚度链产物落 <工程>/artifacts/derived/ 并登记
    // DERIVED catalog 版本——父版本 = D61/D62 时间栅格版本，sha256 可复验，重跑
    // 进同一资产的下一版本；重启（重开 catalog）后路径仍是活文件。
    void thicknessOutputsAreRegisteredDerivedVersions()
    {
        Fixture f;
        QVERIFY( f.init() );

        // 把 D61/D62 时间栅格放进 catalog 受管路径并登记版本（父版本 provenance 源）。
        const QString d61Rel = QStringLiteral( "artifacts/derived/ast-d61/ver-d61/D61.tif" );
        const QString d62Rel = QStringLiteral( "artifacts/derived/ast-d62/ver-d62/D62.tif" );
        const QString d61Abs = f.dir.filePath( d61Rel );
        const QString d62Abs = f.dir.filePath( d62Rel );
        QVERIFY( QDir().mkpath( QFileInfo( d61Abs ).absolutePath() ) );
        QVERIFY( QDir().mkpath( QFileInfo( d62Abs ).absolutePath() ) );
        QVERIFY( !makeConstRaster( d61Abs, 2200.0f ).isEmpty() );
        QVERIFY( !makeConstRaster( d62Abs, 2300.0f ).isEmpty() );
        for ( const auto &[astId, verId, rel] :
              { std::tuple{ QStringLiteral( "ast-d61" ), QStringLiteral( "ver-d61" ), d61Rel },
                std::tuple{ QStringLiteral( "ast-d62" ), QStringLiteral( "ver-d62" ), d62Rel } } )
        {
            CatalogAsset a;
            a.id = astId;
            a.type = QStringLiteral( "horizon" );
            a.format = QStringLiteral( "tif" );
            a.displayName = astId;
            QVERIFY( f.catalog.addAsset( a ) );
            CatalogVersion v;
            v.id = verId;
            v.assetId = a.id;
            v.stage = QStringLiteral( "DERIVED" );
            v.versionNumber = 1;
            v.path = rel;
            v.fileName = QStringLiteral( "D61.tif" );
            QString hashErr;
            v.sha256 = DataCatalog::sha256FileHex( f.dir.filePath( rel ), &hashErr );
            QVERIFY2( !v.sha256.isEmpty(), qPrintable( hashErr ) );
            QVERIFY( f.catalog.addVersion( v ) );
        }

        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        for ( const QString &h : { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } )
        {
            LayerDeclaration rasterDecl;
            rasterDecl.layerId = QStringLiteral( "horizon.%1.derived" ).arg( h );
            rasterDecl.horizon = h;
            rasterDecl.type = QStringLiteral( "raster" );
            rasterDecl.source = h == QLatin1String( "D61" ) ? d61Abs : d62Abs;
            QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        }

        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ),
                  qPrintable( err ) );

        const LayerDeclaration *thick =
            findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY2( thick != nullptr, "等厚图层未声明" );
        // 声明源在工程受管目录下（不是 /tmp）。
        QVERIFY2( thick->source.startsWith(
                      f.dir.filePath( QStringLiteral( "artifacts/derived/" ) ) ),
                  qPrintable( thick->source ) );

        // catalog 里能查到 DERIVED 版本：sha 可复验、父版本 = 两张时间栅格版本。
        QString thicknessVersionId;
        for ( const CatalogAsset &a : f.catalog.assets() )
        {
            if ( a.type != QLatin1String( "thickness_raster" ) )
                continue;
            for ( const CatalogVersion &v : f.catalog.versionsForAsset( a.id ) )
            {
                const QString resolved = f.dir.filePath( v.path );
                if ( resolved == thick->source )
                {
                    thicknessVersionId = v.id;
                    QCOMPARE( v.stage, QStringLiteral( "DERIVED" ) );
                    QCOMPARE( v.versionNumber, 1 );
                    QStringList expectedParents{ QStringLiteral( "ver-d61" ),
                                                 QStringLiteral( "ver-d62" ) };
                    QCOMPARE( v.parentVersionIds, expectedParents );
                    QString hashErr;
                    QCOMPARE( v.sha256,
                              DataCatalog::sha256FileHex( resolved, &hashErr ) );
                }
            }
        }
        QVERIFY2( !thicknessVersionId.isEmpty(), "厚度栅格未登记 DERIVED 版本" );

        // 井点层（wells.thickness）同样受管 + 登记。
        const LayerDeclaration *wells =
            findDecl( f.layers, QStringLiteral( "wells.thickness.D61" ) );
        QVERIFY2( wells != nullptr, "井点层未声明" );
        QVERIFY( wells->source.startsWith(
            f.dir.filePath( QStringLiteral( "artifacts/derived/" ) ) ) );
        bool wellsVersionRegistered = false;
        for ( const CatalogAsset &a : f.catalog.assets() )
        {
            if ( a.type != QLatin1String( "thickness_wells" ) )
                continue;
            for ( const CatalogVersion &v : f.catalog.versionsForAsset( a.id ) )
                if ( f.dir.filePath( v.path ) == wells->source )
                    wellsVersionRegistered = !v.sha256.isEmpty();
        }
        QVERIFY( wellsVersionRegistered );
        delete thick;
        delete wells;

        // 重跑 → 同一资产的版本 2（重算不覆盖历史版本）。
        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ),
                  qPrintable( err ) );
        const LayerDeclaration *thick2 =
            findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY( thick2 != nullptr );
        bool sawVersion2 = false;
        for ( const CatalogAsset &a : f.catalog.assets() )
        {
            if ( a.type != QLatin1String( "thickness_raster" ) )
                continue;
            for ( const CatalogVersion &v : f.catalog.versionsForAsset( a.id ) )
                if ( f.dir.filePath( v.path ) == thick2->source && v.versionNumber == 2 )
                    sawVersion2 = true;
        }
        QVERIFY( sawVersion2 );
        delete thick2;

        // 重启存活：新会话重开同一工程目录的 catalog，版本路径仍是活文件且 sha 一致。
        DataCatalog reopened;
        QString reopenErr;
        QVERIFY2( reopened.open( f.dir.path(), &reopenErr ), qPrintable( reopenErr ) );
        const CatalogVersion v = reopened.versionById( thicknessVersionId );
        QVERIFY( !v.id.isEmpty() );
        const QString resolved = f.dir.filePath( v.path );
        QVERIFY2( QFile::exists( resolved ), qPrintable( resolved ) );
        QString hashErr;
        QCOMPARE( reopened.sha256FileHex( resolved, &hashErr ), v.sha256 );
    }

    // T19/audit #35：D62 早于等于 D61（isochron ≤ 0）的像元写 nodata——按格
    // 逐判，同幅上正等时区照常出厚度（不是整幅失败）。D61 常值 2200；
    // D62 左半 2100（isochron −100 → nodata）、右半 2300（+100 → 厚度）。
    void thicknessNegativeIsochronIsNodata()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString d61 = makeConstRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ), 2200.0f );
        QVERIFY( !d61.isEmpty() );
        const QString d62 = f.dir.filePath( QStringLiteral( "d62.tif" ) );
        {
            GDALAllRegister();
            GDALDriverH drv = GDALGetDriverByName( "GTiff" );
            GDALDatasetH ds = GDALCreate( drv, d62.toUtf8().constData(), 10, 8, 1,
                                        GDT_Float32, nullptr );
            QVERIFY( ds );
            const double gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
            GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
            QVector<float> px( 80, 2300.0f );
            for ( int r = 0; r < 8; ++r )
                for ( int c = 0; c < 5; ++c )
                    px[r * 10 + c] = 2100.0f; // 左半：基面早于层位
            GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
            GDALSetRasterNoDataValue( band, -9999.0 );
            QCOMPARE( GDALRasterIO( band, GF_Write, 0, 0, 10, 8,
                                    const_cast<float *>( px.constData() ), 10, 8,
                                    GDT_Float32, 0, 0 ),
                      CE_None );
            GDALClose( ds );
        }
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        for ( const QString &h : { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } )
        {
            LayerDeclaration rasterDecl;
            rasterDecl.layerId = QStringLiteral( "horizon.%1.derived" ).arg( h );
            rasterDecl.horizon = h;
            rasterDecl.type = QStringLiteral( "raster" );
            rasterDecl.source = h == QLatin1String( "D61" ) ? d61 : d62;
            QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        }

        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ),
                  qPrintable( err ) );
        const LayerDeclaration *thick =
            findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY2( thick != nullptr, "等厚图层未声明" );

        // (400,500) 凸包内但 col4 → isochron −100ms → nodata，不写成负厚度。
        QVERIFY( qIsNaN( sampleAt( thick->source, 400.0, 500.0 ) ) );
        // (550,650) 凸包内 col5 → isochron +100ms → 照常出厚度（同口径公式）。
        const double pos = sampleAt( thick->source, 550.0, 650.0 );
        QVERIFY2( !qIsNaN( pos ), "正等时像元应照常出厚度" );
        QVERIFY( qAbs( pos - expectedIsochronThickness( 100.0, 550.0, 650.0 ) ) < 0.5 );
        // 凸包外照旧 nodata。
        QVERIFY( qIsNaN( sampleAt( thick->source, 50.0, 50.0 ) ) );
        delete thick;
    }

    // D61/D62 网格不一致 → 不写等厚，错误文案如实说明。
    void chainGridMismatchRefuses()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString d61 = makeConstRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ), 2200.0f );
        const QString d62 = makeConstRaster( f.dir.filePath( QStringLiteral( "d62.tif" ) ),
                                             2300.0f, 5, 4 ); // 尺寸不一致
        QVERIFY( !d61.isEmpty() && !d62.isEmpty() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        for ( const QString &h : { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } )
        {
            LayerDeclaration rasterDecl;
            rasterDecl.layerId = QStringLiteral( "horizon.%1.derived" ).arg( h );
            rasterDecl.horizon = h;
            rasterDecl.type = QStringLiteral( "raster" );
            rasterDecl.source = h == QLatin1String( "D61" ) ? d61 : d62;
            QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        }

        QSignalSpy failSpy( &f.mapping, &MappingWorkflow::chainFailed );
        QVERIFY( !f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ) );
        QVERIFY( err.contains( QStringLiteral( "不一致" ) ) );
        QCOMPARE( failSpy.count(), 1 );
        QVERIFY( findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) ) == nullptr );
    }

    // 层位栅格缺失 → 退回井点厚度 IDW，层名「井点厚度（米，无层位栅格）」。
    void chainFallbackWhenHorizonRasterMissing()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ),
                  qPrintable( err ) );
        const LayerDeclaration *d =
            findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY2( d != nullptr, "退回层未声明" );
        QCOMPARE( d->type, QStringLiteral( "raster" ) );
        QCOMPARE( d->title, QStringLiteral( "井点厚度（米，无层位栅格）" ) );
        QVERIFY( QFile::exists( d->source ) );
        // 凸包内 (500,600)：井点厚度 IDW² → 55–100m 之间的插值。
        const double near = sampleAt( d->source, 500.0, 600.0 );
        QVERIFY2( !qIsNaN( near ), "hull 内不应 nodata" );
        QVERIFY( near > 30.0 && near < 120.0 );
        // 凸包外 → nodata。
        QVERIFY( qIsNaN( sampleAt( d->source, 100.0, 100.0 ) ) );
        delete d;
        QVERIFY( findDecl( f.layers, QStringLiteral( "facies.D61" ) ) == nullptr );
    }

    // 样本不足：0 口 → 「没有厚度样本」；<3 口 → 「厚度样本不足以成面」。
    // 两句都是面板文案（thicknessSampleMessage），不发 chainDone、不写栅格。
    void chainInsufficientSamples()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString d61 = makeConstRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ), 2200.0f );
        const QString d62 = makeConstRaster( f.dir.filePath( QStringLiteral( "d62.tif" ) ), 2300.0f );
        QVERIFY( !d61.isEmpty() && !d62.isEmpty() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        for ( const QString &h : { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } )
        {
            LayerDeclaration rasterDecl;
            rasterDecl.layerId = QStringLiteral( "horizon.%1.derived" ).arg( h );
            rasterDecl.horizon = h;
            rasterDecl.type = QStringLiteral( "raster" );
            rasterDecl.source = h == QLatin1String( "D61" ) ? d61 : d62;
            QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        }

        QSignalSpy doneSpy( &f.mapping, &MappingWorkflow::chainDone );

        // D62→D63：只有 A1 有 D63 分层 → 1 口贡献井 < 3。
        QVERIFY( !f.mapping.runThicknessChain( QStringLiteral( "D62" ), &err ) );
        QCOMPARE( f.mapping.thicknessSampleMessage(),
                  QStringLiteral( "厚度样本不足以成面" ) );
        // 行表仍在：6 口井逐行，A1 贡献。
        QCOMPARE( f.mapping.thicknessSampleRows().size(), 6 );

        // D53→D61：没有任何井有 D53 分层 → 0 口。
        QVERIFY( !f.mapping.runThicknessChain( QStringLiteral( "D53" ), &err ) );
        QCOMPARE( f.mapping.thicknessSampleMessage(), QStringLiteral( "没有厚度样本" ) );

        QCOMPARE( doneSpy.count(), 0 );
    }

    // 时间残差（autoplan §5C）：每口井一行——D61 分层的 TD 插值时间 vs
    // D61 栅格包含像元采样（左闭右开）。残差保符号：A1 +215.6、A6 −100.5
    // 超 10ms 阈值；A2 −0.0、A5 −0.8 通过；A3 无时深表；A7 无 D61 分层。
    void timeResidualIssues()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString tif = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );

        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );

        const QList<TimeResidualRow> rows =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 10.0 );
        QCOMPARE( rows.size(), 6 ); // 每口井一行

        auto rowFor = [&rows]( const QString &name ) -> const TimeResidualRow * {
            for ( const TimeResidualRow &r : rows )
                if ( r.wellName == name )
                    return &r;
            return nullptr;
        };
        const TimeResidualRow *a1 = rowFor( QStringLiteral( "A1" ) );
        QVERIFY( a1 );
        QCOMPARE( a1->status, TimeResidualRow::Status::Exceeds );
        // TD 插值 2216.67ms vs 栅格 2001.1ms（col3,row4）→ +215.6ms（保符号）
        QVERIFY( qAbs( a1->timeMs - 2216.67 ) < 0.5 );
        QVERIFY( qAbs( a1->rasterMs - 2001.1 ) < 0.5 );
        QVERIFY( a1->residualMs > 0.0 );
        QVERIFY( qAbs( a1->residualMs - 215.57 ) < 1.0 );
        QVERIFY( qAbs( a1->x - 300.0 ) < 0.01 ); // 分层 X/Y 优先
        QCOMPARE( a1->inlineNo, 1520 ); // inline follows Y; X follows crossline

        const TimeResidualRow *a6 = rowFor( QStringLiteral( "A6" ) );
        QVERIFY( a6 );
        QCOMPARE( a6->status, TimeResidualRow::Status::Exceeds );
        QVERIFY( a6->residualMs < 0.0 ); // 负残差保留符号
        QVERIFY( qAbs( a6->residualMs - ( -100.5 ) ) < 1.0 );

        const TimeResidualRow *a2 = rowFor( QStringLiteral( "A2" ) );
        QVERIFY( a2 );
        QCOMPARE( a2->status, TimeResidualRow::Status::Pass );
        QVERIFY( qAbs( a2->residualMs ) <= 10.0 );

        const TimeResidualRow *a5 = rowFor( QStringLiteral( "A5" ) );
        QVERIFY( a5 );
        QCOMPARE( a5->status, TimeResidualRow::Status::Pass );
        QCOMPARE( a5->inlineNo, a6->inlineNo ); // same Y, different X

        const TimeResidualRow *a3 = rowFor( QStringLiteral( "A3" ) );
        QVERIFY( a3 );
        QCOMPARE( a3->status, TimeResidualRow::Status::NotComputed );
        QCOMPARE( a3->reason, QStringLiteral( "无时深表" ) );
        QVERIFY( qIsNaN( a3->residualMs ) ); // 非数值行不给残差数

        const TimeResidualRow *a7 = rowFor( QStringLiteral( "A7" ) );
        QVERIFY( a7 );
        QCOMPARE( a7->status, TimeResidualRow::Status::NotComputed );
        QCOMPARE( a7->reason, QStringLiteral( "无 D61 分层" ) );

        // 阈值放大 → 全部数值行转 Pass（没有行消失）。
        const QList<TimeResidualRow> loose =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 500.0 );
        QCOMPARE( loose.size(), 6 );
        for ( const TimeResidualRow &r : loose )
            QVERIFY( r.status != TimeResidualRow::Status::Exceeds );

        // ValidationWorkflow 集成：validate() 产出逐井行 + 超限问题。
        ValidationWorkflow vw( &f.layers, &f.store );
        vw.setProjectData( &f.pd );
        vw.setResidualThresholdMs( 10.0 );
        const QList<ValidationIssue> all = vw.validate();
        QCOMPARE( vw.lastResidualRows().size(), 6 );
        // T24：残差行须带联动定位所需的 layer_id/horizon/inline（验证页双击
        // 残差行 → 地图/连井/剖面同一条 locateRequested 载荷）。
        const QVariantMap firstRow = vw.lastResidualRows().first().toMap();
        QCOMPARE( firstRow.value( QStringLiteral( "layer_id" ) ).toString(),
                  QStringLiteral( "horizon.D61.derived" ) );
        QCOMPARE( firstRow.value( QStringLiteral( "horizon" ) ).toString(),
                  QStringLiteral( "D61" ) );
        QVERIFY( firstRow.contains( QStringLiteral( "well_id" ) ) &&
                 firstRow.contains( QStringLiteral( "inline" ) ) &&
                 firstRow.contains( QStringLiteral( "x" ) ) );
        int nExceeds = 0;
        const ValidationIssue *residualIssue = nullptr;
        for ( const ValidationIssue &v : all )
        {
            if ( v.code != QLatin1String( "TIME_RESIDUAL" ) )
                continue;
            ++nExceeds;
            QCOMPARE( v.severity, ValidationIssue::Warning );
            QCOMPARE( v.horizon, QStringLiteral( "D61" ) );
            QCOMPARE( v.layerId, QStringLiteral( "horizon.D61.derived" ) );
            if ( v.wellId == QLatin1String( "well-1" ) )
            {
                residualIssue = &v;
                QVERIFY( v.wktLocation.contains( QStringLiteral( "POINT(300" ) ) );
                QCOMPARE( v.details.value( QStringLiteral( "inline" ) ).toInt(), 1520 );
            }
        }
        QCOMPARE( nExceeds, 2 ); // A1 + A6
        QVERIFY( residualIssue );

        // 序列化往返保留新增字段。
        const ValidationIssue roundtrip = ValidationIssue::fromMap( residualIssue->toMap() );
        QCOMPARE( roundtrip.wellId, QStringLiteral( "well-1" ) );
        QCOMPARE( roundtrip.details.value( QStringLiteral( "inline" ) ).toInt(), 1520 );

        // A deviated top occupies a different raster cell from its wellhead.
        // The publish summary must agree with the validation sample point.
        const QString topsPath = f.dir.filePath( QStringLiteral( "raw/ast-1/ver-1/DC.dat" ) );
        QFile topFile( topsPath );
        QVERIFY( topFile.open( QIODevice::ReadOnly ) );
        QString shifted = QString::fromUtf8( topFile.readAll() );
        topFile.close();
        QVERIFY( shifted.contains( QStringLiteral( "A1           D61          2148.000     300.000" ) ) );
        shifted.replace( QStringLiteral( "A1           D61          2148.000     300.000" ),
                         QStringLiteral( "A1           D61          2148.000     900.000" ) );
        QVERIFY( writeText( topsPath, shifted ) );
        const QList<TimeResidualRow> shiftedRows =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 10.0 );
        const QJsonObject summary = QJsonDocument::fromJson(
            MapVersionController::residualSummaryJson( &f.pd, QStringLiteral( "D61" ) ).toUtf8() ).object();
        bool foundShiftedA1 = false;
        for ( const QJsonValue &value : summary.value( QStringLiteral( "rows" ) ).toArray() )
        {
            const QJsonObject row = value.toObject();
            if ( row.value( QStringLiteral( "well_id" ) ).toString() != QStringLiteral( "well-1" ) )
                continue;
            foundShiftedA1 = true;
            QCOMPARE( row.value( QStringLiteral( "kind" ) ).toString(), QStringLiteral( "residual" ) );
            for ( const TimeResidualRow &validation : shiftedRows )
                if ( validation.wellId == QStringLiteral( "well-1" ) )
                    QVERIFY( qAbs( row.value( QStringLiteral( "residual_ms" ) ).toDouble()
                                   - validation.residualMs ) < 0.01 );
        }
        QVERIFY( foundShiftedA1 );
    }

    // 采样边界（autoplan §5C）：井位恰在外边界 → 归末像元；网外/空道是
    // 警告行不是数值残差。A8 在 (1000,0)（右+下外边界）采到末列末行；
    // A9 在 (-50,-50) 网外；A10 压在空道像元上。
    void residualEdgeSampling()
    {
        Fixture f;
        QVERIFY( f.init() );

        // 栅格：末列末行（col9,row7）= 2050，其余 2000；(0,0) 像元 = nodata。
        const QString tif = f.dir.filePath( QStringLiteral( "d61_edge.tif" ) );
        {
            GDALAllRegister();
            GDALDriverH drv = GDALGetDriverByName( "GTiff" );
            GDALDatasetH ds = GDALCreate( drv, tif.toUtf8().constData(), 10, 8, 1,
                                        GDT_Float32, nullptr );
            QVERIFY( ds );
            const double gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
            GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
            GDALSetMetadataItem( ds, "PALEO_INLINE_MIN", "1315", nullptr );
            GDALSetMetadataItem( ds, "PALEO_INLINE_MAX", "1725", nullptr );
            QVector<float> px( 80, 2000.0f );
            px[7 * 10 + 9] = 2050.0f;
            px[0] = -9999.0f;
            GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
            GDALSetRasterNoDataValue( band, -9999.0 );
            QCOMPARE( GDALRasterIO( band, GF_Write, 0, 0, 10, 8,
                                    const_cast<float *>( px.constData() ), 10, 8,
                                    GDT_Float32, 0, 0 ),
                      CE_None );
            GDALClose( ds );
        }

        int n = 0;
        auto addEdgeWell = [&f, &n]( const QString &name, double x, double y,
                                     double tvd ) {
            ++n;
            const QString id = QStringLiteral( "well-e%1" ).arg( n );
            CatalogEntity e;
            e.id = id;
            e.entityType = QStringLiteral( "well" );
            e.name = name;
            e.surfaceX = x;
            e.surfaceY = y;
            e.hasSurface = true;
            e.coordinateStatus = QStringLiteral( "untransformed" );
            if ( !f.catalog.addEntity( e ) )
                return false;
            const QString astT = QStringLiteral( "ast-e%1t" ).arg( n );
            const QString astD = QStringLiteral( "ast-e%1d" ).arg( n );
            const QString verT = QStringLiteral( "ver-e%1t" ).arg( n );
            const QString verD = QStringLiteral( "ver-e%1d" ).arg( n );
            auto add = [&]( const QString &assetId, const QString &verId,
                            const QString &type, const QString &file,
                            const QString &text, const QString &role ) {
                CatalogAsset a;
                a.id = assetId;
                a.type = type;
                a.format = QStringLiteral( "dat" );
                a.displayName = file;
                if ( !f.catalog.addAsset( a ) )
                    return false;
                CatalogVersion v;
                v.id = verId;
                v.assetId = a.id;
                v.stage = QStringLiteral( "RAW" );
                v.path = DataCatalog::managedPath( QStringLiteral( "raw" ), a.id,
                                                   v.id, file );
                if ( !f.catalog.addVersion( v ) )
                    return false;
                EntityAssetLink l;
                l.entityType = QStringLiteral( "well" );
                l.entityId = id;
                l.assetId = a.id;
                l.role = role;
                l.isPrimary = true;
                return f.catalog.addLink( l ) &&
                       writeText( f.dir.filePath( v.path ), text );
            };
            const QString tops = QStringLiteral(
                "#WellTops File From SMI\n%1 D61 %2 %3 %4 %5 %5 -99999.000\n" )
                .arg( name )
                .arg( tvd, 0, 'f', 3 )
                .arg( x, 0, 'f', 3 )
                .arg( y, 0, 'f', 3 )
                .arg( tvd, 0, 'f', 3 );
            // TD：恒等斜率 1ms/1m（TVD 列 t=深度），使井上时间 = TVD 本身。
            const QString td = QStringLiteral(
                "#TimeDepth File From SMI\n# Well : %1\n"
                "%2 -%3 %3 %3\n%4 -%5 %5 %5\n" )
                .arg( name )
                .arg( tvd, 0, 'f', 3 )
                .arg( tvd, 0, 'f', 3 )
                .arg( tvd + 100.0, 0, 'f', 3 )
                .arg( tvd + 100.0, 0, 'f', 3 );
            return add( astT, verT, QStringLiteral( "well_stratification" ),
                        QStringLiteral( "DC%1.dat" ).arg( n ), tops,
                        QStringLiteral( "tops" ) ) &&
                   add( astD, verD, QStringLiteral( "time_depth" ),
                        QStringLiteral( "A%1_TD.dat" ).arg( n ), td,
                        QStringLiteral( "time_depth" ) );
        };
        // A8 (1000,0) → col=10→末列、row=8→末行 → 2050ms；井上 2050 → r=0。
        QVERIFY( addEdgeWell( QStringLiteral( "A8" ), 1000.0, 0.0, 2050.0 ) );
        // A9 (-50,-50) 网外。
        QVERIFY( addEdgeWell( QStringLiteral( "A9" ), -50.0, -50.0, 2050.0 ) );
        // A10 (50,750) → col0,row0 → 空道。
        QVERIFY( addEdgeWell( QStringLiteral( "A10" ), 50.0, 750.0, 2050.0 ) );

        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );

        const QList<TimeResidualRow> rows =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 10.0 );
        auto rowFor = [&rows]( const QString &name ) -> const TimeResidualRow * {
            for ( const TimeResidualRow &r : rows )
                if ( r.wellName == name )
                    return &r;
            return nullptr;
        };
        const TimeResidualRow *a8 = rowFor( QStringLiteral( "A8" ) );
        QVERIFY( a8 );
        QCOMPARE( a8->status, TimeResidualRow::Status::Pass ); // 外边界归末像元
        QVERIFY( qAbs( a8->rasterMs - 2050.0 ) < 0.01 );
        QVERIFY( qAbs( a8->residualMs ) < 0.5 );

        const TimeResidualRow *a9 = rowFor( QStringLiteral( "A9" ) );
        QVERIFY( a9 );
        QCOMPARE( a9->status, TimeResidualRow::Status::Warn );
        QCOMPARE( a9->reason, QStringLiteral( "井位不在测网内" ) );
        QVERIFY( qIsNaN( a9->residualMs ) );

        const TimeResidualRow *a10 = rowFor( QStringLiteral( "A10" ) );
        QVERIFY( a10 );
        QCOMPARE( a10->status, TimeResidualRow::Status::Warn );
        QCOMPARE( a10->reason, QStringLiteral( "井位落在空道" ) );
    }

    // T25：发布门残差（residualSummaryJson）与验证表（computeTimeResiduals）
    // 同口径——采样点取分层 X/Y（井口兜底）、同一包含像元规则、同一符号
    // （井时间 − 栅格时间）。AX 井口在网外 (−50,−50) 但分层点在 (300,400)
    // 网内：旧口径采井口必判「井位不在测网内」，新口径必须出数值残差，
    // 且与验证表逐井对齐。
    void publishGateResidualMatchesValidation()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString tif = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );
        QVERIFY( addWellWithOffsetTop( f, 1, QStringLiteral( "AX" ),
                                       -50.0, -50.0, 300.0, 400.0, 2000.0 ) );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );

        MapVersionStore versions( f.dir.filePath( QStringLiteral( "versions.sqlite" ) ) );
        QVERIFY2( versions.open( &err ), qPrintable( err ) );
        MapVersionController ctl( &versions, &f.layers );
        const QJsonObject summary = QJsonDocument::fromJson(
            ctl.residualSummaryJson( &f.pd, QStringLiteral( "D61" ) ).toUtf8() )
            .object();
        QCOMPARE( summary.value( QStringLiteral( "wells_total" ) ).toInt(), 7 );
        QCOMPARE( summary.value( QStringLiteral( "covered" ) ).toInt(), 7 );
        QVERIFY( summary.value( QStringLiteral( "missing" ) ).toArray().isEmpty() );

        const QList<TimeResidualRow> rows =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 10.0 );
        QCOMPARE( rows.size(), 7 );
        const QJsonArray gateRows =
            summary.value( QStringLiteral( "rows" ) ).toArray();
        QCOMPARE( gateRows.size(), rows.size() );
        for ( const TimeResidualRow &row : rows )
        {
            QJsonObject gr;
            for ( const auto &v : gateRows )
                if ( v.toObject().value( QStringLiteral( "well_id" ) ).toString() ==
                     row.wellId )
                    gr = v.toObject();
            QVERIFY2( !gr.isEmpty(), qPrintable( row.wellId ) );
            if ( std::isfinite( row.residualMs ) )
            {
                // 数值残差逐井一致（同一采样点 × 同一公式）。
                QCOMPARE( gr.value( QStringLiteral( "kind" ) ).toString(),
                          QStringLiteral( "residual" ) );
                QVERIFY( qAbs( gr.value( QStringLiteral( "residual_ms" ) )
                                         .toDouble() -
                               row.residualMs ) < 0.001 );
            }
            else
            {
                QCOMPARE( gr.value( QStringLiteral( "kind" ) ).toString(),
                          QStringLiteral( "reason" ) );
                // 原因文案同口径（TD 码/无分层/网外/空道共用一份措辞）。
                QCOMPARE( gr.value( QStringLiteral( "reason" ) ).toString(),
                          row.reason );
            }
        }
        // AX 是分层点采样的判别井：采分层点 → kind=residual、r ≈ −1.1ms；
        // 误采井口（−50,−50）→ 网外 reason（旧发布门行为）。
        const TimeResidualRow *ax = nullptr;
        for ( const TimeResidualRow &r : rows )
            if ( r.wellName == QLatin1String( "AX" ) )
                ax = &r;
        QVERIFY( ax );
        QVERIFY( std::isfinite( ax->residualMs ) );
        for ( const auto &v : gateRows )
            if ( v.toObject().value( QStringLiteral( "name" ) ).toString() ==
                 QLatin1String( "AX" ) )
            {
                QCOMPARE( v.toObject().value( QStringLiteral( "kind" ) ).toString(),
                          QStringLiteral( "residual" ) );
                QVERIFY( qAbs( v.toObject()
                                   .value( QStringLiteral( "residual_ms" ) )
                                   .toDouble() -
                               ax->residualMs ) < 0.001 );
            }
    }

    // T25：D61 栅格缺失/打不开 → validate() 发 RASTER_MISSING 问题（原因
    // 「层位 D61 还没有时间栅格」），不让验证页把「没跑成」显示成「还没计算」；
    // 发布门摘要同口径：有分层+TD 的井进 missing[]。
    void residualMissingRasterIsExplicitIssue()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        // 故意不声明任何 D61 栅格。

        ValidationWorkflow vw( &f.layers, &f.store );
        vw.setProjectData( &f.pd );
        const QList<ValidationIssue> issues = vw.validate();
        bool found = false;
        for ( const ValidationIssue &v : issues )
        {
            if ( v.code != QLatin1String( "RASTER_MISSING" ) )
                continue;
            found = true;
            QCOMPARE( v.severity, ValidationIssue::Warning );
            QCOMPARE( v.horizon, QStringLiteral( "D61" ) );
            QCOMPARE( v.message, QStringLiteral( "层位 D61 还没有时间栅格" ) );
        }
        QVERIFY2( found, "缺失的 D61 栅格必须发 RASTER_MISSING 问题" );
        QVERIFY( vw.lastResidualRows().isEmpty() );

        // 残差行也被清成空表对应的「没跑成」语义——发布门摘要同样写明。
        MapVersionStore versions( f.dir.filePath( QStringLiteral( "versions.sqlite" ) ) );
        QString err;
        QVERIFY2( versions.open( &err ), qPrintable( err ) );
        MapVersionController ctl( &versions, &f.layers );
        const QJsonObject s = QJsonDocument::fromJson(
            ctl.residualSummaryJson( &f.pd, QStringLiteral( "D61" ) ).toUtf8() )
            .object();
        QCOMPARE( s.value( QStringLiteral( "wells_total" ) ).toInt(), 6 );
        const QJsonArray missing =
            s.value( QStringLiteral( "missing" ) ).toArray();
        QCOMPARE( missing.size(), 4 ); // A1/A2/A5/A6 有分层+TD，缺的是栅格
        QVERIFY( s.value( QStringLiteral( "covered" ) ).toInt() < 6 );
        for ( const auto &v : s.value( QStringLiteral( "rows" ) ).toArray() )
        {
            const QJsonObject r = v.toObject();
            if ( r.value( QStringLiteral( "name" ) ).toString() ==
                 QLatin1String( "A1" ) )
                QCOMPARE( r.value( QStringLiteral( "reason" ) ).toString(),
                          QStringLiteral( "层位 D61 还没有时间栅格" ) );
        }
    }

    // MD 兜底（plan §3）：分层 TVD 空（-99999）→ 用 MD 对 TD 的 MD 列。
    // A4 的 D61 分层只有 MD=2050；其 TD 表 TVD 列全 -99999、MD 列单调增。
    // 走 MD 列插值得 2050ms，与栅格 ~2001.4ms 差 >1ms → TIME_RESIDUAL；
    // 若误走 TVD 列（或不兜底）则查不出时间，这条问题不会出现。
    void timeResidualMdFallback()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString tif = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );

        CatalogEntity e;
        e.id = QStringLiteral( "well-4" );
        e.entityType = QStringLiteral( "well" );
        e.name = QStringLiteral( "A4" );
        e.surfaceX = 600.0;
        e.surfaceY = 400.0;
        e.hasSurface = true;
        e.coordinateStatus = QStringLiteral( "untransformed" );
        QVERIFY( f.catalog.addEntity( e ) );

        auto addFile = [&f]( const QString &assetId, const QString &type,
                             const QString &verId, const QString &fileName,
                             const QString &text, const QString &role ) {
            CatalogAsset a;
            a.id = assetId;
            a.type = type;
            a.format = QStringLiteral( "dat" );
            a.displayName = fileName;
            if ( !f.catalog.addAsset( a ) )
                return false;
            CatalogVersion v;
            v.id = verId;
            v.assetId = a.id;
            v.stage = QStringLiteral( "RAW" );
            v.path = DataCatalog::managedPath( QStringLiteral( "raw" ), a.id, v.id, fileName );
            if ( !f.catalog.addVersion( v ) )
                return false;
            EntityAssetLink l;
            l.entityType = QStringLiteral( "well" );
            l.entityId = QStringLiteral( "well-4" );
            l.assetId = a.id;
            l.role = role;
            l.isPrimary = true;
            l.unresolved = false;
            return f.catalog.addLink( l ) && writeText( f.dir.filePath( v.path ), text );
        };
        // A4 分层：D61 只有 MD（TVD/Time 皆 -99999 → tvd=NaN，md=2050）。
        QVERIFY( addFile( QStringLiteral( "ast-4" ), QStringLiteral( "well_stratification" ),
                          QStringLiteral( "ver-4" ), QStringLiteral( "DC4.dat" ),
                          QStringLiteral( "#WellTops File From SMI\n"
                                          "A4 D61 2050.000 600.000 400.000 -99999.000 -99999.000 -99999.000\n" ),
                          QStringLiteral( "tops" ) ) );
        // A4 时深：TVD 列全哨兵，MD 列 2000/2100 单调增 → 2050 → 2050ms。
        QVERIFY( addFile( QStringLiteral( "ast-5" ), QStringLiteral( "time_depth" ),
                          QStringLiteral( "ver-5" ), QStringLiteral( "A4_TD.dat" ),
                          QStringLiteral( "#TimeDepth File From SMI\n# Well : A4\n"
                                          "#TIME            TVDSS            TVD            MD\n"
                                          "2000.000          -2000.000        -99999.000       2000.000\n"
                                          "2100.000          -2100.000        -99999.000       2100.000\n" ),
                          QStringLiteral( "time_depth" ) ) );

        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );

        const QList<TimeResidualRow> rows =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 10.0 );
        const TimeResidualRow *a4 = nullptr;
        for ( const TimeResidualRow &r : rows )
            if ( r.wellId == QLatin1String( "well-4" ) )
                a4 = &r;
        QVERIFY( a4 );
        QCOMPARE( a4->status, TimeResidualRow::Status::Exceeds );
        // MD 列插值 2050ms vs 栅格 2001.4ms（col6,row4）→ +48.6ms
        QVERIFY( qAbs( a4->timeMs - 2050.0 ) < 0.5 );
        QVERIFY( a4->residualMs > 0.0 );
    }

    // 布局导出：facies.<h> 由相面链声明（本链不再产相面）；声明后即可导出。
    void exportPdfWritesD61Map()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString tif = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );
        QVERIFY( writeConstraintPolygon( f.dir.filePath( QStringLiteral( "cons.geojson" ) ) ) );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        QString err;
        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        LayerDeclaration consDecl;
        consDecl.layerId = QStringLiteral( "constraints.D61" );
        consDecl.horizon = QStringLiteral( "D61" );
        consDecl.type = QStringLiteral( "vector" );
        consDecl.source = f.dir.filePath( QStringLiteral( "cons.geojson" ) );
        consDecl.group = QStringLiteral( "02_Constraints" );
        QVERIFY2( f.layers.declare( consDecl, &err ), qPrintable( err ) );

        // 等厚链不再产相面（§5C）；相多边形声明走 ComposePage 的转面路径，
        // 这里直接声明一个多边形层代替，导出走 mapexport 原生管线。
        LayerDeclaration faciesDecl;
        faciesDecl.layerId = QStringLiteral( "facies.D61" );
        faciesDecl.horizon = QStringLiteral( "D61" );
        faciesDecl.type = QStringLiteral( "vector" );
        faciesDecl.source = f.dir.filePath( QStringLiteral( "cons.geojson" ) );
        faciesDecl.group = QStringLiteral( "05_PaleoMap" );
        QVERIFY2( f.layers.declare( faciesDecl, &err ), qPrintable( err ) );

        // 厚度栅格声明（chain 产出的 factor.<h>.idw 契约 id）——本测试只验
        // 布局结构，不跑编图链。
        const QString thickTif = makeConstRaster(
            f.dir.filePath( QStringLiteral( "thickness.tif" ) ), 36.0f );
        QVERIFY( !thickTif.isEmpty() );
        LayerDeclaration thickDecl;
        thickDecl.layerId = QStringLiteral( "factor.D61.idw" );
        thickDecl.horizon = QStringLiteral( "D61" );
        thickDecl.type = QStringLiteral( "raster" );
        thickDecl.source = thickTif;
        thickDecl.group = QStringLiteral( "04_SingleFactor" );
        thickDecl.title = QStringLiteral( "D61–D62 等厚（米）" );
        QVERIFY2( f.layers.declare( thickDecl, &err ), qPrintable( err ) );

        // 阶段E — 布局结构（§162/§233）：标题「D61 厚度」+ 地图项（厚度
        // 栅格垫底，井位/相面在上）+ 米制图例 + 比例尺 + 指北针 + CRS 说明。
        {
            QString layoutErr;
            QgsPrintLayout *layout =
                buildHorizonMapLayout( &f.layers, &f.projectSvc, QStringLiteral( "D61" ),
                                       &layoutErr );
            QVERIFY2( layout != nullptr, qPrintable( layoutErr ) );
            QCOMPARE( layout->project(), f.projectSvc.project() ); // 服务工程，非单例

            auto *title = qobject_cast<QgsLayoutItemLabel *>( layout->itemById( "title" ) );
            QVERIFY2( title != nullptr, "title item missing" );
            QCOMPARE( title->text(), QStringLiteral( "D61 厚度" ) );
            QVERIFY( layout->itemById( QStringLiteral( "legend" ) ) != nullptr );
            QVERIFY( layout->itemById( QStringLiteral( "scalebar" ) ) != nullptr );
            QVERIFY( layout->itemById( QStringLiteral( "northArrow" ) ) != nullptr );
            auto *crs =
                qobject_cast<QgsLayoutItemLabel *>( layout->itemById( "crsCaption" ) );
            QVERIFY2( crs != nullptr, "crs caption missing" );
            QCOMPARE( crs->text(), QStringLiteral( "工程坐标 · 米 · 未投影" ) );
            auto *map = qobject_cast<QgsLayoutItemMap *>( layout->itemById( "map" ) );
            QVERIFY2( map != nullptr, "map item missing" );
            QVERIFY( !map->layers().isEmpty() );
            delete layout;
        }

        const QString pdf = f.dir.filePath( QStringLiteral( "D61_map.pdf" ) );
        const QString out =
            exportHorizonMapPdf( &f.layers, &f.projectSvc, QStringLiteral( "D61" ), pdf, &err );
        QVERIFY2( !out.isEmpty(), qPrintable( err ) );
        QVERIFY( QFile::exists( out ) );
        QFile pf( out );
        QVERIFY( pf.open( QIODevice::ReadOnly ) );
        QVERIFY( pf.size() > 0 );
        QCOMPARE( pf.read( 4 ), QByteArray( "%PDF", 4 ) );
        pf.close();

        // 无厚度栅格（factor.<h>.idw）声明 → 干净失败。
        QVERIFY( exportHorizonMapPdf( &f.layers, &f.projectSvc, QStringLiteral( "D62" ),
                                      f.dir.filePath( QStringLiteral( "x.pdf" ) ), &err )
                     .isEmpty() );
        QVERIFY( !err.isEmpty() );

        // 阶段E — PDF → catalog OUTPUT 受管资产：SHA-256 与受管路径带出；
        // 同 SHA-256 重复登记走 dedup 复用，不新增资产。
        {
            QString sha, managed, regErr;
            const QString assetId = registerMapPdfAsset(
                &f.catalog, f.dir.path(), out, &sha, &managed, &regErr );
            QVERIFY2( !assetId.isEmpty(), qPrintable( regErr ) );
            QVERIFY( !sha.isEmpty() );
            QVERIFY( !managed.isEmpty() && QFile::exists( managed ) );
            QVERIFY( managed.contains( QStringLiteral( "artifacts/output/" ) ) );
            QVERIFY( !QFileInfo( managed ).isWritable() ); // 受管副本只读
            // fixture 手工种过 ast-1.. 号（assetById 返回首个匹配，id 可能与
            // 运行时分配的号相撞）—— 断言按 id+format 扫一遍资产表。
            bool pdfAssetFound = false;
            for ( const CatalogAsset &a : f.catalog.assets() )
              if ( a.id == assetId && a.format == QStringLiteral( "pdf" ) )
                pdfAssetFound = true;
            QVERIFY2( pdfAssetFound, "registered OUTPUT asset missing from catalog" );
            const CatalogVersion ver = f.catalog.versionBySha256( sha );
            QVERIFY( !ver.id.isEmpty() );
            QCOMPARE( ver.stage, QStringLiteral( "OUTPUT" ) );
            QCOMPARE( ver.assetId, assetId );

            const int assetCount = f.catalog.assets().size();
            QCOMPARE( registerMapPdfAsset( &f.catalog, f.dir.path(), out ),
                      assetId ); // dedup：同文件再登记复用同资产
            QCOMPARE( f.catalog.assets().size(), assetCount );
        }
    }
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
        qFatal( "QgisRuntime::initialize failed" );
    TestMapping tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgisRuntime::shutdown();
    return rc;
}

#include "tst_mapping.moc"
