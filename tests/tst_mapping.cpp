#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <gdal.h>
#include <cpl_conv.h>

#include <qgsproject.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/types.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/projectdata.h"
#include "../src/workflow/mappingworkflow.h"
#include "../src/workflow/mapexport.h"
#include "../src/workflow/workflows.h"

// wave/mapping-pipeline 阶段C — 只编 D61：
//   tops → TVD 厚度（缺基面分层跳过计数）→ 约束 IDW（测网像元、凸包裁剪）
//   → paleo:paleo_facies_polygonize → facies.D61 声明。
// 全链走真实服务栈（同 tst_workflows 的 Fixture 形态），fixture 是合成
// catalog.json + 小时间栅格；wave/data-foundation 合并后接口不变。

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
            return true;
        }
    };

    // 3 wells inside a 0..1000 x 0..800 local-meter grid: A1 + A3 carry both
    // D61/D62 picks, A2 lacks D62 (thickness skip case), A3 has no TD link
    // (residual「无时深表」case). A1's TD-implied time differs from the raster
    // by ~215ms (residual case); A2's matches to <1ms.
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
             !addWell( QStringLiteral( "well-3" ), QStringLiteral( "A3" ), 700.0, 600.0 ) )
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
            "A2           D61          2050.000     500.000      300.000      -2030.000    2030.000     -99999.000\n"
            "A3           D61          2400.000     700.000      600.000      -2380.000    2380.000     -99999.000\n"
            "A3           D62          2450.000     700.000      600.000      -2430.000    2430.000     -99999.000\n" );
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
                                    QStringLiteral( "well-3" ) } )
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
                          "2200.000          -2228.500         2228.500         2228.500\n" ) );
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

    // tops → 厚度：A1=55m、A3=50m；A2 缺 D62 分层被跳过并计数。
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
        QCOMPARE( pts.size(), 2 );
        QCOMPARE( skipped, 1 );

        QCOMPARE( pts.at( 0 ).wellId, QStringLiteral( "well-1" ) );
        QCOMPARE( pts.at( 0 ).wellName, QStringLiteral( "A1" ) );
        QVERIFY( qAbs( pts.at( 0 ).x - 300.0 ) < 0.01 );
        QVERIFY( qAbs( pts.at( 0 ).y - 400.0 ) < 0.01 );
        QVERIFY( qAbs( pts.at( 0 ).thickness - 55.0 ) < 0.01 );

        QCOMPARE( pts.at( 1 ).wellId, QStringLiteral( "well-3" ) );
        QVERIFY( qAbs( pts.at( 1 ).thickness - 50.0 ) < 0.01 );

        // Facade 未绑定 → 有错误文案，返回空。
        MappingWorkflow orphan( &f.constraint, &f.compose, &f.layers );
        const QVector<ThicknessPoint> none = orphan.computeThickness(
            QStringLiteral( "D61" ), QStringLiteral( "D62" ), &skipped, &err );
        QCOMPARE( none.size(), 0 );
        QVERIFY( !err.isEmpty() );
    }

    // 厚度 → 约束 IDW（测网像元 + 凸包裁剪）→ facies.D61 声明 + 信号。
    void thicknessChainProducesFactorAndFacies()
    {
        Fixture f;
        QVERIFY( f.init() );
        const QString tif = makeRaster( f.dir.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );
        QVERIFY( writeConstraintPolygon( f.dir.filePath( QStringLiteral( "cons.geojson" ) ) ) );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.pd.setManifest( &f.manifest );
        f.mapping.setProjectData( &f.pd );

        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );

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
        QCOMPARE( doneSpy.at( 0 ).at( 1 ).toString(), QStringLiteral( "facies.D61" ) );

        // --- factor raster: survey cell size, convex-hull clip -------------
        const LayerDeclaration *factor = findDecl( f.layers, QStringLiteral( "factor.D61.idw" ) );
        QVERIFY2( factor != nullptr, "factor.D61.idw not declared" );
        QCOMPARE( factor->type, QStringLiteral( "raster" ) );
        QVERIFY( QFile::exists( factor->source ) );

        GDALDatasetH ds = GDALOpen( factor->source.toUtf8().constData(), GA_ReadOnly );
        QVERIFY( ds != nullptr );
        double gt[6] = { 0, 0, 0, 0, 0, 0 };
        GDALGetGeoTransform( ds, gt );
        GDALClose( ds );
        QCOMPARE( gt[1], 100.0 );                    // 测网像元尺度
        QCOMPARE( qAbs( gt[5] ), 100.0 );

        // Inside the constraint hull (left half) → finite thickness-ish value.
        const double inside = sampleAt( factor->source, 300.0, 400.0 );
        QVERIFY2( !qIsNaN( inside ), "inside-hull cell is nodata" );
        QVERIFY( inside > 0.0 && inside < 200.0 );
        // Same grid, right of the hull → nodata (凸包裁剪).
        const double outside = sampleAt( factor->source, 700.0, 400.0 );
        QVERIFY2( qIsNaN( outside ), "outside-hull cell is not clipped" );
        delete factor;

        // --- facies polygons: declared vector on disk -----------------------
        const LayerDeclaration *facies = findDecl( f.layers, QStringLiteral( "facies.D61" ) );
        QVERIFY2( facies != nullptr, "facies.D61 not declared" );
        QCOMPARE( facies->type, QStringLiteral( "vector" ) );
        QCOMPARE( facies->horizon, QStringLiteral( "D61" ) );
        QVERIFY( QFile::exists( facies->source.section( QLatin1Char( '|' ), 0, 0 ) ) );
        QVERIFY( f.layers.instantiate( QStringLiteral( "facies.D61" ) ) != nullptr );
        delete facies;

        // Thickness points layer stays declared for the record.
        QVERIFY( findDecl( f.layers, QStringLiteral( "wells.thickness.D61" ) ) != nullptr );
    }

    // 无时间栅格 / 无厚度点 → 干净失败，不发 chainDone。
    void chainErrorPaths()
    {
        Fixture f;
        QVERIFY( f.init() );
        f.pd.setCatalog( &f.catalog, f.dir.path() );
        f.mapping.setProjectData( &f.pd );

        QSignalSpy doneSpy( &f.mapping, &MappingWorkflow::chainDone );
        QSignalSpy failSpy( &f.mapping, &MappingWorkflow::chainFailed );

        QString err;
        QVERIFY( !f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ) );
        QVERIFY( !err.isEmpty() );
        QVERIFY( err.contains( QStringLiteral( "时间栅格" ) ) );
        QVERIFY( !f.mapping.runThicknessChain( QStringLiteral( "D62" ), &err ) );
        QVERIFY( !err.isEmpty() ); // D62 无分层 → 无厚度点
        QCOMPARE( doneSpy.count(), 0 );
        QCOMPARE( failSpy.count(), 2 );
    }

    // 时间残差：TD 插值时间 vs D61 栅格在井位处的采样（最近像元）。
    // A1 残差 ~215ms 超阈值成问题；A2 残差 0 不成问题；A3 无 TD 记「无时深表」。
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

        // 阈值默认半样点间隔（2ms 采样 → 1ms 起评）。
        const QList<ValidationIssue> issues =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 1.0 );

        int residualCount = 0, noTdCount = 0;
        for ( const ValidationIssue &v : issues )
        {
            if ( v.code == QStringLiteral( "TIME_RESIDUAL" ) )
            {
                ++residualCount;
                QCOMPARE( v.wellId, QStringLiteral( "well-1" ) );
                QCOMPARE( v.horizon, QStringLiteral( "D61" ) );
                QCOMPARE( v.layerId, QStringLiteral( "horizon.D61.derived" ) );
                QCOMPARE( v.severity, ValidationIssue::Warning );
                QVERIFY( v.wktLocation.contains( QStringLiteral( "POINT(300" ) ) );
                // TD 插值 2216.67ms vs 栅格 2001.1ms（col3,row4）→ ~215.6ms
                QVERIFY( qAbs( v.details.value( QStringLiteral( "time_ms" ) ).toDouble() - 2216.67 ) < 0.5 );
                QVERIFY( qAbs( v.details.value( QStringLiteral( "raster_ms" ) ).toDouble() - 2001.1 ) < 0.5 );
                QVERIFY( qAbs( v.details.value( QStringLiteral( "residual_ms" ) ).toDouble() - 215.57 ) < 1.0 );
                QCOMPARE( v.details.value( QStringLiteral( "inline" ) ).toInt(), 1438 );
                QVERIFY( v.message.contains( QStringLiteral( "A1" ) ) );
            }
            else if ( v.code == QStringLiteral( "NO_TD_TABLE" ) )
            {
                ++noTdCount;
                QCOMPARE( v.wellId, QStringLiteral( "well-3" ) );
                QVERIFY( v.message.contains( QStringLiteral( "无时深表" ) ) );
            }
            else
            {
                QFAIL( qPrintable( QStringLiteral( "unexpected issue code: %1" ).arg( v.code ) ) );
            }
        }
        QCOMPARE( residualCount, 1 );
        QCOMPARE( noTdCount, 1 );

        // 阈值放大 → A1 的 215ms 不再成问题，只剩无时深表。
        const QList<ValidationIssue> loose =
            computeTimeResiduals( &f.pd, QStringLiteral( "D61" ), 500.0 );
        QCOMPARE( loose.size(), 1 );
        QCOMPARE( loose.at( 0 ).code, QStringLiteral( "NO_TD_TABLE" ) );

        // ValidationWorkflow 集成：绑定门面后 validate() 带上残差检查。
        ValidationWorkflow vw( &f.layers, &f.store );
        vw.setProjectData( &f.pd );
        const QList<ValidationIssue> all = vw.validate();
        bool sawResidual = false, sawNoTd = false;
        for ( const ValidationIssue &v : all )
        {
            if ( v.code == QLatin1String( "TIME_RESIDUAL" ) )
                sawResidual = true;
            if ( v.code == QLatin1String( "NO_TD_TABLE" ) )
                sawNoTd = true;
        }
        QVERIFY( sawResidual );
        QVERIFY( sawNoTd );

        // 序列化往返保留新增字段。
        const ValidationIssue roundtrip = ValidationIssue::fromMap( issues.at( 0 ).toMap() );
        QCOMPARE( roundtrip.wellId, QStringLiteral( "well-1" ) );
        QCOMPARE( roundtrip.details.value( QStringLiteral( "inline" ) ).toInt(), 1438 );
    }

    // 布局导出：跑完链路后导一张含井位 + 相多边形的 D61 PDF。
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

        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( f.layers.declare( rasterDecl, &err ), qPrintable( err ) );
        LayerDeclaration consDecl;
        consDecl.layerId = QStringLiteral( "constraints.D61" );
        consDecl.horizon = QStringLiteral( "D61" );
        consDecl.type = QStringLiteral( "vector" );
        consDecl.source = f.dir.filePath( QStringLiteral( "cons.geojson" ) );
        consDecl.group = QStringLiteral( "02_Constraints" );
        QVERIFY2( f.layers.declare( consDecl, &err ), qPrintable( err ) );

        QVERIFY2( f.mapping.runThicknessChain( QStringLiteral( "D61" ), &err ), qPrintable( err ) );

        const QString pdf = f.dir.filePath( QStringLiteral( "D61_map.pdf" ) );
        const QString out = exportHorizonMapPdf( &f.layers, QStringLiteral( "D61" ), pdf, &err );
        QVERIFY2( !out.isEmpty(), qPrintable( err ) );
        QVERIFY( QFile::exists( out ) );
        QFile pf( out );
        QVERIFY( pf.open( QIODevice::ReadOnly ) );
        QVERIFY( pf.size() > 0 );
        QCOMPARE( pf.read( 4 ), QByteArray( "%PDF", 4 ) );
        pf.close();

        // 无 facies.<h> 声明 → 干净失败。
        QVERIFY( exportHorizonMapPdf( &f.layers, QStringLiteral( "D62" ),
                                      f.dir.filePath( QStringLiteral( "x.pdf" ) ), &err )
                     .isEmpty() );
        QVERIFY( !err.isEmpty() );
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
