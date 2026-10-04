#include <QtTest>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolBar>

#include "../src/ui/layoutdesignershell.h"
#include "../src/ui/layout/layoutexportactions.h"
#include "../src/ui/layout/layoutitempalette.h"
#include "../src/ui/layout/layoutitempanel.h"
#include "../src/ui/layout/layoutitemtree.h"
#include "../src/ui/layout/layouttemplates.h"
#include "../src/ui/layout/layoutundostack.h"
#include "../src/qgis/qgisruntime.h"

#include <qgsgui.h>
#include <qgslayout.h>
#include <qgslayoutitemguiregistry.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmapgrid.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitemregistry.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutitemwidget.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpoint.h>
#include <qgslayoutsize.h>
#include <qgslayoutsnapper.h>
#include <qgslayoutundostack.h>
#include <qgslayoutview.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// Subtask E: full-designer integration. The shell composes the four sibling
// deliverables — A (PaleoLayoutItemPalette, left), B (PaleoLayoutItemPanel,
// right), C (PaleoLayoutExportActions + PaleoLayoutTemplates, File menu) and
// D (PaleoLayoutUndoStack, Layout/Edit + toolbar) — around the central
// QgsLayoutView, with a 4-entry menu bar (File/Items/Layout/Settings), the 7
// QgsLayoutDesignerInterface menu accessors mapped onto them, page navigation
// via the fitInView recipe (no showPage() in QGIS 4.2) and selection -> panel
// wiring incl. the undo/redo instance-recreation refresh.
//
// QgsProject lifetime (A's process-level finding): constructing the palette
// initializes QgsGui; a stack QgsProject destructing afterwards has a
// QgsProjectStyleSettings::removeProjectStyle crash path on this QGIS 4.2.2
// build. Every fixture here heap-allocates the project and leaks it on
// purpose (A's sanctioned pattern); the layout itself is owned per-test.
class TestLayoutDesignerFull : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeLayout( int pages = 1 )
    {
      auto *project = new QgsProject(); // leaked on purpose (see class comment)
      auto *layout = new QgsPrintLayout( project );
      // Page setup must not pollute the undo history the undo tests assert on.
      layout->undoStack()->blockCommands( true );
      layout->initializeDefaults(); // one A4 landscape page
      for ( int i = 1; i < pages; ++i )
        layout->pageCollection()->extendByNewPage();
      layout->undoStack()->blockCommands( false );
      return layout;
    }

    static int guiId( int coreType )
    {
      return QgsGui::layoutItemGuiRegistry()->metadataIdForItemType( coreType );
    }

    static int labelCount( QgsLayout &layout )
    {
      QList<QgsLayoutItemLabel *> labels;
      layout.layoutItems<QgsLayoutItemLabel>( labels );
      return labels.count();
    }

    static QString templatesDir()
    {
      // tests/ -> worktree root -> docs/templates, robust to the build dir.
      return QFileInfo( QStringLiteral( __FILE__ ) ).dir().filePath( QStringLiteral( "../docs/templates" ) );
    }

  private slots:
    // --- shell structure ------------------------------------------------------

    void shellStructureMenusAndPanels()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      // Exactly four top-level menus, in order, with the agreed titles.
      QMenuBar *bar = shell.findChild<QMenuBar *>();
      QVERIFY( bar != nullptr );
      const QList<QAction *> tops = bar->actions();
      QCOMPARE( tops.size(), 4 );
      QStringList titles;
      for ( QAction *a : tops )
      {
        QVERIFY( a->menu() != nullptr );
        titles << a->menu()->title();
      }
      QCOMPARE( titles, QStringList( { QStringLiteral( "文件(&F)" ), QStringLiteral( "项(&I)" ),
                                       QStringLiteral( "版面(&L)" ), QStringLiteral( "设置(&S)" ) } ) );

      // The 7 interface accessors return real, idempotent QMenus with titles;
      // edit/view/atlas/report hang as submenus off the Layout top-level.
      for ( QMenu *m : { iface->layoutMenu(), iface->editMenu(), iface->viewMenu(),
                         iface->itemsMenu(), iface->atlasMenu(), iface->reportMenu(),
                         iface->settingsMenu() } )
      {
        QVERIFY( m != nullptr );
        QVERIFY( !m->title().isEmpty() );
      }
      QCOMPARE( iface->layoutMenu(), iface->layoutMenu() );
      QCOMPARE( iface->layoutMenu()->title(), QStringLiteral( "版面(&L)" ) );
      QCOMPARE( iface->itemsMenu()->title(), QStringLiteral( "项(&I)" ) );
      QCOMPARE( iface->settingsMenu()->title(), QStringLiteral( "设置(&S)" ) );
      for ( QMenu *sub : { iface->editMenu(), iface->viewMenu(), iface->atlasMenu(), iface->reportMenu() } )
        QVERIFY( iface->layoutMenu()->actions().contains( sub->menuAction() ) );

      // Left palette, right properties panel, bottom status bar all reachable.
      QVERIFY( shell.findChild<PaleoLayoutItemPalette *>() != nullptr );
      QVERIFY( shell.findChild<PaleoLayoutItemPanel *>() != nullptr );
      QVERIFY( shell.findChild<QStatusBar *>() != nullptr );
      QVERIFY( shell.findChild<QLabel *>( QStringLiteral( "pageLabel" ) ) != nullptr );
      QVERIFY( shell.findChild<QSpinBox *>( QStringLiteral( "pageSpinBox" ) ) != nullptr );

      // File menu: 3 export actions + template actions + builtin submenu + close.
      for ( const char *name : { "actionExportLayoutPng", "actionExportLayoutPdf",
                                 "actionExportLayoutSvg", "actionSaveLayoutTemplate",
                                 "actionLoadLayoutTemplate", "actionCloseLayoutDesigner" } )
      {
        QVERIFY2( shell.findChild<QAction *>( QString::fromLatin1( name ) ) != nullptr, name );
      }
      // 内置模板两级（方向 25）：页面规格 N 项 + 图件模板子菜单（三类 × 4 规格）。
      QMenu *builtins = shell.findChild<QMenu *>( QStringLiteral( "menuBuiltinTemplates" ) );
      QVERIFY( builtins != nullptr );
      QCOMPARE( builtins->actions().size(),
                PaleoLayoutTemplates::builtinKeys().size() + 1 ); // + 图件模板子菜单
      QMenu *figures = shell.findChild<QMenu *>( QStringLiteral( "menuFigureTemplates" ) );
      QVERIFY( figures != nullptr );
      QCOMPARE( figures->actions().size(), PaleoLayoutTemplates::figureBuiltinKeys().size() );
      QCOMPARE( PaleoLayoutTemplates::figureBuiltinKeys().size(), 12 );

      // Items menu mirrors the palette's add entries + page properties.
      QMenu *items = iface->itemsMenu();
      QVERIFY( shell.findChild<QAction *>( QStringLiteral( "menuPageProperties" ) ) != nullptr );
      for ( int id : shell.findChild<PaleoLayoutItemPalette *>()->itemMetadataIds() )
      {
        if ( id < 0 )
          continue;
        QVERIFY2( items->actions().contains(
                    shell.findChild<QAction *>( QStringLiteral( "menuAddItem_%1" ).arg( id ) ) ),
                  qPrintable( QStringLiteral( "menuAddItem_%1" ).arg( id ) ) );
      }

      // Edit submenu carries the subtask-D undo/redo actions; the toolbar too.
      QVERIFY( iface->editMenu()->actions().contains( shell.findChild<QAction *>( QStringLiteral( "mActionUndo" ) ) ) );
      QVERIFY( iface->layoutToolbar()->actions().contains( shell.findChild<QAction *>( QStringLiteral( "mActionUndo" ) ) ) );
      QVERIFY( iface->viewMenu()->actions().contains( shell.findChild<QAction *>( QStringLiteral( "actionShowRulers" ) ) ) );
    }

    void nullLayoutDegradesSafely()
    {
      PaleoLayoutDesignerShell shell( nullptr );
      QgsLayoutDesignerInterface *iface = shell.designerInterface();

      QCOMPARE( iface->layout(), nullptr );
      QMenuBar *bar = shell.findChild<QMenuBar *>();
      QVERIFY( bar != nullptr );
      QCOMPARE( bar->actions().size(), 4 );
      QVERIFY( shell.findChild<PaleoLayoutItemPalette *>() != nullptr );
      QVERIFY( shell.findChild<PaleoLayoutItemPanel *>() != nullptr );

      QLabel *pageLabel = shell.findChild<QLabel *>( QStringLiteral( "pageLabel" ) );
      QVERIFY( pageLabel != nullptr );
      QVERIFY( !pageLabel->text().isEmpty() );

      QAction *undo = shell.findChild<QAction *>( QStringLiteral( "mActionUndo" ) );
      QVERIFY( undo != nullptr );
      QVERIFY( !undo->isEnabled() ); // null layout: disabled, not broken

      QCOMPARE( iface->lastExportResults(), nullptr );
      iface->close();
      QTest::qWait( 10 );
    }

    // --- A: palette + view integration ----------------------------------------

    void paletteAddItemNowIntegration()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );
      QgsLayoutView *view = shell.view();

      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutMap ),
                                                   layout.get(), &created ) );
      QVERIFY( created != nullptr );
      QCOMPARE( created->scene(), static_cast<QGraphicsScene *>( layout.get() ) ); // in the scene
      QCOMPARE( view->currentLayout(), layout.get() );                             // view unchanged

      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      QVERIFY( items.contains( created ) );

      // addItemNow selects the item; the shell forwards selection to the panel.
      PaleoLayoutItemPanel *panel = shell.findChild<PaleoLayoutItemPanel *>();
      QVERIFY( panel != nullptr );
      QCOMPARE( panel->item(), created );
      QVERIFY( created->isSelected() );
    }

    // goal/ui-experience-polish：Delete 键删除所选 layout 项（QAction 挂壳
    // → QgsLayoutView::deleteSelectedItems；方向键微调是视图内建）。
    void deleteKeyRemovesSelectedItem()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );
      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow(
          guiId( QgsLayoutItemRegistry::LayoutLabel ), layout.get(), &created ) );
      QVERIFY( created && created->isSelected() );
      auto *act = shell.findChild<QAction *>( QStringLiteral( "layoutDeleteSelectedAction" ) );
      QVERIFY( act );
      QCOMPARE( act->shortcut(), QKeySequence( Qt::Key_Delete ) );
      shell.show();
      QVERIFY( QTest::qWaitForWindowExposed( &shell ) );
      shell.view()->setFocus();
      QTest::keyClick( shell.view(), Qt::Key_Delete );
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      QVERIFY( !items.contains( created ) );
    }

    // --- B: panel hosting through the shell -----------------------------------

    void panelHostsBaseWidgetOnSelection()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &created ) );
      auto *label = qobject_cast<QgsLayoutItemLabel *>( created );
      QVERIFY( label != nullptr );

      // Native or fallback: the panel hosts a QgsLayoutItemBaseWidget either way.
      QVERIFY( shell.findChild<QgsLayoutItemBaseWidget *>() != nullptr );

      // Deselecting clears the hosted widget again (placeholder path).
      shell.designerInterface()->selectItems( {} );
      QCOMPARE( shell.findChild<PaleoLayoutItemPanel *>()->item(), nullptr );
      QCOMPARE( shell.findChild<QgsLayoutItemBaseWidget *>(), nullptr );

      // showItemOptions forwards to the panel (no longer a selection-only fallback).
      shell.designerInterface()->showItemOptions( label, false );
      QCOMPARE( shell.findChild<PaleoLayoutItemPanel *>()->item(), label );
      QVERIFY( label->isSelected() );
    }

    // --- D: undo/redo lifecycle through the shell ------------------------------

    void undoRedoLifecycle()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );
      QUndoStack *stack = layout->undoStack()->stack();
      PaleoLayoutItemPanel *panel = shell.findChild<PaleoLayoutItemPanel *>();

      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &created ) );
      auto *label = qobject_cast<QgsLayoutItemLabel *>( created );
      QVERIFY( label != nullptr );
      QCOMPARE( labelCount( *layout ), 1 );
      QCOMPARE( panel->item(), created );

      // Undoable move: beginCommand + attemptMove + endCommand (raw setPos is
      // not undoable; commands serialize the QgsLayoutPoint state).
      const double originalX = label->positionWithUnits().x();
      label->beginCommand( QStringLiteral( "Move label" ) );
      label->attemptMove( QgsLayoutPoint( 100, 100, Qgis::LayoutUnit::Millimeters ) );
      label->endCommand();
      QVERIFY( label->positionWithUnits().x() != originalX );

      // Undo the move -> position restored.
      stack->undo();
      QVERIFY( qAbs( label->positionWithUnits().x() - originalX ) < 0.001 );

      // Redo the move -> moved again.
      stack->redo();
      QVERIFY( qAbs( label->positionWithUnits().x() - 100.0 ) < 0.001 );

      // Undo everything (move, then the add macro) -> item disappears and the
      // panel clears itself (undo rebuilds instances by UUID — old pointers
      // die through QGIS's deferred deletion).
      stack->undo();
      stack->undo();
      QCOMPARE( labelCount( *layout ), 0 );
      QTest::qWait( 10 ); // let the deferred item destruction reach the panel
      QCOMPARE( panel->item(), nullptr );

      // Redo everything -> item is back as a NEW instance; selection wiring
      // (and thus the panel) still works after the recreation.
      stack->redo();
      stack->redo();
      QCOMPARE( labelCount( *layout ), 1 );
      QList<QgsLayoutItemLabel *> labels;
      layout->layoutItems( labels );
      shell.designerInterface()->selectItems( { labels.first() } );
      QCOMPARE( panel->item(), labels.first() );
    }

    // --- page navigation (D's fitInView recipe) --------------------------------

    void pageNavigator()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout( 2 ) );
      PaleoLayoutDesignerShell shell( layout.get() );

      QCOMPARE( layout->pageCollection()->pageCount(), 2 );

      QSpinBox *spin = shell.findChild<QSpinBox *>( QStringLiteral( "pageSpinBox" ) );
      QLabel *label = shell.findChild<QLabel *>( QStringLiteral( "pageLabel" ) );
      QVERIFY( spin != nullptr && label != nullptr );
      QCOMPARE( spin->maximum(), 2 );

      QCOMPARE( shell.view()->currentPage(), 0 );
      QVERIFY( !shell.findChild<QAction *>( QStringLiteral( "actionPreviousPage" ) )->isEnabled() );
      QVERIFY( shell.findChild<QAction *>( QStringLiteral( "actionNextPage" ) )->isEnabled() );

      shell.findChild<QAction *>( QStringLiteral( "actionNextPage" ) )->trigger();
      QTest::qWait( 10 );
      QCOMPARE( shell.view()->currentPage(), 1 );
      QVERIFY( label->text().contains( QLatin1String( "2" ) ) );
      QVERIFY( shell.findChild<QAction *>( QStringLiteral( "actionPreviousPage" ) )->isEnabled() );
      QVERIFY( !shell.findChild<QAction *>( QStringLiteral( "actionNextPage" ) )->isEnabled() );

      // Jump back through the spin box (1-based UI, 0-based API).
      spin->setValue( 1 );
      QTest::qWait( 10 );
      QCOMPARE( shell.view()->currentPage(), 0 );
    }

    // --- C: export through the shell's action object ---------------------------

    void exportPngAndPdf()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      QgsLayoutItem *created = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &created ) );

      // The shell's own export controller (the object behind the File menu).
      auto *exports = shell.findChild<PaleoLayoutExportActions *>();
      QVERIFY( exports != nullptr );
      QCOMPARE( shell.lastExportResults(), nullptr ); // nothing exported yet

      QTemporaryDir dir;
      const QString pngPath = dir.filePath( QStringLiteral( "full.png" ) );
      const auto png = exports->exportLayout( layout.get(), pngPath,
                                              PaleoLayoutExportActions::Format::Png, 96.0,
                                              PaleoLayoutExportActions::PageRange() );
      QVERIFY2( png.ok, qPrintable( png.error ) );
      QVERIFY( QFile( pngPath ).size() > 0 );
      QFile pngFile( pngPath );
      QVERIFY( pngFile.open( QIODevice::ReadOnly ) );
      QCOMPARE( pngFile.read( 8 ), QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) );

      const QString pdfPath = dir.filePath( QStringLiteral( "full.pdf" ) );
      const auto pdf = exports->exportLayout( layout.get(), pdfPath,
                                              PaleoLayoutExportActions::Format::Pdf, 96.0,
                                              PaleoLayoutExportActions::PageRange() );
      QVERIFY2( pdf.ok, qPrintable( pdf.error ) );
      QFile pdfFile( pdfPath );
      QVERIFY( pdfFile.size() > 0 );
      QVERIFY( pdfFile.open( QIODevice::ReadOnly ) );
      QCOMPARE( pdfFile.read( 4 ), QByteArray( "%PDF", 4 ) );

      // First successful export fills the interface's lastExportResults().
      QVERIFY( shell.lastExportResults() != nullptr );
    }

    // --- C: template save/load roundtrip ----------------------------------------

    void templateRoundtrip()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      // Two labels with deliberately distinct frame sizes (labels keep their
      // stored QgsLayoutSize verbatim) + a scale bar. The scale bar's frame is
      // recomputed from its segment/font content on load — QGIS behavior, not
      // a template defect — so only its presence is asserted.
      QgsLayoutItem *small = nullptr;
      QgsLayoutItem *big = nullptr;
      QgsLayoutItem *barItem = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &small ) );
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &big ) );
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutScaleBar ),
                                                   layout.get(), &barItem ) );
      layout->undoStack()->blockCommands( true ); // fixture setup, not user edits
      big->attemptMove( QgsLayoutPoint( 10, 100, Qgis::LayoutUnit::Millimeters ) );
      big->attemptResize( QgsLayoutSize( 120, 30, Qgis::LayoutUnit::Millimeters ) );
      layout->undoStack()->blockCommands( false );

      PaleoLayoutTemplates templates;
      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "roundtrip.qpt" ) );
      const auto saved = templates.saveTemplate( layout.get(), path );
      QVERIFY2( saved.ok, qPrintable( saved.error ) );
      QCOMPARE( saved.itemCount, 3 );

      // Disturb the layout, then load the template back.
      for ( QgsLayoutItem *item : { small, big, barItem } )
        layout->removeLayoutItem( item );
      QTest::qWait( 10 );
      QCOMPARE( labelCount( *layout ), 0 );

      const auto loaded = templates.loadTemplate( layout.get(), path );
      QVERIFY2( loaded.ok, qPrintable( loaded.error ) );
      QCOMPARE( loaded.itemCount, 3 );

      // Content count and label frame sizes survive the roundtrip.
      QList<QgsLayoutItemLabel *> labels;
      layout->layoutItems( labels );
      QCOMPARE( labels.size(), 2 );
      bool sawDefault = false, sawResized = false;
      for ( QgsLayoutItemLabel *label : labels )
      {
        const QgsLayoutSize size = label->sizeWithUnits();
        if ( qAbs( size.width() - 80.0 ) < 0.01 && qAbs( size.height() - 15.0 ) < 0.01 )
          sawDefault = true;
        if ( qAbs( size.width() - 120.0 ) < 0.01 && qAbs( size.height() - 30.0 ) < 0.01 )
        {
          sawResized = true;
          QVERIFY( qAbs( label->positionWithUnits().y() - 100.0 ) < 0.01 ); // position kept too
        }
      }
      QVERIFY( sawDefault );
      QVERIFY( sawResized );

      QList<QgsLayoutItemScaleBar *> bars;
      layout->layoutItems( bars );
      QCOMPARE( bars.size(), 1 );
    }

    void builtinTemplateActionAppliesPageSize()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      // Builtins resolve through $PALEO_LAYOUT_TEMPLATES_DIR when the app dir
      // has no docs/templates ancestor (the case for the test binaries).
      qputenv( "PALEO_LAYOUT_TEMPLATES_DIR", templatesDir().toLocal8Bit() );

      QAction *a0 = shell.findChild<QAction *>( QStringLiteral( "actionApplyBuiltin_a0_landscape" ) );
      QVERIFY( a0 != nullptr );
      a0->trigger();
      QTest::qWait( 10 );

      QCOMPARE( layout->pageCollection()->pageCount(), 1 );
      QgsLayoutItemPage *page = layout->pageCollection()->page( 0 );
      QVERIFY( page != nullptr );
      QCOMPARE( page->pageSize().units(), Qgis::LayoutUnit::Millimeters );
      QVERIFY2( qAbs( page->pageSize().width() - 1189.0 ) < 0.5,
                qPrintable( QString::number( page->pageSize().width() ) ) );
      QVERIFY( qAbs( page->pageSize().height() - 841.0 ) < 0.5 );
    }

    // --- 方向 25：图件内容模板（代码骨架，不依赖 docs/templates）----------------

    void figureBuiltinActionAppliesSkeleton()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      QAction *facies =
          shell.findChild<QAction *>( QStringLiteral( "actionApplyBuiltin_facies@a3_portrait" ) );
      QVERIFY( facies != nullptr );
      facies->trigger();
      QTest::qWait( 10 );

      // 页面规格 + 骨架元素 + 插图（沉积相图独有）。
      QgsLayoutItemPage *page = layout->pageCollection()->page( 0 );
      QVERIFY( page != nullptr );
      QVERIFY( qAbs( page->pageSize().width() - 297.0 ) < 0.5 ); // A3 竖
      QVERIFY( qAbs( page->pageSize().height() - 420.0 ) < 0.5 );
      QList<QgsLayoutItemMap *> maps;
      layout->layoutItems( maps );
      QCOMPARE( maps.size(), 2 ); // map + inset
      auto *mainMap = qobject_cast<QgsLayoutItemMap *>(
          layout->itemById( QStringLiteral( "map" ) ) );
      QVERIFY( mainMap != nullptr );
      QVERIFY( mainMap->grids()->size() == 1 ); // 主图带坐标网格（插图不带）
      QVERIFY( qobject_cast<QgsLayoutItemMap *>( layout->itemById( QStringLiteral( "inset" ) ) )
                   ->overviews()
                   ->size() == 1 );

      // 标题 = 图件种类名。
      QList<QgsLayoutItemLabel *> labels;
      layout->layoutItems( labels );
      bool sawTitle = false;
      for ( QgsLayoutItemLabel *label : labels )
      {
        if ( label->id() == QLatin1String( "title" ) &&
             label->text() == QStringLiteral( "沉积相图" ) )
          sawTitle = true;
      }
      QVERIFY( sawTitle );
    }

    // --- 方向 25：Items 菜单标准图件元素 --------------------------------------

    void standardElementsMenuAddsFactories()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      // 无地图项：菜单动作守卫（不崩、不加项）。
      shell.findChild<QAction *>( QStringLiteral( "actionAddStandardScaleBar" ) )->trigger();
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      QCOMPARE( items.size(), 1 ); // 只剩页面项

      // 加地图项后逐类添加。
      QgsLayoutItem *mapItem = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutMap ),
                                                   layout.get(), &mapItem ) );
      for ( const char *name : { "actionAddStandardScaleBar", "actionAddStandardLegend",
                                 "actionAddStandardNorthArrow", "actionAddStandardGrid",
                                 "actionAddStandardTitleBlock" } )
        shell.findChild<QAction *>( QString::fromLatin1( name ) )->trigger();
      QTest::qWait( 10 );

      layout->layoutItems( items );
      QStringList ids;
      for ( QgsLayoutItem *item : items )
        ids << item->id();
      for ( const char *id : { "scalebar", "legend", "northArrow", "title", "subtitle",
                               "signature" } )
        QVERIFY2( ids.contains( QString::fromLatin1( id ) ), id );
      auto *map = qobject_cast<QgsLayoutItemMap *>( mapItem );
      QVERIFY( map );
      QCOMPARE( map->grids()->size(), 1 ); // 坐标网格挂在地图项上

      // 加完即选：面板宿主新加的标题项。
      PaleoLayoutItemPanel *panel = shell.findChild<PaleoLayoutItemPanel *>();
      QCOMPARE( panel->item()->id(), QStringLiteral( "title" ) );
    }

    // --- 方向 25 M6：元素树双向选中 -------------------------------------------

    void itemTreeSelectionSync()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );
      auto *tree = shell.findChild<PaleoLayoutItemTree *>();
      QVERIFY( tree != nullptr );

      QgsLayoutItem *first = nullptr;
      QgsLayoutItem *second = nullptr;
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &first ) );
      QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                   layout.get(), &second ) );

      // 树 → 版面：模拟树内激活（外部选中路径）。
      emit tree->itemActivated( second );
      QVERIFY( second->isSelected() );
      QCOMPARE( shell.findChild<PaleoLayoutItemPanel *>()->item(), second );

      // 版面 → 树：壳选 first，树高亮同步。
      shell.designerInterface()->selectItems( { first } );
      QCOMPARE( tree->currentItem(), first );

      // 双击 → 属性面板。
      emit tree->itemShowOptions( first );
      QCOMPARE( shell.findChild<PaleoLayoutItemPanel *>()->item(), first );
    }

    // --- 方向 25 M6：对齐/分布 + 吸附开关 --------------------------------------

    void alignDistributeAndSnapping()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      PaleoLayoutDesignerShell shell( layout.get() );

      // 守卫：选中不足不动作、不崩。
      shell.findChild<QAction *>( QStringLiteral( "actionAlignLeft" ) )->trigger();
      shell.findChild<QAction *>( QStringLiteral( "actionDistributeHSpace" ) )->trigger();

      QList<QgsLayoutItem *> labels;
      for ( int i = 0; i < 3; ++i )
      {
        QgsLayoutItem *created = nullptr;
        QVERIFY( PaleoLayoutItemPalette::addItemNow( guiId( QgsLayoutItemRegistry::LayoutLabel ),
                                                     layout.get(), &created ) );
        created->attemptMove( QgsLayoutPoint( 10 + 30 * i, 20 + 15 * i,
                                              Qgis::LayoutUnit::Millimeters ) );
        labels << created;
      }

      // 左对齐（2 项足够）：前两项左边对齐。
      shell.designerInterface()->selectItems( { labels[0], labels[1] } );
      shell.findChild<QAction *>( QStringLiteral( "actionAlignLeft" ) )->trigger();
      QVERIFY( qAbs( labels[0]->mapToScene( labels[0]->rect().topLeft() ).x() -
                     labels[1]->mapToScene( labels[1]->rect().topLeft() ).x() ) < 0.01 );

      // 左边缘等距分布（3 项，位置互异）：相邻左边缘间距一致。
      // 先把对齐步的两项拉开（完全重合是分布的退化输入）。
      labels[1]->attemptMove( QgsLayoutPoint( 45, 35, Qgis::LayoutUnit::Millimeters ) );
      labels[2]->attemptMove( QgsLayoutPoint( 85, 50, Qgis::LayoutUnit::Millimeters ) );
      shell.designerInterface()->selectItems( labels );
      QCOMPARE( layout->selectedLayoutItems().size(), 3 );
      shell.findChild<QAction *>( QStringLiteral( "actionDistributeLeft" ) )->trigger();
      const double l0 = labels[0]->mapToScene( labels[0]->rect().topLeft() ).x();
      const double l1 = labels[1]->mapToScene( labels[1]->rect().topLeft() ).x();
      const double l2 = labels[2]->mapToScene( labels[2]->rect().topLeft() ).x();
      QVERIFY( qAbs( ( l1 - l0 ) - ( l2 - l1 ) ) < 0.01 );

      // 吸附开关拨到 snapper。
      QAction *grid = shell.findChild<QAction *>( QStringLiteral( "actionSnapToGrid" ) );
      QVERIFY( grid && grid->isCheckable() );
      QVERIFY( !layout->snapper().snapToGrid() );
      grid->trigger();
      QVERIFY( layout->snapper().snapToGrid() );
      QAction *items = shell.findChild<QAction *>( QStringLiteral( "actionSnapToItems" ) );
      QVERIFY( items->isChecked() );
      QVERIFY( layout->snapper().snapToItems() ); // 默认开
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutDesignerFull tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutdesigner_full.moc"
