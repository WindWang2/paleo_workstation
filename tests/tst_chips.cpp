#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>

#include "../src/domain/mappinghorizons.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/horizonchipbar.h"

#include <qgsapplication.h>
#include <qgsvectorlayer.h>

// wave/mapping-pipeline 阶段E — 编图 chip 只列 8 个层序界面；
// 切换沿用 activeHorizon + 按层位懒加载；集合外的层位名字不进 chip。

class TestChips : public QObject
{
    Q_OBJECT

  private slots:

    void chipInterceptWhileEditingEmitsReasonAndRecovers();
    void fixedSetOfEightAndLazySwitch()
    {
        QTemporaryDir dir;
        QVERIFY( dir.isValid() );
        QgisProjectService projectSvc;
        QVERIFY( projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
        LayerManifest manifest{ dir.filePath( QStringLiteral( "m.sqlite" ) ) };
        QVERIFY( manifest.open() );
        QgisLayerService layers{ &projectSvc, &manifest };
        SelectionContext ctx;

        // D62 layers materialized first — the chip switch must release them.
        QFile geo( dir.filePath( QStringLiteral( "f.geojson" ) ) );
        QVERIFY( geo.open( QIODevice::WriteOnly ) );
        geo.write( "{\"type\":\"FeatureCollection\",\"features\":["
                   "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
                   "{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[1,0],[1,1],[0,0]]]}}]}" );
        geo.close();
        LayerDeclaration d62;
        d62.layerId = QStringLiteral( "facies.D62" );
        d62.horizon = QStringLiteral( "D62" );
        d62.type = QStringLiteral( "vector" );
        d62.source = dir.filePath( QStringLiteral( "f.geojson" ) );
        QVERIFY( manifest.upsert( d62 ) );
        QVERIFY( layers.instantiate( QStringLiteral( "facies.D62" ) ) != nullptr );
        QVERIFY( layers.isInstantiated( QStringLiteral( "facies.D62" ) ) );

        // 阶段E 可用性门：D61 chip 要有栅格声明才可点（E4）。
        LayerDeclaration d61;
        d61.layerId = QStringLiteral( "horizon.D61.derived" );
        d61.horizon = QStringLiteral( "D61" );
        d61.type = QStringLiteral( "raster" );
        d61.source = dir.filePath( QStringLiteral( "d61.tif" ) ); // 声明即可
        d61.group = QStringLiteral( "00_Horizon" );
        QString declErr;
        QVERIFY2( layers.declare( d61, &declErr ), qPrintable( declErr ) );

        HorizonChipBar bar( &ctx, &layers );
        QCOMPARE( bar.chipNames(), mappingHorizons() );
        QCOMPARE( bar.chipNames().size(), 8 );

        // 集合外的层位名（如井分层里的 D61x / E1）从未出现在 chip 里。
        QVERIFY( !bar.chipNames().contains( QStringLiteral( "D61x" ) ) );
        QVERIFY( !bar.chipNames().contains( QStringLiteral( "E1" ) ) );
        QVERIFY( bar.findChild<QToolButton *>( QStringLiteral( "chip_D61x" ) ) == nullptr );

        // 点击 D61 chip：activeHorizon 切换 + 懒加载（D62 实例被释放）。
        QSignalSpy horizonSpy( &ctx, &SelectionContext::activeHorizonChanged );
        auto *chip = bar.findChild<QToolButton *>( QStringLiteral( "chip_D61" ) );
        QVERIFY( chip != nullptr );
        chip->click();
        QCOMPARE( ctx.activeHorizon(), QStringLiteral( "D61" ) );
        QCOMPARE( layers.activeHorizon(), QStringLiteral( "D61" ) );
        QCOMPARE( horizonSpy.count(), 1 );
        QVERIFY( bar.isChipActive( QStringLiteral( "D61" ) ) );
        QVERIFY( !bar.isChipActive( QStringLiteral( "D62" ) ) );
        QVERIFY( !layers.isInstantiated( QStringLiteral( "facies.D62" ) ) );

        // 反向同步：外部（如 locator）切到 D62，chip 跟随高亮。
        ctx.setActiveHorizon( QStringLiteral( "D62" ) );
        QVERIFY( bar.isChipActive( QStringLiteral( "D62" ) ) );
        QVERIFY( !bar.isChipActive( QStringLiteral( "D61" ) ) );

        // 集合外层位激活 → 不点亮任何 chip，chip 列表不变。
        ctx.setActiveHorizon( QStringLiteral( "E1" ) );
        const QStringList before = bar.chipNames();
        for ( const QString &h : mappingHorizons() )
            QVERIFY( !bar.isChipActive( h ) );
        QCOMPARE( bar.chipNames(), before );
    }

    // 阶段E — 无栅格声明的层位 chip 禁用 + 固定原因 tooltip；
    // 声明补上（layerDeclared）后放闸、可选中，已点亮状态不受影响。
    void chipDisabledUntilRasterDeclared()
    {
        QTemporaryDir dir;
        QVERIFY( dir.isValid() );
        QgisProjectService projectSvc;
        QVERIFY( projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
        LayerManifest manifest{ dir.filePath( QStringLiteral( "m.sqlite" ) ) };
        QVERIFY( manifest.open() );
        QgisLayerService layers{ &projectSvc, &manifest };
        SelectionContext ctx;

        HorizonChipBar bar( &ctx, &layers );
        // 空清单 → 全部禁用 + 同一句 tooltip。
        for ( const QString &h : mappingHorizons() )
        {
            auto *chip = bar.findChild<QToolButton *>( QStringLiteral( "chip_%1" ).arg( h ) );
            QVERIFY2( chip != nullptr, qPrintable( h ) );
            QVERIFY2( !chip->isEnabled(), qPrintable( h ) );
            QCOMPARE( chip->toolTip(), QStringLiteral( "这一阶段还没有这个层位的栅格" ) );
        }
        // 禁用的 chip 点不动 —— 不改 activeHorizon。
        auto *d61 = bar.findChild<QToolButton *>( QStringLiteral( "chip_D61" ) );
        d61->click();
        QVERIFY( ctx.activeHorizon().isEmpty() );

        // 声明 D61 栅格（声明本身即可，文件物化与否不影响可用性）。
        LayerDeclaration d;
        d.layerId = QStringLiteral( "horizon.D61.derived" );
        d.horizon = QStringLiteral( "D61" );
        d.type = QStringLiteral( "raster" );
        d.source = dir.filePath( QStringLiteral( "d61.tif" ) );
        d.group = QStringLiteral( "00_Horizon" );
        QString err;
        QVERIFY2( layers.declare( d, &err ), qPrintable( err ) ); // emit layerDeclared → 重算

        QVERIFY( d61->isEnabled() );
        QVERIFY( d61->toolTip().isEmpty() );
        for ( const QString &h : mappingHorizons() )
        {
            if ( h == QLatin1String( "D61" ) )
                continue;
            auto *chip = bar.findChild<QToolButton *>( QStringLiteral( "chip_%1" ).arg( h ) );
            QVERIFY2( !chip->isEnabled(), qPrintable( h ) );
            QCOMPARE( chip->toolTip(), QStringLiteral( "这一阶段还没有这个层位的栅格" ) );
        }

        // 放闸的 chip 保留原有 activeHorizon 行为。
        d61->click();
        QCOMPARE( ctx.activeHorizon(), QStringLiteral( "D61" ) );
        QVERIFY( bar.isChipActive( QStringLiteral( "D61" ) ) );
    }
};

// mapping 主线3：编辑中拦截切换层位——不再静默，带文案 + 会话结束后恢复。
void TestChips::chipInterceptWhileEditingEmitsReasonAndRecovers()
{
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QgisProjectService projectSvc;
    QVERIFY( projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
    LayerManifest manifest{ dir.filePath( QStringLiteral( "m.sqlite" ) ) };
    QVERIFY( manifest.open() );
    QgisLayerService layers{ &projectSvc, &manifest };
    SelectionContext ctx;

    // D61 栅格声明（chip 可点门）+ 一个已实例化矢量层（D62）承载编辑会话。
    LayerDeclaration d61;
    d61.layerId = QStringLiteral( "horizon.D61.derived" );
    d61.horizon = QStringLiteral( "D61" );
    d61.type = QStringLiteral( "raster" );
    d61.source = dir.filePath( QStringLiteral( "d61.tif" ) );
    QString err;
    QVERIFY2( layers.declare( d61, &err ), qPrintable( err ) );

    QFile geo( dir.filePath( QStringLiteral( "f.geojson" ) ) );
    QVERIFY( geo.open( QIODevice::WriteOnly ) );
    geo.write( "{\"type\":\"FeatureCollection\",\"features\":["
               "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
               "{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[1,0],[1,1],[0,0]]]}}]}" );
    geo.close();
    LayerDeclaration d62;
    d62.layerId = QStringLiteral( "facies.D62" );
    d62.horizon = QStringLiteral( "D62" );
    d62.type = QStringLiteral( "vector" );
    d62.source = dir.filePath( QStringLiteral( "f.geojson" ) );
    QVERIFY2( layers.declare( d62, &err ), qPrintable( err ) );
    QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>(
        layers.instantiate( QStringLiteral( "facies.D62" ) ) );
    QVERIFY( vl != nullptr );
    layers.setActiveHorizon( QStringLiteral( "D62" ) );

    HorizonChipBar bar( &ctx, &layers );
    ctx.setActiveHorizon( QStringLiteral( "D62" ) );
    QVERIFY( bar.isChipActive( QStringLiteral( "D62" ) ) );

    // 编辑会话开启 → 点 D61：拦截 + 文案（含编辑层名），activeHorizon 不变。
    QVERIFY( vl->startEditing() );
    QSignalSpy refuseSpy( &bar, &HorizonChipBar::horizonSwitchRefused );
    QSignalSpy horizonSpy( &ctx, &SelectionContext::activeHorizonChanged );
    auto *chip = bar.findChild<QToolButton *>( QStringLiteral( "chip_D61" ) );
    QVERIFY( chip != nullptr && chip->isEnabled() );
    chip->click();
    QCOMPARE( refuseSpy.count(), 1 );
    QVERIFY( refuseSpy.at( 0 ).at( 0 ).toString().contains( QStringLiteral( "编辑" ) ) );
    QVERIFY( refuseSpy.at( 0 ).at( 0 ).toString().contains( vl->name() ) );
    QCOMPARE( ctx.activeHorizon(), QStringLiteral( "D62" ) ); // 未切换
    QCOMPARE( horizonSpy.count(), 0 );
    QVERIFY( bar.isChipActive( QStringLiteral( "D62" ) ) ); // 高亮弹回原层位

    // 恢复路径：会话结束（回滚）后同一 chip 可再点且切换成功。
    vl->rollBack();
    chip->click();
    QCOMPARE( refuseSpy.count(), 1 ); // 不再拦截
    QCOMPARE( ctx.activeHorizon(), QStringLiteral( "D61" ) );
    QCOMPARE( layers.activeHorizon(), QStringLiteral( "D61" ) );
}

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    QgsApplication app( argc, argv, false );
    app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
    app.initQgis();
    TestChips tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgsApplication::exitQgis();
    return rc;
}

#include "tst_chips.moc"
