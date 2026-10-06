#include <QtTest>
#include <QDockWidget>
#include <QMenu>
#include <QToolBar>

#include "../src/ui/layoutdesignershell.h"
#include "../src/qgis/qgisruntime.h"

#include <qgslayoutitemlabel.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>
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
    void initialZoomWaitsForVisibleViewportAndReopenPreservesZoom()
    {
      QgsProject project;
      QgsPrintLayout layout(&project);
      layout.initializeDefaults();
      PaleoLayoutDesignerShell shell(&layout);
      // 先处理隐藏期间的队列，复现以前纸面缩成一个点的初始化顺序。
      QCoreApplication::processEvents();
      shell.show();
      auto *view = shell.view();
      // 首帧的适配通过 queued invocation 执行；完成它后再模拟用户缩放。
      QCoreApplication::sendPostedEvents(view, QEvent::MetaCall);
      QCoreApplication::processEvents();
      const auto paperFits = [&] {
        const auto rect = view->mapFromScene(layout.pageCollection()->page(0)->sceneBoundingRect())
                              .boundingRect();
        return rect.width() > 100 && rect.height() > 100 &&
               rect.width() <= view->viewport()->width() && rect.height() <= view->viewport()->height();
      };
      QTRY_VERIFY(paperFits());
      view->setZoomLevel(0.7);
      const QTransform zoom = view->transform();
      shell.hide();
      shell.show();
      QCoreApplication::processEvents();
      QCOMPARE(view->transform(), zoom);
    }

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
      // TEST-04：原断言自比较恒真。真实不变量：菜单 getter 幂等（重复取
      // 同一 QMenu*——接口契约：QGIS 设计器假设菜单实例稳定），且各菜单
      // 互不相同（getter 串了对象即红）。
      QCOMPARE( iface->layoutMenu(), iface->layoutMenu() );
      QVERIFY( iface->layoutMenu() != iface->editMenu() );
      QVERIFY( iface->layoutMenu() != iface->viewMenu() );

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
