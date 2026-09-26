#include <QtTest>
#include <QDir>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include <gdal.h>
#include <cpl_conv.h>

#include <qgsproject.h>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/types.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/linkage/threewaylocator.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/projectdata.h"
#include "../src/ui/correlationpanel.h"
#include "../src/ui/pages/pagepanels.h"
#include "../src/ui/seismicpreviewpanel.h"
#include "../src/workflow/workflows.h"

// wave/mapping-pipeline 阶段C —「问题 → 三视图」（预览壳重排后地图/连井）：
// ValidatePage 双击残差问题 → locateRequested(layerId, wkt, payload) →
// ThreeWayLocator ① 记录/移地图到井点（payload well_x/well_y，画布为空时
// 只记目标）② 底栏切到「连井剖面」+ scrollToWellTop。地震面板不再参与
// ——底栏地震标签已移除，测线跳转走「在数据页看这条剖面」。

class TestThreeWay : public QObject
{
    Q_OBJECT

  private:
    static bool writeText( const QString &path, const QString &text )
    {
        QFile f( path );
        if ( !f.open( QIODevice::WriteOnly ) )
            return false;
        f.write( text.toUtf8() );
        f.close();
        return QFile::exists( path );
    }

    static QString makeRaster( const QString &path, bool withInlineMeta = true )
    {
        GDALAllRegister();
        GDALDriverH drv = GDALGetDriverByName( "GTiff" );
        GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), 10, 8, 1, GDT_Float32, nullptr );
        if ( !ds )
            return QString();
        const double gt[6] = { 0.0, 100.0, 0.0, 800.0, 0.0, -100.0 };
        GDALSetGeoTransform( ds, const_cast<double *>( gt ) );
        if ( withInlineMeta )
        {
            GDALSetMetadataItem( ds, "PALEO_INLINE_MIN", "1315", nullptr );
            GDALSetMetadataItem( ds, "PALEO_INLINE_MAX", "1725", nullptr );
        }
        QVector<float> px;
        for ( int y = 0; y < 8; ++y )
            for ( int x = 0; x < 10; ++x )
                px.append( 2000.0f + 0.1f * x + 0.2f * y );
        GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
        GDALSetRasterNoDataValue( band, -9999.0 );
        const CPLErr err = GDALRasterIO( band, GF_Write, 0, 0, 10, 8,
                                         const_cast<float *>( px.constData() ), 10, 8,
                                         GDT_Float32, 0, 0 );
        GDALClose( ds );
        return err == CE_None ? path : QString();
    }

    // 井 well-1「A1」(300,400)：D61 分层（TVD 2125）+ TD（TVD 1800..2400 →
    // 2000..2400ms）→ 残差 +215ms 超限、inline=1438。
    static bool buildWellCatalog( DataCatalog &catalog, const QDir &d )
    {
        if ( !d.mkpath( QStringLiteral( "raw/ast-1/ver-1" ) ) )
            return false;
        if ( !d.mkpath( QStringLiteral( "raw/ast-2/ver-2" ) ) )
            return false;
        CatalogEntity well;
        well.id = QStringLiteral( "well-1" );
        well.entityType = QStringLiteral( "well" );
        well.name = QStringLiteral( "A1" );
        well.surfaceX = 300.0;
        well.surfaceY = 400.0;
        well.hasSurface = true;
        well.coordinateStatus = QStringLiteral( "untransformed" );
        if ( !catalog.addEntity( well ) )
            return false;

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
        topsVer.path = QStringLiteral( "raw/ast-1/ver-1/DC.dat" );
        if ( !catalog.addVersion( topsVer ) )
            return false;
        if ( !writeText( d.filePath( topsVer.path ), QStringLiteral(
            "#WellTops File From SMI\n"
            "A1           D61          2148.000     300.000      400.000      -2125.000    2125.000     -99999.000\n" ) ) )
            return false;
        EntityAssetLink topsLink;
        topsLink.entityType = QStringLiteral( "well" );
        topsLink.entityId = well.id;
        topsLink.assetId = topsAsset.id;
        topsLink.role = QStringLiteral( "tops" );
        topsLink.isPrimary = true;
        if ( !catalog.addLink( topsLink ) )
            return false;

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
        tdVer.path = QStringLiteral( "raw/ast-2/ver-2/A1_TD.dat" );
        if ( !catalog.addVersion( tdVer ) )
            return false;
        if ( !writeText( d.filePath( tdVer.path ), QStringLiteral(
            "#TimeDepth File From SMI\n# Well : A1\n"
            "2000.000          -1800.000         1800.000         1800.000\n"
            "2200.000          -2100.000         2100.000         2100.000\n"
            "2400.000          -2400.000         2400.000         2400.000\n" ) ) )
            return false;
        EntityAssetLink tdLink;
        tdLink.entityType = QStringLiteral( "well" );
        tdLink.entityId = well.id;
        tdLink.assetId = tdAsset.id;
        tdLink.role = QStringLiteral( "time_depth" );
        tdLink.isPrimary = true;
        return catalog.addLink( tdLink );
    }

  private slots:

    void cleanup()
    {
        QgsProject::instance()->removeAllMapLayers();
    }

    // 双击残差问题 → locateRequested 带机器载荷；ThreeWayLocator 驱动两面板。
    void issueDoubleClickedDrivesThreeViews()
    {
        QTemporaryDir dir;
        QDir d( dir.path() );
        DataCatalog catalog;
        QVERIFY( catalog.open( dir.path() ) );
        QVERIFY( buildWellCatalog( catalog, d ) );
        const QString tif = makeRaster( d.filePath( QStringLiteral( "d61.tif" ) ) );
        QVERIFY( !tif.isEmpty() );

        QgisProjectService projectSvc;
        QVERIFY( projectSvc.createProject( d.filePath( QStringLiteral( "proj.qgz" ) ) ) );
        PaleoProjectStore store;
        LayerManifest manifest{ d.filePath( QStringLiteral( "project.sqlite" ) ) };
        QVERIFY( manifest.open() );
        QgisLayerService layers{ &projectSvc, &manifest };

        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( layers.declare( rasterDecl, &err ), qPrintable( err ) );

        ProjectDataFacade pd;
        pd.setCatalog( &catalog, dir.path() );
        pd.setManifest( &manifest );

        ValidationWorkflow wf( &layers, &store );
        wf.setProjectData( &pd );
        ValidatePage page( &wf );

        // Panels: real widgets, offscreen.
        SelectionContext ctx;
        // 预览壳重排：底栏 QTabWidget 挂连井面板（地震标签已移除）。
        // bottomTabs 先声明——addTab 会把面板 reparent 进去，栈析构倒序保证
        // 面板先死、自动脱离父子表，tabs 析构时不再二次 delete。
        QTabWidget bottomTabs;
        WellCorrelationPanel wellPanel( &ctx );
        wellPanel.setWells( { { QStringLiteral( "well-1" ), QStringLiteral( "A1" ) } } );
        wellPanel.setManifestHorizons( { QStringLiteral( "D61" ) } );
        wellPanel.markers()->setWellDepth( QStringLiteral( "D61" ),
                                           QStringLiteral( "well-1" ), 2125.0f );
        // 画布为空 → zoomToPoint 跳过但井点目标仍被记录（lastMapPoint）。
        bottomTabs.addTab( &wellPanel, QStringLiteral( "连井剖面" ) );

        ThreeWayLocator locator( nullptr, &wellPanel, &bottomTabs );
        locator.attach( &page );

        QSignalSpy spy( &page, &ValidatePage::locateRequested );
        page.populate();
        QVERIFY( spy.isEmpty() );

        // Find the TIME_RESIDUAL row and double-click it.
        auto *table = page.findChild<QTableWidget *>( QStringLiteral( "issueTable" ) );
        QVERIFY( table != nullptr );
        int issueRow = -1;
        for ( int r = 0; r < table->rowCount(); ++r )
        {
            auto *code = table->item( r, 1 );
            if ( code && code->text() == QLatin1String( "TIME_RESIDUAL" ) )
                issueRow = r;
        }
        QVERIFY2( issueRow >= 0, "no TIME_RESIDUAL row in table" );
        // Repo pattern (tst_panels): invoke the signal with the real item.
        QVERIFY( QMetaObject::invokeMethod(
            table, "itemDoubleClicked",
            Q_ARG( QTableWidgetItem *, table->item( issueRow, 0 ) ) ) );

        QCOMPARE( spy.count(), 1 );
        const QVariantList args = spy.at( 0 );
        QCOMPARE( args.at( 0 ).toString(), QStringLiteral( "horizon.D61.derived" ) );
        QVERIFY( args.at( 1 ).toString().startsWith( QStringLiteral( "POINT(300" ) ) );
        const QVariantMap payload = args.at( 2 ).toMap();
        QCOMPARE( payload.value( QStringLiteral( "wellId" ) ).toString(), QStringLiteral( "well-1" ) );
        QCOMPARE( payload.value( QStringLiteral( "horizon" ) ).toString(), QStringLiteral( "D61" ) );
        QCOMPARE( payload.value( QStringLiteral( "inline" ) ).toInt(), 1438 );

        // ① 地图点目标已记录（payload well_x=300, well_y=400——画布为空只
        // 记目标不缩放）；② 底栏已切到连井面板且滚到该井该分层。
        QVERIFY( locator.hasMapPoint() );
        QCOMPARE( locator.lastMapPoint(), QPointF( 300.0, 400.0 ) );
        QCOMPARE( bottomTabs.currentWidget(), static_cast<QWidget *>( &wellPanel ) );
        QCOMPARE( wellPanel.lastScrollWell(), QStringLiteral( "well-1" ) );
        QCOMPARE( wellPanel.lastScrollHorizon(), QStringLiteral( "D61" ) );

        // Non-residual issue (no wellId/inline) → only recorded targets stay
        // untouched: direct locate() with empty payload must not move panels.
        const QString keepWell = wellPanel.lastScrollWell();
        const QPointF keepPoint = locator.lastMapPoint();
        locator.locate( QStringLiteral( "some.layer" ), QString(), QVariantMap() );
        QCOMPARE( wellPanel.lastScrollWell(), keepWell );
        QCOMPARE( locator.lastMapPoint(), keepPoint );

        // ---- T24：残差表行与问题行同一条 locateRequested --------------------
        // 双击 D61 残差表的井行 → 地图移到井点 + 连井滚到分层（同一载荷）。
        auto *resTable = page.findChild<QTableWidget *>(
            QStringLiteral( "residualTable" ) );
        QVERIFY( resTable != nullptr );
        QCOMPARE( resTable->rowCount(), 1 ); // 井 A1 的逐井残差行
        spy.clear();
        QVERIFY( QMetaObject::invokeMethod(
            resTable, "itemDoubleClicked",
            Q_ARG( QTableWidgetItem *, resTable->item( 0, 0 ) ) ) );
        QCOMPARE( spy.count(), 1 );
        const QVariantList rargs = spy.at( 0 );
        QCOMPARE( rargs.at( 0 ).toString(),
                  QStringLiteral( "horizon.D61.derived" ) );
        QVERIFY( rargs.at( 1 ).toString().startsWith(
            QStringLiteral( "POINT(300" ) ) );
        const QVariantMap rpayload = rargs.at( 2 ).toMap();
        QCOMPARE( rpayload.value( QStringLiteral( "wellId" ) ).toString(),
                  QStringLiteral( "well-1" ) );
        QCOMPARE( rpayload.value( QStringLiteral( "horizon" ) ).toString(),
                  QStringLiteral( "D61" ) );
        QCOMPARE( rpayload.value( QStringLiteral( "inline" ) ).toInt(), 1438 );
        QVERIFY( rpayload.contains( QStringLiteral( "time_ms" ) ) );
        QCOMPARE( locator.lastMapPoint(), QPointF( 300.0, 400.0 ) );
        QCOMPARE( wellPanel.lastScrollWell(), QStringLiteral( "well-1" ) );
        QCOMPARE( wellPanel.lastScrollHorizon(), QStringLiteral( "D61" ) );

        // 选中残差行 → inline 可解 →「在数据页看这条剖面」可用；点击把整份
        // 载荷发给 shell（inline=1438 + time_ms）。
        auto *openSection = page.findChild<QPushButton *>(
            QStringLiteral( "openSeismicSectionButton" ) );
        QVERIFY( openSection != nullptr );
        QVERIFY( !openSection->isEnabled() ); // 无选中 → 不亮
        QSignalSpy sectionSpy( &page, &ValidatePage::seismicSectionRequested );
        resTable->selectRow( 0 );
        QVERIFY( openSection->isEnabled() );
        openSection->click();
        QCOMPARE( sectionSpy.count(), 1 );
        const QVariantMap sp = sectionSpy.at( 0 ).at( 0 ).toMap();
        QCOMPARE( sp.value( QStringLiteral( "inline" ) ).toInt(), 1438 );
        QCOMPARE( sp.value( QStringLiteral( "wellId" ) ).toString(),
                  QStringLiteral( "well-1" ) );

        // 问题行同样能武装这颗钮（两张表共用）：选中 TIME_RESIDUAL 行后
        // 载荷换回问题行那份。
        table->selectRow( issueRow );
        QVERIFY( openSection->isEnabled() );
        openSection->click();
        QCOMPARE( sectionSpy.count(), 2 );
        QCOMPARE( sectionSpy.at( 1 ).at( 0 ).toMap()
                      .value( QStringLiteral( "wellId" ) ).toString(),
                  QStringLiteral( "well-1" ) );
    }

    // T24 反面：栅格没有 PALEO_INLINE_* → 残差行 inline=-1，选中残差行也
    // 不亮「在数据页看这条剖面」——测线号解不出来就不放人跳页。
    void residualRowWithoutInlineKeepsSectionDisabled()
    {
        QTemporaryDir dir;
        QDir d( dir.path() );
        DataCatalog catalog;
        QVERIFY( catalog.open( dir.path() ) );
        QVERIFY( buildWellCatalog( catalog, d ) );
        const QString tif =
            makeRaster( d.filePath( QStringLiteral( "d61.tif" ) ), /*withInlineMeta=*/false );
        QVERIFY( !tif.isEmpty() );

        QgisProjectService projectSvc;
        QVERIFY( projectSvc.createProject( d.filePath( QStringLiteral( "proj.qgz" ) ) ) );
        PaleoProjectStore store;
        LayerManifest manifest{ d.filePath( QStringLiteral( "project.sqlite" ) ) };
        QVERIFY( manifest.open() );
        QgisLayerService layers{ &projectSvc, &manifest };

        LayerDeclaration rasterDecl;
        rasterDecl.layerId = QStringLiteral( "horizon.D61.derived" );
        rasterDecl.horizon = QStringLiteral( "D61" );
        rasterDecl.type = QStringLiteral( "raster" );
        rasterDecl.source = tif;
        rasterDecl.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( layers.declare( rasterDecl, &err ), qPrintable( err ) );

        ProjectDataFacade pd;
        pd.setCatalog( &catalog, dir.path() );
        pd.setManifest( &manifest );

        ValidationWorkflow wf( &layers, &store );
        wf.setProjectData( &pd );
        ValidatePage page( &wf );
        page.populate();

        auto *resTable = page.findChild<QTableWidget *>(
            QStringLiteral( "residualTable" ) );
        QVERIFY( resTable != nullptr );
        QCOMPARE( resTable->rowCount(), 1 );
        auto *openSection = page.findChild<QPushButton *>(
            QStringLiteral( "openSeismicSectionButton" ) );
        QVERIFY( openSection != nullptr );
        resTable->selectRow( 0 );
        QVERIFY( !openSection->isEnabled() ); // inline 解不出 → 钮不亮
        QSignalSpy sectionSpy( &page, &ValidatePage::seismicSectionRequested );
        openSection->click();
        QCOMPARE( sectionSpy.count(), 0 );
        // 但定位载荷仍在——双击照常联地图/连井（inline 缺只是没剖面）。
        const QVariantMap p = resTable->item( 0, 0 )->data( Qt::UserRole + 2 ).toMap();
        QCOMPARE( p.value( QStringLiteral( "inline" ) ).toInt(), -1 );
        QCOMPARE( p.value( QStringLiteral( "wellId" ) ).toString(),
                  QStringLiteral( "well-1" ) );
    }

    // 面板新 API 的独立行为：未知井只记目标不滚动；已知井滚到 pick 线。
    void panelScrollApis()
    {
        SelectionContext ctx;
        WellCorrelationPanel wellPanel( &ctx );
        SeismicPreviewPanel seismicPanel( nullptr );

        // Empty panel: scroll target still recorded.
        wellPanel.scrollToWellTop( QStringLiteral( "well-X" ), QStringLiteral( "D61" ) );
        QCOMPARE( wellPanel.lastScrollWell(), QStringLiteral( "well-X" ) );
        QCOMPARE( wellPanel.lastScrollHorizon(), QStringLiteral( "D61" ) );

        seismicPanel.gotoLine( 1500, 2050.0 );
        QCOMPARE( seismicPanel.lastGotoInline(), 1500 );
        QVERIFY( qAbs( seismicPanel.lastGotoTimeMs() - 2050.0 ) < 0.01 );

        // gotoLine without a loaded section keeps the target, no crash.
        seismicPanel.gotoLine( -1, qQNaN() );
        QCOMPARE( seismicPanel.lastGotoInline(), -1 );
        QVERIFY( qIsNaN( seismicPanel.lastGotoTimeMs() ) );
    }
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
        qFatal( "QgisRuntime::initialize failed" );
    TestThreeWay tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgisRuntime::shutdown();
    return rc;
}

#include "tst_threeway.moc"
