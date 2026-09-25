#pragma once
#include <QObject>
#include <QPointer>
#include <QString>

class QgsVectorLayer;
class QUndoStack;

// ui/edittools/ — PaleoUndoStack: the undo/redo spine for the editing wave.
//
// QGIS 4.2 reality check: QgsVectorLayerEditBuffer has NO undoStack member —
// the native per-layer stack lives on QgsMapLayer::undoStack()
// (qgsmaplayer.h:1555) and edit commands pushed by
// QgsVectorLayer::beginEditCommand/endEditCommand land on exactly that
// stack. This wrapper adds the Paleo contract on top:
//
//   · single-edit-layer discipline: exactly one layer is watched at a time
//     (setLayer). Undo/redo act on the watched layer only — there is no
//     cross-layer undo mixing, matching QgisEditingService's one-session
//     rule and the toolbar's single current edit layer;
//   · switching layers is REFUSED while the watched stack still has
//     undoable commands (switchRefused carries the reason): the caller must
//     save or roll back first, so pending edits can never be silently
//     orphaned on a layer the UI no longer shows;
//   · save/clear policy — DECISION: the stack is cleared on save, natively.
//     Both QgsVectorLayer::commitChanges() and rollBack() clear the layer's
//     undo stack themselves (upstream behavior), and PaleoUndoStack keeps
//     that: after a save there is nothing to redo into a committed dataset.
//     The wrapper only re-emits canUndo/canRedo so the UI follows.
class PaleoUndoStack : public QObject
{
    Q_OBJECT
  public:
    explicit PaleoUndoStack( QObject *parent = nullptr );
    ~PaleoUndoStack() override;

    // Attach to the single watched layer (nullptr detaches — allowed even
    // with pending commands; the stack is simply no longer reachable).
    // Returns false (and emits switchRefused) when \a layer is non-null,
    // differs from the watched layer, and the watched stack canUndo().
    bool setLayer( QgsVectorLayer *layer );
    // out-of-line: QPointer's conversion operator needs the complete type,
    // which this header deliberately only forward-declares
    QgsVectorLayer *layer() const;

    // Undo/redo the last edit command group on the watched layer. No-ops
    // when detached or when the native stack cannot step.
    void undo();
    void redo();

    bool canUndo() const;
    bool canRedo() const;
    int count() const; // command groups on the native stack

  signals:
    void canUndoChanged( bool canUndo );
    void canRedoChanged( bool canRedo );
    void layerChanged( QgsVectorLayer *layer );            // after a successful setLayer
    void switchRefused( const QString &layerId, const QString &reason );

  private:
    void detach();

    // QPointer (not a raw pointer): QGIS layers routinely die on foreign
    // stacks (project teardown, tests) — reads after that must see nullptr.
    QPointer<QgsVectorLayer> mLayer;        // not owned
    QUndoStack *mStack = nullptr;           // not owned (mLayer->undoStack())
};
