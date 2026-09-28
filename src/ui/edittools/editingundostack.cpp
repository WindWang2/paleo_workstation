// 层：视图
#include "editingundostack.h"

#include <QMetaType>
#include <QUndoStack>

#include <qgsvectorlayer.h>

// ui/edittools/ — PaleoUndoStack implementation notes:
//
//   · the native stack is QgsMapLayer::undoStack() (QgsVectorLayerEditBuffer
//     has none in QGIS 4.2); beginEditCommand()/endEditCommand() groups are
//     pushed onto exactly that stack, so the wrapper only forwards and gates;
//   · save/clear policy: QgsVectorLayer::commitChanges() and rollBack() clear
//     that stack themselves — after a save there is nothing left to redo into
//     the committed dataset, so no extra clearing happens here. The wrapper
//     merely re-emits canUndo/canRedo (the native clear emits them) so the UI
//     follows.

PaleoUndoStack::PaleoUndoStack( QObject *parent )
  : QObject( parent )
{
  // layerChanged(QgsVectorLayer*) must cross QVariant boundaries (signal
  // spies, queued cross-thread connections) — register the pointer type so
  // it always marshals instead of arriving as a null QVariant.
  qRegisterMetaType<QgsVectorLayer *>( "QgsVectorLayer*" );
}

PaleoUndoStack::~PaleoUndoStack()
{
  // Both pointers are borrowed (the layer owns its QUndoStack) and Qt severs
  // every connection during teardown — nothing to release by hand.
}

QgsVectorLayer *PaleoUndoStack::layer() const
{
  return mLayer; // QPointer conversion; reads null once the layer died
}

bool PaleoUndoStack::setLayer( QgsVectorLayer *layer )
{
  if ( layer == mLayer.data() )
    return true; // idempotent: same layer, or detach while already detached

  // Single-edit-layer discipline: never orphan undoable, unsaved command
  // groups on the watched layer. A commitChanges()/rollBack() clears the
  // native stack, so a saved (or reverted) layer always passes this gate.
  // nullptr never lands here — detaching is always allowed.
  if ( layer && mStack && mStack->canUndo() )
  {
    emit switchRefused( mLayer->id(),
                        tr( "The current edit layer still has unsaved edit commands; save or roll back before switching layers." ) );
    return false;
  }

  const bool hadUndo = canUndo();
  const bool hadRedo = canRedo();
  const bool wasWatching = ( mLayer != nullptr );
  detach();
  if ( hadUndo )
    emit canUndoChanged( false ); // keep consumers in sync with the detach
  if ( hadRedo )
    emit canRedoChanged( false );

  if ( !layer )
  {
    if ( wasWatching )
      emit layerChanged( nullptr );
    return true;
  }

  mLayer = layer;
  mStack = layer->undoStack(); // not owned; the stack the edit commands land on
  connect( mStack, &QUndoStack::canUndoChanged, this, &PaleoUndoStack::canUndoChanged );
  connect( mStack, &QUndoStack::canRedoChanged, this, &PaleoUndoStack::canRedoChanged );

  // Foreign destruction (project teardown, layers living on test stacks):
  // clear the borrowed pointers without touching any QgsVectorLayer/QUndoStack
  // API — destroyed() fires from ~QObject with parts of the object already
  // gone. Qt auto-severs the stack connections as the dying layer's children
  // (the QUndoStack among them) are deleted.
  connect( layer, &QObject::destroyed, this, [this]
  {
    mLayer = nullptr; // the QPointer has self-nulled; make it explicit
    mStack = nullptr; // child of the layer — dies with it
    emit canUndoChanged( false );
    emit canRedoChanged( false );
    emit layerChanged( nullptr );
  } );

  emit layerChanged( layer );
  // Sync state for layers attached mid-session (e.g. already carrying
  // undoable command groups from an earlier wave).
  emit canUndoChanged( mStack->canUndo() );
  emit canRedoChanged( mStack->canRedo() );
  return true;
}

void PaleoUndoStack::undo()
{
  if ( !mStack || !mStack->canUndo() )
    return; // detached, or the native stack cannot step
  mStack->undo();
}

void PaleoUndoStack::redo()
{
  if ( !mStack || !mStack->canRedo() )
    return;
  mStack->redo();
}

bool PaleoUndoStack::canUndo() const
{
  return mStack && mStack->canUndo();
}

bool PaleoUndoStack::canRedo() const
{
  return mStack && mStack->canRedo();
}

int PaleoUndoStack::count() const
{
  return mStack ? mStack->count() : 0;
}

void PaleoUndoStack::detach()
{
  if ( QUndoStack *stack = mStack )
  {
    disconnect( stack, nullptr, this, nullptr ); // stop signal forwarding
    mStack = nullptr;
  }
  if ( QgsVectorLayer *layer = mLayer.data() )
  {
    disconnect( layer, nullptr, this, nullptr ); // drop the destroyed() hook
    mLayer = nullptr;
  }
}
