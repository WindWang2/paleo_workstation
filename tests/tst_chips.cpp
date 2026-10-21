#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSet>
#include <QToolButton>

#include "../src/domain/mappinghorizons.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/horizonchipbar.h"
#include "../src/ui/wellsection/wellsectionstyle.h"
#include "uipolish_capture.h"

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

        // D-02（full-audit-20261021）：reload 后同一事件循环步内即时查询只
        // 反映新名单——旧实现 deleteLater 窗口内 findChildren 命中新旧两代，
        // chipNames() 双计数（8 层位返回 16 条）。
        bar.reloadHorizons();
        const QStringList reloaded = bar.chipNames();
        QCOMPARE( reloaded, mappingHorizons() );
        QCOMPARE( reloaded.size(), 8 );
        QCOMPARE( QSet<QString>( reloaded.cbegin(), reloaded.cend() ).size(), 8 );
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

    // goal/ui-experience-polish：Space 键切换层位 chip（checkable
    // QToolButton 的键盘等效——焦点环 2px 可见）。
    void spaceKeyTogglesChip()
    {
        SelectionContext ctx;
        HorizonChipBar bar( &ctx, nullptr );
        bar.setAllowEmptyHorizons( true );
        auto *chip = bar.findChild<QToolButton *>( QStringLiteral( "chip_C3" ) );
        QVERIFY( chip && chip->isEnabled() );
        bar.show();
        QVERIFY( QTest::qWaitForWindowExposed( &bar ) );
        chip->setFocus();
        QVERIFY( !chip->isChecked() );
        QTest::keyClick( chip, Qt::Key_Space );
        QCOMPARE( ctx.activeHorizon(), QStringLiteral( "C3" ) );
        QVERIFY( chip->isChecked() );
    }

    // goal/ui-experience-polish：chip 条（可用/禁用/激活态）修前/修后证据。
    void captureEvidence()
    {
        QTemporaryDir dir;
        QgisProjectService projectSvc;
        QVERIFY( projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
        LayerManifest manifest{ dir.filePath( QStringLiteral( "m.sqlite" ) ) };
        QVERIFY( manifest.open() );
        QgisLayerService layers{ &projectSvc, &manifest };
        SelectionContext ctx;
        HorizonChipBar bar( &ctx, &layers );
        bar.setAllowEmptyHorizons( true ); // 全 chip 可点，捕获完整胶囊样式
        uipolish::capturePanel( &bar, QStringLiteral( "horizonchips" ), QSize( 560, 48 ) );
    }

    // 2026-10-10 用户要求：层位 chip 颜色与井道图（连井剖面层段底色）保持
    // 一致，且「不要只有颜色，文字还是得有」。未选 chip 的底色取
    // wellsection::zoneColor 同一色板、mappingHorizons() 同一浅→深序（剖面上
    // 层段名 = 其顶界层位名）；文字完整显示、不被挤成省略号。
    void chipsColoredLikeWellSectionAndKeepText()
    {
        SelectionContext ctx;
        HorizonChipBar bar( &ctx, nullptr );
        bar.setAllowEmptyHorizons( true );
        // 故意给窄宽：chip 不许缩到 sizeHint 以下（否则文字省略只剩颜色）。
        bar.resize( 120, 40 );
        bar.show();
        QVERIFY( QTest::qWaitForWindowExposed( &bar ) );
        const QStringList horizons = mappingHorizons();
        for ( int i = 0; i < horizons.size(); ++i )
        {
            auto *chip = bar.findChild<QToolButton *>(
                QStringLiteral( "chip_%1" ).arg( horizons.at( i ) ) );
            QVERIFY2( chip != nullptr, qPrintable( horizons.at( i ) ) );
            QCOMPARE( chip->text(), horizons.at( i ) );
            QCOMPARE( chip->toolButtonStyle(), Qt::ToolButtonTextOnly );
            QVERIFY2( chip->width() >= chip->sizeHint().width(),
                      qPrintable( QStringLiteral( "%1 被压窄：%2 < %3" )
                                      .arg( horizons.at( i ) ).arg( chip->width() )
                                      .arg( chip->sizeHint().width() ) ) );
            // 底色 = 该层位在井道图上的层段色（取胶囊内左侧留白处，避开文字）。
            chip->setChecked( false );
            const QImage img = chip->grab().toImage();
            const QColor got = img.pixelColor( img.width() / 6, img.height() / 2 );
            const QColor want = wellsection::zoneColor( i );
            QVERIFY2( qAbs( got.red() - want.red() ) < 10 && qAbs( got.green() - want.green() ) < 10
                          && qAbs( got.blue() - want.blue() ) < 10,
                      qPrintable( QStringLiteral( "%1: got %2 want %3" )
                                      .arg( horizons.at( i ), got.name(), want.name() ) ) );
        }
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
