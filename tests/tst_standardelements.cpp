#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QTemporaryDir>

#include "../src/qgis/standardelements.h"
#include "../src/qgis/qgisruntime.h"

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmapgrid.h>
#include <qgslayoutitemmapoverview.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutundostack.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>

// 方向 25 M1/M2/M3 核心：标准图件元素工厂 + 三类内容模板骨架。
// 纪律同 tst_layoutdesigner_full：QgsProject 堆分配并故意泄漏（QGIS 4.2.2
// 栈上析构有 QgsProjectStyleSettings 崩溃路径）；版面本身逐用例管理。
class TestStandardElements : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose (see class comment)
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      return layout;
    }

    static int countItems( QgsPrintLayout *layout, const QString &id )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      int n = 0;
      for ( QgsLayoutItem *item : items )
        if ( item->id() == id )
          ++n;
      return n;
    }

    static QgsLayoutItem *findItem( QgsPrintLayout *layout, const QString &id )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      for ( QgsLayoutItem *item : items )
        if ( item->id() == id )
          return item;
      return nullptr;
    }

  private slots:
    void figureKindVocabulary()
    {
      const QList<PaleoStandardElements::FigureKind> kinds = {
        PaleoStandardElements::FigureKind::WellPosition,
        PaleoStandardElements::FigureKind::SingleFactor,
        PaleoStandardElements::FigureKind::Facies,
      };
      QStringList keys;
      for ( auto kind : kinds )
      {
        const QString key = PaleoStandardElements::figureKindKey( kind );
        QVERIFY( !key.isEmpty() );
        QVERIFY( !keys.contains( key ) );
        keys << key;

        PaleoStandardElements::FigureKind back;
        QVERIFY( PaleoStandardElements::figureKindFromKey( key, &back ) );
        QCOMPARE( back, kind );

        QVERIFY( !PaleoStandardElements::figureKindTitle( kind ).isEmpty() );
      }
      QCOMPARE( keys, QStringList( { QStringLiteral( "well_position" ),
                                     QStringLiteral( "single_factor" ),
                                     QStringLiteral( "facies" ) } ) );
      PaleoStandardElements::FigureKind dummy;
      QVERIFY( !PaleoStandardElements::figureKindFromKey( QStringLiteral( "nope" ), &dummy ) );
      QCOMPARE( PaleoStandardElements::figureKindTitle( PaleoStandardElements::FigureKind::Facies ),
                QStringLiteral( "沉积相图" ) );
    }

    void applyPageSetupResizesPage()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );

      PaleoStandardElements::PageSetup a3l;
      a3l.sizeName = QStringLiteral( "A3" );
      a3l.landscape = true;
      QVERIFY( PaleoStandardElements::applyPageSetup( layout.get(), a3l ) );
      QgsLayoutSize paper = layout->pageCollection()->page( 0 )->pageSize();
      QCOMPARE( paper.units(), Qgis::LayoutUnit::Millimeters );
      QVERIFY( qAbs( paper.width() - 420.0 ) < 0.5 );
      QVERIFY( qAbs( paper.height() - 297.0 ) < 0.5 );

      PaleoStandardElements::PageSetup a4p;
      a4p.sizeName = QStringLiteral( "A4" );
      a4p.landscape = false;
      QVERIFY( PaleoStandardElements::applyPageSetup( layout.get(), a4p ) );
      paper = layout->pageCollection()->page( 0 )->pageSize();
      QVERIFY( qAbs( paper.width() - 210.0 ) < 0.5 );
      QVERIFY( qAbs( paper.height() - 297.0 ) < 0.5 );

      QVERIFY( !PaleoStandardElements::applyPageSetup( nullptr, a4p ) );
    }

    void factoriesLinkAndStyle()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );

      QgsLayoutItemMap *map = PaleoStandardElements::addMainMap( layout.get(), {}, QgsRectangle() );
      QVERIFY( map != nullptr );
      QCOMPARE( map->id(), QStringLiteral( "map" ) );
      QCOMPARE( map->scene(), static_cast<QGraphicsScene *>( layout.get() ) ); // 所有权在 layout
      // 装饰定位基准是地图的 scene 矩形——先给地图一个真实版面尺寸。
      map->attemptResize( QgsLayoutSize( 200, 150, Qgis::LayoutUnit::Millimeters ) );
      map->attemptMove( QgsLayoutPoint( 10, 24, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( QgsRectangle( 0, 0, 1000, 800 ) );

      QgsLayoutItemScaleBar *bar = PaleoStandardElements::addScaleBar( layout.get(), map );
      QVERIFY( bar != nullptr );
      QCOMPARE( bar->linkedMap(), map );                 // 比例联动落点
      QCOMPARE( bar->units(), Qgis::DistanceUnit::Meters );
      QCOMPARE( bar->style(), QStringLiteral( "Single Box" ) ); // 条式 + 数字

      QgsLayoutItemLegend *legend = PaleoStandardElements::addLegend( layout.get(), map );
      QVERIFY( legend != nullptr );
      QCOMPARE( legend->linkedMap(), map );
      QCOMPARE( legend->syncMode(), Qgis::LegendSyncMode::AllProjectLayers ); // 随图层树自动更新
      QVERIFY( legend->legendFilterByMapEnabled() );

      QgsLayoutItem *north = PaleoStandardElements::addNorthArrow( layout.get(), map );
      QVERIFY( north != nullptr );
      QCOMPARE( north->id(), QStringLiteral( "northArrow" ) );
      if ( auto *picture = qobject_cast<QgsLayoutItemPicture *>( north ) )
        QCOMPARE( picture->linkedMap(), map ); // 随地图旋转

      PaleoStandardElements::addCoordinateGrid( map );
      QCOMPARE( map->grids()->size(), 1 );
      QVERIFY( map->grids()->grid( 0 )->enabled() );
      QVERIFY( map->grids()->grid( 0 )->annotationEnabled() );

      // 定位约定（DESIGN.md 画布内装饰）：指北针在地图左上、比例尺在地图左下。
      const QRectF mapRect = map->mapRectToScene( map->rect() );
      QVERIFY( north->mapRectToScene( north->rect() ).center().x() < mapRect.center().x() );
      QVERIFY( north->mapRectToScene( north->rect() ).center().y() < mapRect.center().y() );
      QVERIFY( bar->mapRectToScene( bar->rect() ).center().y() > mapRect.center().y() );

      const auto block = PaleoStandardElements::addTitleBlock( layout.get(), QStringLiteral( "图名" ),
                                                               QStringLiteral( "副题" ) );
      QVERIFY( block.title && block.subtitle && block.signature );
      QCOMPARE( block.title->text(), QStringLiteral( "图名" ) );
      QCOMPARE( block.subtitle->text(), QStringLiteral( "副题" ) );
      QCOMPARE( block.title->textFormat().size(), 15.0 ); // DESIGN.md 图件标题 15pt
      QCOMPARE( block.subtitle->textFormat().size(), 9.0 );
      QCOMPARE( block.signature->textFormat().size(), 8.0 );

      // 空参防呆：nullptr 全部安全。
      QCOMPARE( PaleoStandardElements::addScaleBar( nullptr, nullptr ), nullptr );
      QCOMPARE( PaleoStandardElements::addLegend( nullptr, nullptr ), nullptr );
      QCOMPARE( PaleoStandardElements::addNorthArrow( nullptr, nullptr ), nullptr );
      PaleoStandardElements::addCoordinateGrid( nullptr ); // 不崩即可
    }

    void insetOverviewBindsMainMap()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItemMap *map = PaleoStandardElements::addMainMap( layout.get(), {}, QgsRectangle() );
      map->attemptResize( QgsLayoutSize( 200, 150, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( QgsRectangle( 0, 0, 1000, 800 ) ); // 定框后设范围（mapbook 顺序纪律）

      QgsLayoutItemMap *inset = PaleoStandardElements::addInsetMap( layout.get(), map );
      QVERIFY( inset != nullptr );
      QCOMPARE( inset->id(), QStringLiteral( "inset" ) );
      QCOMPARE( inset->overviews()->size(), 1 );
      QCOMPARE( inset->overviews()->overview( 0 )->linkedMap(), map ); // M3 联动落点
      QVERIFY( inset->overviews()->overview( 0 )->enabled() );
      QVERIFY( !inset->extent().isEmpty() );

      QCOMPARE( PaleoStandardElements::addInsetMap( nullptr, map ), nullptr );
      QCOMPARE( PaleoStandardElements::addInsetMap( layout.get(), nullptr ), nullptr );
    }

    void populateFigureKindsInventory()
    {
      for ( auto kind : { PaleoStandardElements::FigureKind::WellPosition,
                          PaleoStandardElements::FigureKind::SingleFactor,
                          PaleoStandardElements::FigureKind::Facies } )
      {
        std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
        PaleoStandardElements::PageSetup a3p;
        a3p.sizeName = QStringLiteral( "A3" );
        a3p.landscape = false;
        QString err;
        QVERIFY2( PaleoStandardElements::populateFigureLayout( layout.get(), kind, a3p, {},
                                                               QgsRectangle(), &err ),
                  qPrintable( err ) );

        // 页面规格生效。
        const QgsLayoutSize paper = layout->pageCollection()->page( 0 )->pageSize();
        QVERIFY( qAbs( paper.width() - 297.0 ) < 0.5 );
        QVERIFY( qAbs( paper.height() - 420.0 ) < 0.5 );

        // 标准元素清单：恰好各一件（骨架唯一性）。
        for ( const char *id : { "map", "scalebar", "legend", "northArrow", "title", "subtitle",
                                 "signature" } )
        {
          const QString itemId = QString::fromLatin1( id );
          QVERIFY2( countItems( layout.get(), itemId ) == 1,
                    qPrintable( QStringLiteral( "%1: %2" )
                                    .arg( PaleoStandardElements::figureKindKey( kind ), itemId ) ) );
        }

        // 沉积相图独有插图（overview 指主图）。
        QCOMPARE( countItems( layout.get(), QStringLiteral( "inset" ) ),
                  kind == PaleoStandardElements::FigureKind::Facies ? 1 : 0 );

        // 标题=图件种类名；副题为空串占位。
        auto *title = qobject_cast<QgsLayoutItemLabel *>(
            findItem( layout.get(), QStringLiteral( "title" ) ) );
        QVERIFY( title != nullptr );
        QCOMPARE( title->text(), PaleoStandardElements::figureKindTitle( kind ) );

        // 主地图在标题块之下。
        auto *mapItem = qobject_cast<QgsLayoutItemMap *>( findItem( layout.get(), QStringLiteral( "map" ) ) );
        QVERIFY( mapItem != nullptr );
        QVERIFY( mapItem->mapRectToScene( mapItem->rect() ).top() > 20.0 );

        // 网格挂上主地图。
        QCOMPARE( mapItem->grids()->size(), 1 );
      }
    }

    void populateClearsExistingContent()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      // 预置杂物：两个标签 + 一段 undo 历史。
      const int undoBefore = layout->undoStack()->stack()->count();
      auto *junk = new QgsLayoutItemLabel( layout.get() );
      junk->setId( QStringLiteral( "junk" ) );
      layout->addLayoutItem( junk );
      auto *junk2 = new QgsLayoutItemLabel( layout.get() );
      junk2->setId( QStringLiteral( "junk2" ) );
      layout->addLayoutItem( junk2 );
      QVERIFY( countItems( layout.get(), QStringLiteral( "junk" ) ) == 1 );
      const int undoBaseline = layout->undoStack()->stack()->count(); // 含夹具两条 add

      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::WellPosition,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );
      QCOMPARE( countItems( layout.get(), QStringLiteral( "junk" ) ), 0 );
      QCOMPARE( countItems( layout.get(), QStringLiteral( "junk2" ) ), 0 );

      // 模板装配不进 undo 历史（清场与重建都不留命令）。
      QCOMPARE( layout->undoStack()->stack()->count(), undoBaseline );

      QVERIFY( !PaleoStandardElements::populateFigureLayout( nullptr,
                                                             PaleoStandardElements::FigureKind::Facies,
                                                             PaleoStandardElements::PageSetup{}, {},
                                                             QgsRectangle() ) );
    }

    void templateXmlRoundTrip()
    {
      // Oracle 1 的种子：populate → saveAsTemplate → 清场 → loadFromTemplate，
      // 元素树（id 集合）+ 绑定（linkedMap/syncMode/overview）+ 属性（位置/尺寸/
      // 字号）逐字段一致。
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::Facies,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "facies.qpt" ) );
      QgsReadWriteContext saveCtx;
      QVERIFY( layout->saveAsTemplate( path, saveCtx ) );
      QVERIFY( QFileInfo::exists( path ) );

      // 记录套用前状态（逐字段）。
      QStringList idsBefore;
      QMap<QString, QPointF> posBefore;
      QMap<QString, QSizeF> sizeBefore;
      QList<QgsLayoutItem *> items; // 内容项（页面项不进 round-trip 账）
      layout->layoutItems( items );
      for ( QgsLayoutItem *item : items )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        idsBefore << item->id();
        posBefore[item->id()] = item->mapToScene( item->rect().topLeft() );
        sizeBefore[item->id()] = QSizeF( item->rect().width(), item->rect().height() );
      }
      idsBefore.sort();

      // 清场重载（loadFromTemplate clearExisting）——页面项保留。
      for ( QgsLayoutItem *item : items )
      {
        if ( !qobject_cast<QgsLayoutItemPage *>( item ) )
          layout->removeLayoutItem( item );
      }
      QTest::qWait( 10 );

      QFile in( path );
      QVERIFY( in.open( QIODevice::ReadOnly ) );
      QDomDocument doc;
      QVERIFY( doc.setContent( &in ) );
      in.close();
      bool loadedOk = false;
      layout->loadFromTemplate( doc, QgsReadWriteContext(), true, &loadedOk );
      QVERIFY( loadedOk );

      // 元素树一致（页项不计——QGIS 会按模板页集合重建）。
      QStringList idsAfter;
      QList<QgsLayoutItem *> after;
      layout->layoutItems( after );
      for ( QgsLayoutItem *item : after )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        idsAfter << item->id();
      }
      idsAfter.sort();
      QCOMPARE( idsAfter, idsBefore );

      // 属性逐字段：位置/尺寸（1mm 容差——QGIS 序列化往返的浮点口径）。
      for ( QgsLayoutItem *item : after )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        QVERIFY2( posBefore.contains( item->id() ),
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->mapToScene( item->rect().topLeft() ).x() - posBefore[item->id()].x() ) < 1.0,
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->mapToScene( item->rect().topLeft() ).y() - posBefore[item->id()].y() ) < 1.0,
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->rect().width() - sizeBefore[item->id()].width() ) < 1.0,
                  qPrintable( item->id() ) );
      }

      // 绑定逐字段：比例尺/图例回指主图；插图的 overview 回指主图。
      auto *mapAfter = qobject_cast<QgsLayoutItemMap *>( findItem( layout.get(), QStringLiteral( "map" ) ) );
      auto *barAfter = qobject_cast<QgsLayoutItemScaleBar *>(
          findItem( layout.get(), QStringLiteral( "scalebar" ) ) );
      auto *legendAfter = qobject_cast<QgsLayoutItemLegend *>(
          findItem( layout.get(), QStringLiteral( "legend" ) ) );
      auto *insetAfter = qobject_cast<QgsLayoutItemMap *>(
          findItem( layout.get(), QStringLiteral( "inset" ) ) );
      QVERIFY( mapAfter && barAfter && legendAfter && insetAfter );
      QCOMPARE( barAfter->linkedMap(), mapAfter );
      QCOMPARE( legendAfter->linkedMap(), mapAfter );
      QCOMPARE( legendAfter->syncMode(), Qgis::LegendSyncMode::AllProjectLayers );
      QCOMPARE( insetAfter->overviews()->size(), 1 );
      QCOMPARE( insetAfter->overviews()->overview( 0 )->linkedMap(), mapAfter );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestStandardElements tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_standardelements.moc"
