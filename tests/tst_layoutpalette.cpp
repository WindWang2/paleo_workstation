#include <QtTest>
#include <QMultiHash>
#include <QSet>
#include <QSignalSpy>
#include <QToolButton>

#include "../src/ui/layout/layoutitempalette.h"
#include "../src/qgis/qgisruntime.h"

#include <qgsapplication.h>
#include <qgsgui.h>
#include <qgslayout.h>
#include <qgslayoutframe.h>
#include <qgslayoutitemattributetable.h>
#include <qgslayoutitemchart.h>
#include <qgslayoutitemguiregistry.h>
#include <qgslayoutitemelevationprofile.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmarker.h>
#include <qgslayoutitemmanualtable.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemregistry.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutitemshape.h>
#include <qgslayoutitemtexttable.h>
#include <qgslayoutmultiframe.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutview.h>
#include <qgslayoutviewtooladditem.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// Subtask A contract tests: PaleoLayoutItemPalette.
//
// QGIS 4.2 reality (probed on this box): QgsGui::layoutItemGuiRegistry()
// starts EMPTY; QgsLayoutGuiUtils::registerGuiForKnownItemTypes() registers
// the 18 default item GUI metadata entries with sequential ids (0..17) that
// are NOT QgsLayoutItemRegistry::ItemType values. LayoutTextTable has no
// default GUI metadata and is registered by the palette itself. The palette
// therefore speaks GUI-registry metadata ids end to end: buttons emit them,
// attach() feeds QgsLayoutViewToolAddItem::setItemMetadataId, addItemNow()
// goes through QgsLayoutItemGuiRegistry::createItem().
class TestLayoutPalette : public QObject
{
    Q_OBJECT

  private:
    QgsPrintLayout *makeLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose: ~QgsProject crashes in this embed scenario
      auto *layout = new QgsPrintLayout( project );
      auto *page = new QgsLayoutItemPage( layout );
      page->setPageSize( QStringLiteral( "A4" ), QgsLayoutItemPage::Portrait );
      layout->pageCollection()->addPage( page );
      return layout;
    }

    QRectF pageRect( QgsPrintLayout *layout )
    {
      QgsLayoutItemPage *page = layout->pageCollection()->page( 0 );
      const QSizeF size = layout->convertToLayoutUnits( page->pageSize() );
      return QRectF( page->pos(), size );
    }

    int guiId( int coreType, const QString &variantName = QString() )
    {
      QgsLayoutItemGuiRegistry *reg = QgsGui::layoutItemGuiRegistry();
      if ( !variantName.isEmpty() )
      {
        for ( int id : reg->itemMetadataIds() )
        {
          if ( reg->itemMetadata( id )->type() == coreType
               && reg->itemMetadata( id )->visibleName() == variantName )
            return id;
        }
      }
      return reg->metadataIdForItemType( coreType );
    }

    QList<QToolButton *> groupButtons( const PaleoLayoutItemPalette &p, const char *groupName )
    {
      QWidget *group = p.findChild<QWidget *>( groupName );
      if ( !group )
        return {};
      return group->findChildren<QToolButton *>();
    }

  private slots:
    void initTestCase()
    {
      // The palette must be constructible before any explicit registry setup:
      // construction guarantees the default GUI metadata exists.
      m_palette = std::make_unique<PaleoLayoutItemPalette>();
    }

    void groupsAndButtonCounts()
    {
      const QList<QToolButton *> main = groupButtons( *m_palette, "mainElements" );
      const QList<QToolButton *> secondary = groupButtons( *m_palette, "secondaryElements" );

      QVERIFY( main.size() >= 8 );
      QVERIFY( secondary.size() >= 5 );
      QCOMPARE( main.size() + secondary.size(), 16 );

      for ( QToolButton *b : main + secondary )
      {
        QVERIFY( b != nullptr );
        QVERIFY( !b->text().isEmpty() );
        QVERIFY( !b->objectName().isEmpty() );
        QCOMPARE( b->toolButtonStyle(), Qt::ToolButtonTextUnderIcon );
      }
      // object names unique across both groups
      QSet<QString> names;
      for ( QToolButton *b : main + secondary )
      {
        QVERIFY2( !names.contains( b->objectName() ), qPrintable( b->objectName() ) );
        names.insert( b->objectName() );
      }
    }

    void buttonClicksEmitGuiMetadataIds()
    {
      QgsLayoutItemGuiRegistry *reg = QgsGui::layoutItemGuiRegistry();
      QSignalSpy spy( m_palette.get(), &PaleoLayoutItemPalette::itemRequested );

      const QList<QToolButton *> all = groupButtons( *m_palette, "mainElements" )
                                     + groupButtons( *m_palette, "secondaryElements" );
      for ( QToolButton *b : all )
      {
        spy.clear();
        b->click();
        QCOMPARE( spy.count(), 1 );
        const int id = spy.at( 0 ).at( 0 ).toInt();
        QVERIFY2( id >= 0, qPrintable( b->objectName() ) );
        QVERIFY2( reg->itemMetadata( id ) != nullptr, qPrintable( b->objectName() ) );
      }

      // Core type coverage: every expected QgsLayoutItemRegistry::ItemType is
      // reachable through some button, and the duplicate-type variants
      // (picture/north arrow, rectangle/ellipse) resolve to distinct ids.
      QMultiHash<int, int> typeToIds;
      for ( QToolButton *b : all )
      {
        spy.clear();
        b->click();
        const int id = spy.at( 0 ).at( 0 ).toInt();
        typeToIds.insert( reg->itemMetadata( id )->type(), id );
      }
      const QList<int> expectedTypes = {
        QgsLayoutItemRegistry::LayoutMap,        QgsLayoutItemRegistry::LayoutLegend,
        QgsLayoutItemRegistry::LayoutScaleBar,   QgsLayoutItemRegistry::LayoutLabel,
        QgsLayoutItemRegistry::LayoutPicture,    QgsLayoutItemRegistry::LayoutShape,
        QgsLayoutItemRegistry::LayoutPolygon,    QgsLayoutItemRegistry::LayoutPolyline,
        QgsLayoutItemRegistry::LayoutAttributeTable, QgsLayoutItemRegistry::LayoutManualTable,
        QgsLayoutItemRegistry::LayoutTextTable,  QgsLayoutItemRegistry::LayoutElevationProfile,
        QgsLayoutItemRegistry::LayoutChart,      QgsLayoutItemRegistry::LayoutMarker
      };
      for ( int t : expectedTypes )
        QVERIFY2( typeToIds.contains( t ), QString( "core type %1 missing" ).arg( t ).toUtf8() );

      QCOMPARE( typeToIds.values( QgsLayoutItemRegistry::LayoutShape ).size(), 2 );        // rectangle + ellipse
      QCOMPARE( typeToIds.values( QgsLayoutItemRegistry::LayoutPicture ).size(), 2 );      // picture + north arrow
      const QList<int> shapeIds = typeToIds.values( QgsLayoutItemRegistry::LayoutShape );
      QVERIFY( shapeIds.at( 0 ) != shapeIds.at( 1 ) );
    }

    void addItemNowMapLandsCenteredOnPage()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = nullptr;
      const bool ok = PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutMap ), layout.get(), &created );

      QVERIFY( ok );
      QVERIFY( created != nullptr );
      QVERIFY( qobject_cast<QgsLayoutItemMap *>( created ) != nullptr );
      QCOMPARE( created->scene(), layout.get() ); // owned by the layout

      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      QCOMPARE( maps.size(), 1 );

      const QRectF page = pageRect( layout.get() );
      const QRectF itemRect( created->pos(), created->rect().size() );
      QVERIFY2( page.contains( itemRect ),
                qPrintable( QStringLiteral( "item %1 not inside page %2" ).arg( itemRect.width() ).arg( page.width() ) ) );
      QVERIFY( ( itemRect.center() - page.center() ).manhattanLength() < 1.0 );
    }

    void addItemNowLabelScaleBarShape()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = nullptr;

      // label — QGIS-native added-to-layout hook fills in default text
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutLabel ), layout.get(), &created ) );
      auto *label = qobject_cast<QgsLayoutItemLabel *>( created );
      QVERIFY( label != nullptr );
      QVERIFY( !label->text().isEmpty() );
      QVERIFY( pageRect( layout.get() ).intersects( QRectF( label->pos(), label->rect().size() ) ) );

      // scale bar
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutScaleBar ), layout.get(), &created ) );
      QVERIFY( qobject_cast<QgsLayoutItemScaleBar *>( created ) != nullptr );

      // ellipse variant of the shape metadata
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutShape, QStringLiteral( "Ellipse" ) ), layout.get(), &created ) );
      auto *shape = qobject_cast<QgsLayoutItemShape *>( created );
      QVERIFY( shape != nullptr );
      QCOMPARE( shape->shapeType(), QgsLayoutItemShape::Ellipse );

      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      QCOMPARE( items.size(), 3 + 1 ); // + the page item itself
    }

    void addItemNowMultiFrameTables()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = nullptr;

      // attribute table — QGIS's own multiframe factory metadata
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutAttributeTable ), layout.get(), &created ) );
      QVERIFY( qobject_cast<QgsLayoutFrame *>( created ) != nullptr );
      QCOMPARE( layout->multiFrames().size(), 1 );
      QCOMPARE( layout->multiFrames().at( 0 )->frames().size(), 1 );
      QCOMPARE( layout->multiFrames().at( 0 )->type(), QgsLayoutItemRegistry::LayoutAttributeTable );
      QVERIFY( created->scene() == layout.get() );

      // text table — GUI metadata registered by the palette itself
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutTextTable ), layout.get(), &created ) );
      QVERIFY( qobject_cast<QgsLayoutFrame *>( created ) != nullptr );
      QCOMPARE( layout->multiFrames().size(), 2 );
      QCOMPARE( layout->multiFrames().at( 1 )->type(), QgsLayoutItemRegistry::LayoutTextTable );
      QCOMPARE( layout->multiFrames().at( 1 )->frames().size(), 1 );

      const QRectF page = pageRect( layout.get() );
      const QRectF frameRect( created->pos(), created->rect().size() );
      QVERIFY2( page.contains( frameRect ),
                qPrintable( QStringLiteral( "frame %1x%2 out of page" ).arg( frameRect.width() ).arg( frameRect.height() ) ) );
    }

    void addItemNowNorthArrow()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = nullptr;

      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutPicture, QStringLiteral( "North Arrow" ) ), layout.get(), &created ) );
      auto *picture = qobject_cast<QgsLayoutItemPicture *>( created );
      QVERIFY( picture != nullptr );
      QVERIFY( picture->picturePath().contains( QLatin1String( "north_arrow" ), Qt::CaseInsensitive ) );
      QVERIFY( !picture->id().isEmpty() ); // north arrows get a discernible id (upstream behavior)
    }

    void addItemNowNodeItems()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = nullptr;

      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutPolygon ), layout.get(), &created ) );
      QVERIFY( created != nullptr );
      QCOMPARE( created->type(), QgsLayoutItemRegistry::LayoutPolygon );

      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutPolyline, QStringLiteral( "Polyline" ) ), layout.get(), &created ) );
      QVERIFY( created != nullptr );
      QCOMPARE( created->type(), QgsLayoutItemRegistry::LayoutPolyline );

      const QRectF page = pageRect( layout.get() );
      QVERIFY( page.intersects( created->sceneBoundingRect() ) );
    }

    void addItemNowRejectsInvalidInput()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItem *created = reinterpret_cast<QgsLayoutItem *>( quintptr( 1 ) );

      QVERIFY( !PaleoLayoutItemPalette::addItemNow( -1, layout.get(), &created ) );
      QCOMPARE( created, nullptr );
      QVERIFY( !PaleoLayoutItemPalette::addItemNow( 999999, layout.get(), &created ) );
      QCOMPARE( created, nullptr );
      QVERIFY( !PaleoLayoutItemPalette::addItemNow( 0, nullptr, &created ) );
      QCOMPARE( created, nullptr );

      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      QCOMPARE( items.size(), 1 ); // page only, nothing leaked into the layout
    }

    void addItemNowOnPagelessLayoutFallsBackToNominalPage()
    {
      // a fresh QgsPrintLayout has zero pages (probed); placement must still
      // work against the nominal A4 fallback rect
      auto *project = new QgsProject();
      std::unique_ptr<QgsPrintLayout> layout( new QgsPrintLayout( project ) );
      QCOMPARE( layout->pageCollection()->pageCount(), 0 );

      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
        guiId( QgsLayoutItemRegistry::LayoutLegend ), layout.get(), &created ) );
      QVERIFY( qobject_cast<QgsLayoutItemLegend *>( created ) != nullptr );

      const QRectF nominal( 0, 0, 210, 297 );
      const QRectF rect( created->pos(), created->rect().size() );
      QVERIFY2( nominal.contains( rect ),
                qPrintable( QStringLiteral( "legend %1,%2 %3x%4 outside nominal A4" )
                              .arg( rect.x() ).arg( rect.y() ).arg( rect.width() ).arg( rect.height() ) ) );
    }

    void attachSwitchesViewToAddItemTool()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );

      PaleoLayoutItemPalette palette;
      QgsLayoutView view;
      view.setCurrentLayout( layout.get() );
      palette.attach( &view );

      QToolButton *mapButton = palette.findChild<QToolButton *>( "btnAddMap" );
      QVERIFY( mapButton != nullptr );
      mapButton->click();

      QVERIFY2( qobject_cast<QgsLayoutViewToolAddItem *>( view.tool() ) != nullptr,
                "view tool must be the QGIS add-item tool after a palette button click" );
      QCOMPARE( qobject_cast<QgsLayoutViewToolAddItem *>( view.tool() )->itemMetadataId(),
                guiId( QgsLayoutItemRegistry::LayoutMap ) );

      // standalone emission (no attach) must stay clean
      PaleoLayoutItemPalette bare;
      QSignalSpy spy( &bare, &PaleoLayoutItemPalette::itemRequested );
      bare.findChild<QToolButton *>( "btnAddNorthArrow" )->click();
      QCOMPARE( spy.count(), 1 );
    }

    void pagePropertiesEntry()
    {
      QSignalSpy itemSpy( m_palette.get(), &PaleoLayoutItemPalette::itemRequested );
      QSignalSpy pageSpy( m_palette.get(), &PaleoLayoutItemPalette::pagePropertiesRequested );

      QToolButton *pageButton = m_palette->findChild<QToolButton *>( "btnPageProperties" );
      QVERIFY( pageButton != nullptr );
      pageButton->click();

      QCOMPARE( pageSpy.count(), 1 );
      QCOMPARE( itemSpy.count(), 0 ); // page entry never masquerades as an item
    }

  private:
    std::unique_ptr<PaleoLayoutItemPalette> m_palette;
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutPalette tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutpalette.moc"
