#pragma once
#include <QWidget>
#include <QString>
#include <QList>
#include <functional>
#include <memory>

#include "editingundostack.h"

class QgsMapCanvas;
class QgsVectorLayer;
class QToolBar;
class QComboBox;
class QLabel;
class QAction;
class QgisEditingService;

// ui/edittools/ — PaleoEditingToolbar: the 「编辑」 toolbar host.
//
// Shape (per DESIGN.md): a plain QWidget hosting one QToolBar + a layer
// combo + an edit-state label. NOT a dock, NOT modal — a floating tool
// window the shell can reparent. Chrome stays native QToolBar/QToolButton
// with text actions (repo has no icon assets); 4px button spacing (spacing
// xs), Noto Sans SC via the app font, no second signature element. Disabled
// actions carry a reason tooltip (§35 discipline).
//
// Layer list: host-driven — setLayers() feeds candidates (or
// refreshFromProject() pulls every project vector layer). Only layers
// passing layerFilter() are listed. The horizon-constraint business rule
// (活动层位约束下只能编辑该层位的图层) is NOT hard-coded here: the host injects
// it as a filter so the toolbar stays business-agnostic plumbing.
//
// Edit session: edit-tool actions auto-start editing on the selected layer
// (direct layer->startEditing(), or routed through QgisEditingService when
// one is set — busy-marking discipline). saveEditing/cancelEditing stop the
// session (commit / rollBack) and the combo + label re-render the state:
// the editing layer is highlighted in the combo (● marker) and the label
// shows 编辑中 with the layer name.
//
// Undo/redo: the internal PaleoUndoStack follows the selected layer (the
// single current edit layer); its native stack drives the button states.
class PaleoEditingToolbar : public QWidget
{
    Q_OBJECT
  public:
    explicit PaleoEditingToolbar( QgsMapCanvas *canvas, QWidget *parent = nullptr );
    ~PaleoEditingToolbar() override;

    // Candidate layers (host-driven; order preserved). Re-renders the combo;
    // keeps the current selection when the layer is still listed.
    void setLayers( const QList<QgsVectorLayer *> &layers );
    // Convenience: candidates := project vector layers (QgsProject::instance).
    void refreshFromProject();

    // Business-linkage hook: only layers passing the filter are listed /
    // editable. Default: accept every vector layer. Re-applies on set.
    using LayerFilter = std::function<bool( const QgsVectorLayer * )>;
    void setLayerFilter( LayerFilter filter );
    LayerFilter layerFilter() const { return mLayerFilter; }

    // Optional routing of session transitions through the existing
    // QgisEditingService (busy-marking + serialized commit). nullptr →
    // direct startEditing/commitChanges/rollBack.
    void setEditingService( QgisEditingService *service );

    QgsVectorLayer *currentLayer() const;
    // Selects \a layer in the combo (must be a listed candidate) and makes
    // it the canvas current layer.
    void setCurrentLayer( QgsVectorLayer *layer );

    bool isEditing() const;
    PaleoUndoStack *undoStack() const { return mUndoStack.get(); }

    // Action surface (for host wiring and tests).
    QAction *actionSelect() const { return mActionSelect; }
    QAction *actionAddFeature() const { return mActionAddFeature; } // menu: 点/线/面
    QAction *actionAddPoint() const { return mActionAddPoint; }
    QAction *actionAddLine() const { return mActionAddLine; }
    QAction *actionAddPolygon() const { return mActionAddPolygon; }
    QAction *actionReshape() const { return mActionReshape; }
    QAction *actionMove() const { return mActionMove; }
    QAction *actionDeleteFeatures() const { return mActionDeleteFeatures; }
    QAction *actionVertexEdit() const { return mActionVertexEdit; }
    QAction *actionSave() const { return mActionSave; }
    QAction *actionCancel() const { return mActionCancel; }
    QAction *actionUndo() const { return mActionUndo; }
    QAction *actionRedo() const { return mActionRedo; }

    QToolBar *toolBar() const { return mToolBar; }
    QComboBox *layerCombo() const { return mLayerCombo; }
    QLabel *stateLabel() const { return mStateLabel; }

  public slots:
    // Session transitions. startEditing: begin on current layer (refused
    // with messageEmitted-style log when none). saveEditing/cancelEditing:
    // stop the session (emit editingStopped(saved)); the QGIS-native stack
    // clear on commit/rollback updates undo buttons via canUndoChanged.
    bool startEditing();
    bool saveEditing();
    bool cancelEditing();

  signals:
    void editingStarted( const QString &layerId );
    void editingStopped( const QString &layerId, bool saved );
    // A tool landed an edit command group on the current edit layer.
    void featureEdited( const QString &layerId );
    // Emitted for the host log when a session transition is refused.
    void editRefused( const QString &reason );

  private:
    void buildUi();
    void installTool( class QgsMapTool *tool ); // teardown-aware swap on the canvas
    void refreshCombo();                        // rebuild from mLayers ∩ filter
    void updateActionStates();                  // enable/disable + reason tooltips
    void updateStateLabel();
    void onEditToolTriggered();                 // shared: auto-start editing + install
    QgsVectorLayer *editableTarget() const;     // current layer passing gates

    QgsMapCanvas *mCanvas = nullptr;            // not owned
    QList<QgsVectorLayer *> mLayers;            // not owned candidates
    LayerFilter mLayerFilter;                   // default: accept all
    QgisEditingService *mEditingService = nullptr; // not owned, optional
    QgsVectorLayer *mEditLayer = nullptr;       // not owned; layer in session

    QToolBar *mToolBar = nullptr;
    QComboBox *mLayerCombo = nullptr;
    QLabel *mStateLabel = nullptr;
    QAction *mActionSelect = nullptr;
    QAction *mActionAddFeature = nullptr;
    QAction *mActionAddPoint = nullptr;
    QAction *mActionAddLine = nullptr;
    QAction *mActionAddPolygon = nullptr;
    QAction *mActionReshape = nullptr;
    QAction *mActionMove = nullptr;
    QAction *mActionDeleteFeatures = nullptr;
    QAction *mActionVertexEdit = nullptr;
    QAction *mActionSave = nullptr;
    QAction *mActionCancel = nullptr;
    QAction *mActionUndo = nullptr;
    QAction *mActionRedo = nullptr;
    class QgsMapTool *mActiveEditTool = nullptr; // installed tool, not owned
    std::unique_ptr<PaleoUndoStack> mUndoStack;
};
