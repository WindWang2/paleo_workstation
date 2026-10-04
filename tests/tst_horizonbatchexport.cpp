#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/qgis/layoutexport.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/standardelements.h"
#include "../src/workflow/horizonbatchexport.h"

#include <qgsfeature.h>
#include <qgsfillsymbol.h>
#include <qgsgeometry.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutundostack.h>
#include <qgsmaplayer.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgspolygon.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgssinglesymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>

// 方向 25 M4：按层位组批量出图——计数=层位组数（Oracle 2）、文件名规则
// （工程_层位_日期）、catalog OUTPUT 登记、版面批后复原。
// 纪律同 tst_layoutdesigner_full：QgsProject 堆分配并故意泄漏。
class TestHorizonBatchExport : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeFigureLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      PaleoStandardElements::populateFigureLayout(
          layout, PaleoStandardElements::FigureKind::WellPosition,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() );
      // 定框后设真实范围（复原断言需要一个非退化的初始范围）。
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      if ( !maps.isEmpty() )
        maps.first()->setExtent( QgsRectangle( 0, 0, 2000, 1400 ) );
      return layout;
    }

    // 纯色多边形层（红填充无描边），覆盖 0..1000 × 0..800——像素一致性断言
    // 的渲染源（内部纯色，避开反锯齿边缘）。
    QgsVectorLayer *makeSolidLayer()
    {
      auto *layer = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:3857" ),
                                        QStringLiteral( "solid" ),
                                        QStringLiteral( "memory" ) );
      QgsFeature feature;
      feature.setGeometry( QgsGeometry::fromWkt(
          QStringLiteral( "POLYGON((0 0, 1000 0, 1000 800, 0 800, 0 0))" ) ) );
      layer->dataProvider()->addFeature( feature );
      layer->setRenderer( new QgsSingleSymbolRenderer( QgsFillSymbol::createSimple(
                              { { QStringLiteral( "color" ), QStringLiteral( "255,0,0,255" ) },
                                { QStringLiteral( "outline_style" ), QStringLiteral( "no" ) } } )
                              .release() ) );
      layer->updateExtents();
      return layer;
    }

    // 内存多边形层：整幅一个纯色矩形（像素一致性断言用得到时也可复用）。
    QgsVectorLayer *makeLayer( const QString &name )
    {
      auto *layer = new QgsVectorLayer( QStringLiteral( "Polygon?crs=EPSG:3857" ), name,
                                        QStringLiteral( "memory" ) );
      layer->updateExtents();
      return layer;
    }

  private slots:
    void countsMatchHorizonGroupsAndRestoresLayout()
    {
      QTemporaryDir projectDir;
      QVERIFY( projectDir.isValid() );
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );

      // 三个层位组（各带图层）。
      QMap<QString, QList<QgsMapLayer *>> groups;
      QList<QgsMapLayer *> keepAlive; // 内存层生命周期：批处理期间必须存活
      for ( const QString &h : { QStringLiteral( "SB1" ), QStringLiteral( "SB2" ),
                                 QStringLiteral( "SB3" ) } )
      {
        auto *layer = makeLayer( h + QStringLiteral( "_layer" ) );
        keepAlive.append( layer );
        groups.insert( h, { layer } );
      }

      // 版面原状（断言复原用）。
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      QVERIFY( !maps.isEmpty() );
      auto *titleLabel = qobject_cast<QgsLayoutItemLabel *>(
          layout->itemById( QStringLiteral( "title" ) ) );
      QVERIFY( titleLabel != nullptr );
      const QString titleBackup = titleLabel->text();
      const QgsRectangle extentBackup = maps.first()->extent();
      const int layerCountBackup = maps.first()->layers().size();

      PaleoHorizonBatchExport::Request request;
      request.layout = layout.get();
      request.projectName = QStringLiteral( "demo 工程" ); // 含空格/中文——文件名消毒对象
      request.horizonLayers = groups;
      request.catalog = &catalog;
      request.projectDir = projectDir.path();
      request.dpi = 96.0;
      request.format = PaleoHorizonBatchExport::Format::Png;
      request.date = QStringLiteral( "20261004" );

      const auto result = PaleoHorizonBatchExport::run( request );

      // Oracle 2：计数 = 层位组数。
      QCOMPARE( result.total, 3 );
      QCOMPARE( result.succeeded, 3 );
      QCOMPARE( result.failed, 0 );
      QCOMPARE( result.horizons.size(), 3 );

      // 文件名规则 + 受管落位 + catalog 登记。
      for ( const auto &outcome : result.horizons )
      {
        QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
        QVERIFY( !outcome.assetId.isEmpty() );
        QVERIFY( outcome.sha256.size() == 64 );
        QVERIFY( QFile::exists( outcome.file ) );
        const CatalogAsset asset = catalog.assetById( outcome.assetId );
        QCOMPARE( asset.type, QStringLiteral( "document" ) );
        QCOMPARE( asset.format, QStringLiteral( "png" ) );
        const CatalogVersion ver = catalog.versionBySha256( outcome.sha256 );
        QCOMPARE( ver.stage, QStringLiteral( "OUTPUT" ) );
        QVERIFY( ver.fileName.startsWith( QStringLiteral( "demo_工程_" ) ) );
        QVERIFY( ver.fileName.contains( QStringLiteral( "20261004" ) ) );
        QVERIFY( ver.fileName.endsWith( QStringLiteral( ".png" ) ) );
        QCOMPARE( ver.sha256, outcome.sha256 );
      }

      // 版面复原：标题文本与地图项状态回到批前（逐字段）。
      QCOMPARE( titleLabel->text(), titleBackup );
      QCOMPARE( maps.first()->layers().size(), layerCountBackup );
      QVERIFY( qAbs( maps.first()->extent().width() - extentBackup.width() ) < 1e-6 );
      QVERIFY( qAbs( maps.first()->extent().height() - extentBackup.height() ) < 1e-6 );

      // undo 历史不被批处理污染。
      QCOMPARE( layout->undoStack()->stack()->count(), 0 );
    }

    void emptyHorizonReportedAndMissingLayoutBails()
    {
      QTemporaryDir projectDir;
      DataCatalog catalog;
      QVERIFY2( catalog.open( projectDir.path() ), qPrintable( catalog.openError() ) );

      // 无版面：整体失败，逐层位入账。
      {
        QMap<QString, QList<QgsMapLayer *>> groups;
        groups.insert( QStringLiteral( "SB1" ), {} );
        PaleoHorizonBatchExport::Request request;
        request.layout = nullptr;
        request.horizonLayers = groups;
        request.catalog = &catalog;
        request.projectDir = projectDir.path();
        const auto result = PaleoHorizonBatchExport::run( request );
        QCOMPARE( result.total, 1 );
        QCOMPARE( result.failed, 1 );
        QVERIFY( !result.horizons.first().error.isEmpty() );
      }

      // 空图层集层位：如实失败（不静默跳过）；好层位照出。
      {
        std::unique_ptr<QgsPrintLayout> layout( makeFigureLayout() );
        auto *layer = makeLayer( QStringLiteral( "only" ) );
        QMap<QString, QList<QgsMapLayer *>> groups;
        groups.insert( QStringLiteral( "SB1" ), QList<QgsMapLayer *>() ); // 空
        groups.insert( QStringLiteral( "SB2" ), { layer } );
        PaleoHorizonBatchExport::Request request;
        request.layout = layout.get();
        request.projectName = QStringLiteral( "p" );
        request.horizonLayers = groups;
        request.catalog = &catalog;
        request.projectDir = projectDir.path();
        request.dpi = 96.0;
        const auto result = PaleoHorizonBatchExport::run( request );
        QCOMPARE( result.total, 2 ); // 计数 = 键数（Oracle 2 语义）
        QCOMPARE( result.succeeded, 1 );
        QCOMPARE( result.failed, 1 );
        QCOMPARE( result.horizons.first().horizon, QStringLiteral( "SB1" ) );
        QVERIFY( !result.horizons.first().ok );
        QVERIFY( result.horizons.last().ok );
        QVERIFY( !result.summary().isEmpty() );
      }

      // resolveHorizonLayers 防呆：空服务回空表。
      QVERIFY( PaleoHorizonBatchExport::resolveHorizonLayers( nullptr ).isEmpty() );
    }

    // Oracle 2 前半：导出与画布同图层渲染像素抽样一致。
    void exportMatchesCanvasPixels()
    {
      auto *project = new QgsProject(); // leaked on purpose
      QgsVectorLayer *layer = makeSolidLayer();
      project->addMapLayer( layer ); // 布局渲染管线要求图层挂工程

      // 布局：A4 横版，地图项铺满整页（无页边距 → 导出像素与画布像素同构）。
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      const QgsRectangle extent( 0, 0, 1000, 800 );
      auto *map = PaleoStandardElements::addMainMap( layout, { layer }, QgsRectangle() );
      QVERIFY( map != nullptr );
      map->attemptResize( QgsLayoutSize( 297, 210, Qgis::LayoutUnit::Millimeters ) );
      map->attemptMove( QgsLayoutPoint( 0, 0, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( extent ); // 定框后设范围（mapbook 顺序纪律）

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "match.png" ) );
      const auto outcome = PaleoLayoutExport::exportLayout(
          layout, path, PaleoLayoutExport::Format::Png, 96.0, PaleoLayoutExport::PageRange() );
      QVERIFY2( outcome.ok, qPrintable( outcome.error ) );
      QImage exported( path );
      QVERIFY( !exported.isNull() );
      QCOMPARE( exported.format(), QImage::Format_ARGB32 );

      // 画布同口径：同一 MapSettings 管线（QgsMapCanvas 渲染走的就是它）。
      QgsMapSettings settings;
      settings.setLayers( { layer } );
      settings.setExtent( extent );
      settings.setOutputSize( exported.size() );
      settings.setOutputDpi( 96.0 );
      QgsMapRendererSequentialJob job( settings );
      job.start();
      job.waitForFinished();
      const QImage canvas = job.renderedImage();
      QCOMPARE( canvas.size(), exported.size() );

      // 5×5 内部抽样（各缩进 10%）：纯色多边形内部，两管线应逐通道一致
      //（小容差吸收文本渲染/伽马微差；几何边界的 AA 点不采样）。
      int sampled = 0;
      for ( int iy = 1; iy <= 5; ++iy )
        for ( int ix = 1; ix <= 5; ++ix )
        {
          const QPoint p( exported.width() * ix / 6, exported.height() * iy / 6 );
          const QColor a = exported.pixelColor( p );
          const QColor b = canvas.pixelColor( p );
          QVERIFY2( qAbs( a.red() - b.red() ) <= 2 &&
                        qAbs( a.green() - b.green() ) <= 2 &&
                        qAbs( a.blue() - b.blue() ) <= 2,
                    qPrintable( QStringLiteral( "(%1,%2) export=%3 canvas=%4" )
                                    .arg( p.x() )
                                    .arg( p.y() )
                                    .arg( a.name(), b.name() ) ) );
          ++sampled;
        }
      QCOMPARE( sampled, 25 );
      delete layout;
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestHorizonBatchExport tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_horizonbatchexport.moc"
