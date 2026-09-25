#include <QtTest>
#include <QDockWidget>
#include <QMenu>
#include <QToolBar>

#include "../src/ui/layoutdesignershell.h"
#include "../src/qgis/qgisruntime.h"

#include <qgslayoutitemlabel.h>
#include <qgslayoutview.h>
#include <qgsmasterlayoutinterface.h>
#include <qgsmessagebar.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// ET9 audit deliverable: PaleoLayoutDesignerShell hosts the qgis_gui designer
// widgets, and designerInterface() exposes them through the exported
// QgsLayoutDesignerInterface (same adapter pattern as upstream's
// QgsAppLayoutDesignerInterface). These tests pin the interface contract —
// identity accessors, no-op safety, dock adoption, standard tools — over a
// real QgsPrintLayout, offscreen, no fixtures needed.
class TestLayoutShell : public QObject
{
  Q_OBJECT

  private slots:
    void interfaceAccessors()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutDesignerShell shell( &layout );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      QVERIFY( iface != nullptr );
      QVERIFY( iface->view() != nullptr );
      QCOMPARE( iface->layout(), static_cast<QgsLayout *>( &layout ) );
      QCOMPARE( iface->masterLayout(), static_cast<QgsMasterLayoutInterface *>( &layout ) );
      QCOMPARE( iface->window(), static_cast<QWidget *>( &shell ) );
      QVERIFY( iface->messageBar() != nullptr );
      QCOMPARE( iface->view()->currentLayout(), static_cast<QgsLayout *>( &layout ) );
    }

    void menusAndToolbarsAreReal()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutDesignerShell shell( &layout );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      for ( QMenu *m : { iface->layoutMenu(), iface->editMenu(), iface->viewMenu(),
                         iface->itemsMenu(), iface->atlasMenu(), iface->reportMenu(),
                         iface->settingsMenu() } )
      {
        QVERIFY( m != nullptr );
        QVERIFY( !m->title().isEmpty() );
      }
      // Idempotent: repeated calls return the same menu.
      QCOMPARE( iface->layoutMenu(), iface->layoutMenu() );

      for ( QToolBar *tb : { iface->layoutToolbar(), iface->navigationToolbar(),
                             iface->actionsToolbar(), iface->atlasToolbar() } )
      {
        QVERIFY( tb != nullptr );
        QVERIFY( !tb->objectName().isEmpty() );
      }
      QCOMPARE( iface->navigationToolbar(), iface->navigationToolbar() );
    }

    void selectItemsAndItemOptions()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutDesignerShell shell( &layout );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      auto *label = new QgsLayoutItemLabel( &layout );
      layout.addLayoutItem( label );

      iface->selectItems( { label } );
      QVERIFY( label->isSelected() );
      QVERIFY( layout.selectedLayoutItems().contains( label ) );

      iface->selectItems( {} );
      QVERIFY( !label->isSelected() );

      // showItemOptions falls back to selection while the item dock is absent.
      iface->showItemOptions( label, false );
      QVERIFY( label->isSelected() );
    }

    void dockAdoptionAndRemoval()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutDesignerShell shell( &layout );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      auto *dock = new QDockWidget( QStringLiteral( "Test Dock" ) );
      iface->addDockWidget( Qt::RightDockWidgetArea, dock );
      QVERIFY( !dock->objectName().isEmpty() );
      QVERIFY( dock->parent() != nullptr ); // adopted into the shell's dock area
      QVERIFY( !dock->isHidden() ); // shown (shell itself isn't visible in this test)

      iface->removeDockWidget( dock );
      QVERIFY( dock->parent() == nullptr );
      delete dock;
    }

    void standardToolsAndStubs()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutDesignerShell shell( &layout );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      iface->activateTool( QgsLayoutDesignerInterface::ToolMoveItemContent );
      QVERIFY( iface->view()->tool() != nullptr );
      iface->activateTool( QgsLayoutDesignerInterface::ToolMoveItemNodes );
      QVERIFY( iface->view()->tool() != nullptr );

      // Stubs must be null-safe no-ops.
      QCOMPARE( iface->lastExportResults(), nullptr );
      iface->showRulers( false );
      iface->showRulers( true );
      iface->setAtlasPreviewEnabled( false );
      QVERIFY( !iface->atlasPreviewEnabled() );
    }

    void showAndCloseOffscreen()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      {
        PaleoLayoutDesignerShell shell( &layout );
        QgsLayoutDesignerInterface *iface = shell.designerInterface();
        shell.show();
        QTest::qWait( 20 );
        iface->close(); // interface-driven close path
        QVERIFY( !shell.isVisible() );
        QTest::qWait( 20 ); // event processing after close must be clean
      }
      QVERIFY( layout.layoutProject() == &project ); // shell didn't eat the layout
    }

    void nullLayoutDoesNotCrash()
    {
      PaleoLayoutDesignerShell shell( nullptr );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();
      QCOMPARE( iface->layout(), nullptr );
      QCOMPARE( iface->masterLayout(), nullptr );
      QVERIFY( iface->view() != nullptr );
      QVERIFY( iface->messageBar() != nullptr );
      shell.show();
      iface->close();
      QTest::qWait( 10 );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutShell tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutshell.moc"
