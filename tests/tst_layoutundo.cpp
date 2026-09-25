#include <QtTest>
#include <QCoreApplication>
#include <QMenu>
#include <QToolBar>
#include <QWidget>

#include "../src/ui/layout/layoutundostack.h"
#include "../src/qgis/qgisruntime.h"

#include <QUndoStack>

#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpoint.h>
#include <qgslayoutsize.h>
#include <qgslayoutundostack.h>
#include <qgslayoutview.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>

// Task D: PaleoLayoutUndoStack wraps QgsLayout::undoStack() (never creating or
// clearing a stack) and exposes QUndoStack-flavoured undo/redo actions plus
// forwarded can{Undo,Redo}Changed signals. These tests pin that contract over
// a real QgsPrintLayout, offscreen.
//
// QGIS 4.2 undo channels used here (all verified against the installed 4.2.2):
//  - add: QgsLayout::addLayoutItem() pushes QgsLayoutItemAddItemCommand itself.
//  - remove: QgsLayout::removeLayoutItem() pushes the delete command itself.
//  - modify: QgsLayoutItem::beginCommand(text) + attempt*() mutation +
//    endCommand(). Raw setPos()/setRect() are NOT undoable — commands
//    serialize the item's QgsLayoutPoint/QgsLayoutSize state, which only
//    attemptMove()/attemptResize()/attemptSetSceneRect() update.
//  - untracked setup: QgsLayoutUndoStack::blockCommands() suppresses the
//    automatic pushes above (used for fixtures and page creation).
//  - page switch: QgsLayoutView has NO showPage(int) in 4.2 — navigate via
//    fitInView(page->mapRectToScene(page->rect())) + viewChanged().
class TestLayoutUndo : public QObject
{
    Q_OBJECT

  private:
    // Two real pages (A4 landscape), created OUTSIDE the undo history: page
    // creation routes through QgsLayout::addLayoutItem and would otherwise
    // land on the undo stack. A fresh QgsPrintLayout has NO pages, and
    // extendByNewPage() inherits the size of the (nonexistent) last page, so
    // the first page must be created explicitly with a size.
    void makeTwoPages( QgsPrintLayout &layout )
    {
      layout.undoStack()->blockCommands( true );
      auto *page0 = new QgsLayoutItemPage( &layout );
      page0->setPageSize( QgsLayoutSize( 297, 210 ) );
      layout.pageCollection()->addPage( page0 );
      layout.pageCollection()->extendByNewPage();
      layout.undoStack()->blockCommands( false );
      QCOMPARE( layout.pageCollection()->pageCount(), 2 );
    }

    static int labelCount( QgsLayout &layout )
    {
      QList<QgsLayoutItemLabel *> labels;
      layout.layoutItems<QgsLayoutItemLabel>( labels );
      return labels.count();
    }

    // Fixture setup: add an item WITHOUT touching the undo history. This is
    // the sanctioned QGIS channel for programmatic, non-user setup
    // (QgsLayoutUndoStack::blockCommands); it keeps the tests' undo
    // assertions about exactly one command. Verified: the blocked add leaves
    // stack count/canUndo unchanged while the item is added.
    static QgsLayoutItemLabel *addLabelUntracked( QgsPrintLayout &layout, const QString &text )
    {
      layout.undoStack()->blockCommands( true );
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( text );
      layout.addLayoutItem( label );
      layout.undoStack()->blockCommands( false );
      return label;
    }

    static QgsLayoutItemLabel *firstLabel( QgsLayout &layout )
    {
      QList<QgsLayoutItemLabel *> labels;
      layout.layoutItems<QgsLayoutItemLabel>( labels );
      return labels.isEmpty() ? nullptr : labels.first();
    }

  private slots:
    // --- actions: shortcuts, initial state, stack identity ----------------

    void actionsHaveShortcutsAndStartDisabled()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QVERIFY( undo.undoAction() != nullptr );
      QVERIFY( undo.redoAction() != nullptr );
      QCOMPARE( undo.undoAction()->shortcut(), QKeySequence( QKeySequence::StandardKey::Undo ) );
      QCOMPARE( undo.redoAction()->shortcut(), QKeySequence( QKeySequence::StandardKey::Redo ) );
      QVERIFY( !undo.undoAction()->shortcut().isEmpty() );
      QVERIFY( !undo.redoAction()->shortcut().isEmpty() );
      QVERIFY( !undo.undoAction()->text().isEmpty() );
      QVERIFY( !undo.redoAction()->text().isEmpty() );

      QVERIFY( !undo.canUndo() );
      QVERIFY( !undo.canRedo() );
      QVERIFY( !undo.undoAction()->isEnabled() );
      QVERIFY( !undo.redoAction()->isEnabled() );
    }

    void wrapsLayoutStackWithoutRecreatingOrClearing()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );

      // Push history BEFORE the wrapper exists: wrapping must neither build a
      // fresh stack nor clear the existing history.
      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "history" ) );
      layout.addLayoutItem( label );
      QVERIFY( layout.undoStack()->stack()->canUndo() );

      PaleoLayoutUndoStack undo( &layout );
      QCOMPARE( undo.stack(), layout.undoStack()->stack() ); // same stack, by identity
      QVERIFY( undo.canUndo() );
    }

    // --- add / remove channels ---------------------------------------------

    void addItemEntersUndoStackAndRoundTrips()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QSignalSpy canUndoSpy( &undo, &PaleoLayoutUndoStack::canUndoChanged );

      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "added" ) );
      layout.addLayoutItem( label );

      // Channel: addLayoutItem() pushes the add command itself.
      QVERIFY( undo.canUndo() );
      QVERIFY( undo.undoAction()->isEnabled() );
      QCOMPARE( labelCount( layout ), 1 );
      QCOMPARE( canUndoSpy.count(), 1 );
      QCOMPARE( canUndoSpy.takeFirst().at( 0 ).toBool(), true );

      undo.undo();
      QCOMPARE( labelCount( layout ), 0 );
      QVERIFY( !undo.canUndo() );
      QVERIFY( undo.canRedo() );
      QVERIFY( undo.redoAction()->isEnabled() );
      QVERIFY( !undo.undoAction()->isEnabled() );

      undo.redo();
      QCOMPARE( labelCount( layout ), 1 );
      QVERIFY( undo.canUndo() );
      QVERIFY( !undo.canRedo() );
    }

    void removeItemEntersUndoStackAndUndoRestores()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      addLabelUntracked( layout, QStringLiteral( "doomed" ) );
      QCOMPARE( labelCount( layout ), 1 );
      QVERIFY( !undo.canUndo() );

      // Channel: removeLayoutItem() pushes the delete command itself.
      // Note: removeLayoutItem deletes the item; re-query after each step.
      if ( QgsLayoutItemLabel *it = firstLabel( layout ) )
        layout.removeLayoutItem( it );
      QCOMPARE( labelCount( layout ), 0 );
      QVERIFY( undo.canUndo() );

      undo.undo();
      QCOMPARE( labelCount( layout ), 1 ); // recreated from the saved state

      undo.redo();
      QCOMPARE( labelCount( layout ), 0 );
    }

    // --- item modification channel ------------------------------------------

    void moveUndoRestoresPosition()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QgsLayoutItemLabel *label = addLabelUntracked( layout, QStringLiteral( "mover" ) );
      QVERIFY( !undo.canUndo() );

      const QPointF originalPos( 0, 0 );
      QCOMPARE( label->pos(), originalPos );

      // Channel note: raw QGraphicsItem::setPos() is NOT undoable here — the
      // command serializes QgsLayoutItem's own position state, which only the
      // attempt*() APIs update (and a bare setPos inside beginCommand is even
      // rejected on read-back). attemptMove() is the verified channel.
      label->beginCommand( QStringLiteral( "Move item" ) );
      label->attemptMove( QgsLayoutPoint( 30, 40 ) );
      label->endCommand();
      QCOMPARE( label->pos(), QPointF( 30, 40 ) );

      undo.undo();
      QCOMPARE( label->pos(), originalPos );

      undo.redo();
      QCOMPARE( label->pos(), QPointF( 30, 40 ) );
    }

    void resizeUndoRestoresRect()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QgsLayoutItemLabel *label = addLabelUntracked( layout, QStringLiteral( "resizer" ) );
      QVERIFY( !undo.canUndo() );

      // Establish a non-trivial baseline rect (outside any command).
      label->attemptSetSceneRect( QRectF( 5, 6, 120, 40 ) );
      const QRectF baseline( label->pos(), label->rect().size() );
      QCOMPARE( baseline, QRectF( 5, 6, 120, 40 ) );

      label->beginCommand( QStringLiteral( "Resize item" ) );
      label->attemptSetSceneRect( QRectF( 10, 12, 90, 50 ) );
      label->endCommand();
      QCOMPARE( QRectF( label->pos(), label->rect().size() ), QRectF( 10, 12, 90, 50 ) );

      undo.undo();
      QCOMPARE( QRectF( label->pos(), label->rect().size() ), baseline );

      undo.redo();
      QCOMPARE( QRectF( label->pos(), label->rect().size() ), QRectF( 10, 12, 90, 50 ) );
    }

    void macroCollapsesCommandsIntoOneStep()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QgsLayoutItemLabel *label = addLabelUntracked( layout, QStringLiteral( "macro" ) );
      QVERIFY( !undo.canUndo() );

      QgsLayoutUndoStack *layoutStack = layout.undoStack();
      layoutStack->beginMacro( QStringLiteral( "Move and resize" ) );
      label->beginCommand( QStringLiteral( "Move item" ) );
      label->setPos( QPointF( 15, 25 ) );
      label->endCommand();
      label->beginCommand( QStringLiteral( "Resize item" ) );
      label->attemptSetSceneRect( QRectF( 15, 25, 80, 30 ) );
      label->endCommand();
      layoutStack->endMacro();

      QCOMPARE( QRectF( label->pos(), label->rect().size() ), QRectF( 15, 25, 80, 30 ) );

      // One undo() rolls the whole macro back...
      undo.undo();
      QCOMPARE( QRectF( label->pos(), label->rect().size() ), QRectF( 0, 0, 0, 0 ) );
      QVERIFY( !undo.canUndo() ); // ...and it was the only step on the stack
    }

    // --- persistence across view page switches ------------------------------

    void undoSurvivesViewPageSwitch()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      makeTwoPages( layout );
      PaleoLayoutUndoStack undo( &layout );

      QgsLayoutView view;
      view.resize( 800, 600 );
      view.setCurrentLayout( &layout );
      view.show();
      QCoreApplication::processEvents();
      view.zoomFull();
      QCoreApplication::processEvents();
      QCOMPARE( view.currentPage(), 0 );

      auto *label = addLabelUntracked( layout, QStringLiteral( "page zero" ) );
      QVERIFY( !undo.canUndo() );

      label->beginCommand( QStringLiteral( "Move item" ) );
      label->attemptMove( QgsLayoutPoint( 55, 66 ) );
      label->endCommand();
      QCOMPARE( QPointF( label->pos() ), QPointF( 55, 66 ) );

      // QGIS 4.2 has no QgsLayoutView::showPage(int): navigate the page by
      // fitting its scene rect and letting the view recompute the current page.
      if ( QgsLayoutItemPage *page1 = layout.pageCollection()->page( 1 ) )
      {
        view.fitInView( page1->mapRectToScene( page1->rect() ), Qt::KeepAspectRatio );
        QCoreApplication::processEvents();
        view.viewChanged();
        QCoreApplication::processEvents();
      }
      QCOMPARE( view.currentPage(), 1 );

      // The stack belongs to the layout, not the view: after switching to
      // page 2 the last command is still undoable and reverts correctly.
      QVERIFY( undo.canUndo() );
      undo.undo();
      QCOMPARE( QPointF( label->pos() ), QPointF( 0, 0 ) );
      QCOMPARE( view.currentPage(), 1 ); // undo did not move the view
    }

    // --- signal forwarding ---------------------------------------------------

    void forwardsCanChangedSignals()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      QSignalSpy undoSpy( &undo, &PaleoLayoutUndoStack::canUndoChanged );
      QSignalSpy redoSpy( &undo, &PaleoLayoutUndoStack::canRedoChanged );

      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "signals" ) );
      layout.addLayoutItem( label );

      // Qt 6.11's QUndoStack emits can{Undo,Redo}Changed on every index change
      // (even when the value did not flip), so assert on the delivered values
      // rather than exact emission counts.
      QVERIFY( undoSpy.count() >= 1 );
      QCOMPARE( undoSpy.last().at( 0 ).toBool(), true );
      QVERIFY( redoSpy.count() >= 1 );
      QCOMPARE( redoSpy.last().at( 0 ).toBool(), false );

      undo.undo();
      QVERIFY( undoSpy.count() >= 1 );
      QCOMPARE( undoSpy.last().at( 0 ).toBool(), false );
      QVERIFY( redoSpy.count() >= 1 );
      QCOMPARE( redoSpy.last().at( 0 ).toBool(), true );
    }

    // --- menu / widget attachment habits --------------------------------------

    void attachMenuAndWidgetHabits()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      // Menu with prior content gets a separator before the undo/redo group.
      QMenu menu;
      menu.addAction( QStringLiteral( "Existing" ) );
      undo.attachMenu( &menu );
      const QList<QAction *> acts = menu.actions();
      QVERIFY( acts.size() >= 3 );
      QCOMPARE( acts.at( acts.size() - 2 ), undo.undoAction() );
      QCOMPARE( acts.at( acts.size() - 1 ), undo.redoAction() );
      QVERIFY( acts.at( acts.size() - 3 )->isSeparator() );

      // Attaching twice does not duplicate the actions.
      undo.attachMenu( &menu );
      int undoCount = 0;
      for ( QAction *a : menu.actions() )
        if ( a == undo.undoAction() )
          ++undoCount;
      QCOMPARE( undoCount, 1 );

      // Empty menu: no separator, just the group.
      QMenu freshMenu;
      undo.attachMenu( &freshMenu );
      QCOMPARE( freshMenu.actions().size(), 2 );

      // Toolbar: undo/redo followed by a trailing separator (shell habit).
      QToolBar toolbar;
      undo.attachWidget( &toolbar );
      QCOMPARE( toolbar.actions().size(), 3 );
      QCOMPARE( toolbar.actions().at( 0 ), undo.undoAction() );
      QCOMPARE( toolbar.actions().at( 1 ), undo.redoAction() );
      QVERIFY( toolbar.actions().at( 2 )->isSeparator() );

      // Generic widget: actions attached, no crash.
      QWidget widget;
      undo.attachWidget( &widget );
      QVERIFY( widget.actions().contains( undo.undoAction() ) );
      QVERIFY( widget.actions().contains( undo.redoAction() ) );
    }

    void actionsTriggerUndoRedo()
    {
      QgsProject project;
      QgsPrintLayout layout( &project );
      PaleoLayoutUndoStack undo( &layout );

      auto *label = new QgsLayoutItemLabel( &layout );
      label->setText( QStringLiteral( "trigger" ) );
      layout.addLayoutItem( label );

      // The QActions are the real entry point — they must drive the stack.
      undo.undoAction()->trigger();
      QCOMPARE( labelCount( layout ), 0 );
      QVERIFY( undo.redoAction()->isEnabled() );

      undo.redoAction()->trigger();
      QCOMPARE( labelCount( layout ), 1 );
      QVERIFY( undo.undoAction()->isEnabled() );
    }

    // --- null-safety -----------------------------------------------------------

    void nullLayoutIsSafe()
    {
      PaleoLayoutUndoStack undo( nullptr );

      QCOMPARE( undo.stack(), nullptr );
      QVERIFY( !undo.canUndo() );
      QVERIFY( !undo.canRedo() );
      QVERIFY( undo.undoAction() != nullptr );
      QVERIFY( undo.redoAction() != nullptr );
      QVERIFY( !undo.undoAction()->isEnabled() );
      QVERIFY( !undo.redoAction()->isEnabled() );
      QVERIFY( !undo.undoAction()->shortcut().isEmpty() );

      QSignalSpy undoSpy( &undo, &PaleoLayoutUndoStack::canUndoChanged );
      undo.undo(); // no-op, must not crash
      undo.redo();
      QCOMPARE( undoSpy.count(), 0 );

      QMenu menu;
      undo.attachMenu( &menu );
      QCOMPARE( menu.actions().size(), 2 ); // stub actions still attachable
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestLayoutUndo tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_layoutundo.moc"
