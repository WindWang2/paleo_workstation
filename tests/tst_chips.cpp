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

// wave/mapping-pipeline 阶段E — 编图 chip 只列 8 个层序界面；
// 切换沿用 activeHorizon + 按层位懒加载；集合外的层位名字不进 chip。

class TestChips : public QObject
{
    Q_OBJECT

  private slots:

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
};

int main( int argc, char *argv[] )
{
    if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
        qputenv( "QT_QPA_PLATFORM", "offscreen" );
    QgsApplication app( argc, argv, false );
    app.setPrefixPath( QStringLiteral( "/usr" ), true );
    app.initQgis();
    TestChips tc;
    const int rc = QTest::qExec( &tc, argc, argv );
    QgsApplication::exitQgis();
    return rc;
}

#include "tst_chips.moc"
