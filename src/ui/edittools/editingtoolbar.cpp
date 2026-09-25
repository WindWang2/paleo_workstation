#include "editingtoolbar.h"

#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPoint>
#include <QRect>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptool.h>
#include <qgsmaptoolselectutils.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>

#include "../../qgis/qgiseditingservice.h"
#include "editingtools.h"
#include "vertexeditortools.h"

// ui/edittools/ — PaleoEditingToolbar implementation notes:
//
//   · tool lifetime: the installed tool is NOT owned by the canvas — installTool()
//     swaps it out with unsetMapTool() + deleteLater() (deferred, because the
//     swap can run from inside the tool's own editAborted emission);
//   · CAD dock: ONE shared QgsAdvancedDigitizingDockWidget is injected into
//     every capture tool (decision B of the two sanctioned options). The
//     alternative — cadDock = nullptr and let each tool fabricate a
//     canvas-parented dock (resolveCadDock in editingtools.cpp) — leaks one
//     dock per tool swap for the life of the canvas. The contract header pins
//     the member list, so the dock lives as an objectName-tagged child of this
//     widget and is located via findChild();
//   · the ● combo marker and the warning-colored state label are the only
//     "editing in progress" chrome — both plain text, no custom painting.

namespace
{
// DESIGN.md tokens (status colors are always paired with text — never the
// carrier alone): warning #F29900 for the editing label, text-muted #5D6E80
// for the idle label. No other color literals appear in this file.
const QColor kStateEditingColor = QColor( QStringLiteral( "#F29900" ) );
const QColor kStateIdleColor = QColor( QStringLiteral( "#5D6E80" ) );

// File-local select tool: drag a rectangle rubber band, release selects the
// canvas current layer's features intersecting the box. QGIS 4.2.2 build note:
// qgsmaptoolselectutils.h DECLARES selectMultipleFeatures/selectSingleFeature,
// but nm -D libqgis_gui.so shows only setRubberBand/expandSelectRectangle are
// exported (the selectors carry no GUI_EXPORT — same verdict as QgsVertexTool,
// symbols not linkable). Degrade sanctioned: the exported setRubberBand builds
// the map-CRS selection geometry, and selection itself goes through
// QgsVectorLayer::selectByRect — the same native core path the util's
// selectors drive internally. No Q_OBJECT on purpose: the class declares no
// signals of its own and the standalone build recipe does not moc this .cpp
// (emitting the inherited QgsMapTool::messageEmitted needs no moc).
class PaleoSelectTool : public QgsMapTool
{
  public:
    explicit PaleoSelectTool( QgsMapCanvas *canvas )
      : QgsMapTool( canvas )
      , mRubberBand( new QgsRubberBand( canvas, Qgis::GeometryType::Polygon ) )
    {
      setToolName( QCoreApplication::translate( "PaleoEditingToolbar", "Select features" ) );
      setCursor( QCursor( Qt::ArrowCursor ) );
    }

    ~PaleoSelectTool() override
    {
      // QGIS 4: QgsMapCanvasItem is no longer a QObject — delete, don't hide
      delete mRubberBand;
    }

    void activate() override
    {
      QgsMapTool::activate();
    }

    void deactivate() override
    {
      mRubberBand->reset( Qgis::GeometryType::Polygon );
      QgsMapTool::deactivate();
    }

  protected:
    void canvasPressEvent( QgsMapMouseEvent *e ) override
    {
      if ( e->button() != Qt::LeftButton )
        return;
      mPressPos = e->pixelPoint();
      mDragging = true;
    }

    void canvasMoveEvent( QgsMapMouseEvent *e ) override
    {
      if ( !mDragging )
        return;
      QRect rect = QRect( mPressPos, e->pixelPoint() ).normalized();
      QgsMapToolSelectUtils::setRubberBand( canvas(), rect, mRubberBand );
    }

    void canvasReleaseEvent( QgsMapMouseEvent *e ) override
    {
      if ( e->button() != Qt::LeftButton || !mDragging )
        return;
      mDragging = false;
      // setRubberBand converts the device-coords rect into a map-CRS geometry
      // carried by the band; a click without drag yields an empty rect that
      // simply matches nothing (same behavior as QGIS's rectangle select).
      QRect rect = QRect( mPressPos, e->pixelPoint() ).normalized();
      QgsMapToolSelectUtils::setRubberBand( canvas(), rect, mRubberBand );

      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
      if ( !vl )
      {
        emit messageEmitted( QCoreApplication::translate( "PaleoEditingToolbar",
                                   "Select needs a vector layer as the current layer" ),
                             Qgis::MessageLevel::Warning );
        mRubberBand->reset( Qgis::GeometryType::Polygon );
        return;
      }

      QgsRectangle layerRect = mRubberBand->asGeometry().boundingBox(); // map CRS
      const QgsCoordinateReferenceSystem mapCrs = canvas()->mapSettings().destinationCrs();
      if ( mapCrs.isValid() && vl->crs().isValid() && mapCrs != vl->crs() )
      {
        try
        {
          layerRect = QgsCoordinateTransform( mapCrs, vl->crs(),
                                              canvas()->mapSettings().transformContext() )
                          .transformBoundingBox( layerRect );
        }
        catch ( QgsCsException & )
        {
          emit messageEmitted( QCoreApplication::translate( "PaleoEditingToolbar",
                                     "Cannot transform the selection rectangle to the layer CRS" ),
                               Qgis::MessageLevel::Warning );
          mRubberBand->reset( Qgis::GeometryType::Polygon );
          return;
        }
      }

      // Modifier semantics mirror QGIS's rectangle select: plain = replace,
      // Shift = add, Ctrl = subtract.
      Qgis::SelectBehavior behavior = Qgis::SelectBehavior::SetSelection;
      if ( e->modifiers() & Qt::ShiftModifier )
        behavior = Qgis::SelectBehavior::AddToSelection;
      else if ( e->modifiers() & Qt::ControlModifier )
        behavior = Qgis::SelectBehavior::RemoveFromSelection;
      vl->selectByRect( layerRect, behavior );
      mRubberBand->reset( Qgis::GeometryType::Polygon );
    }

  private:
    QgsRubberBand *mRubberBand = nullptr; // owned canvas item (drag box)
    QPoint mPressPos;
    bool mDragging = false;
};

// The one shared CAD dock (see file-header notes). Created lazily so a
// toolbar that never arms a capture tool never builds the dock.
QgsAdvancedDigitizingDockWidget *sharedCadDock( PaleoEditingToolbar *host, QgsMapCanvas *canvas )
{
  const QString name = QStringLiteral( "paleo-editing-cad-dock" );
  QgsAdvancedDigitizingDockWidget *dock = host->findChild<QgsAdvancedDigitizingDockWidget *>( name );
  if ( !dock )
  {
    dock = new QgsAdvancedDigitizingDockWidget( canvas, host );
    dock->setObjectName( name );
    dock->hide(); // the host shell may reparent and show it; hidden by default
  }
  return dock;
}

int comboIndexForLayer( QComboBox *combo, const QgsVectorLayer *layer )
{
  if ( !layer )
    return -1;
  for ( int i = 0; i < combo->count(); ++i )
  {
    if ( qvariant_cast<QgsVectorLayer *>( combo->itemData( i ) ) == layer )
      return i;
  }
  return -1;
}

// Programmatic combo selection: block signals — side effects run through the
// caller (setCurrentLayer), never through the UI notification path.
void selectComboLayer( QComboBox *combo, QgsVectorLayer *layer )
{
  const QSignalBlocker block( combo );
  combo->setCurrentIndex( comboIndexForLayer( combo, layer ) );
}

// Uses the public action accessors only — the contract header's member list
// leaves no room for extra bookkeeping members.
void uncheckEditTools( PaleoEditingToolbar *bar )
{
  const QList<QAction *> actions = { bar->actionSelect(), bar->actionAddPoint(), bar->actionAddLine(),
                                     bar->actionAddPolygon(), bar->actionReshape(), bar->actionMove(),
                                     bar->actionDeleteFeatures(), bar->actionVertexEdit() };
  for ( QAction *a : actions )
    a->setChecked( false );
}
} // namespace

// ---------------------------------------------------------------------------

PaleoEditingToolbar::PaleoEditingToolbar( QgsMapCanvas *canvas, QWidget *parent )
  : QWidget( parent )
  , mCanvas( canvas )
{
  Q_ASSERT( mCanvas ); // the toolbar drives the canvas; a null one is a wiring bug
  mLayerFilter = []( const QgsVectorLayer * ) { return true; };

  buildUi();

  mUndoStack = std::make_unique<PaleoUndoStack>( this );
  connect( mUndoStack.get(), &PaleoUndoStack::canUndoChanged, mActionUndo, &QAction::setEnabled );
  connect( mUndoStack.get(), &PaleoUndoStack::canRedoChanged, mActionRedo, &QAction::setEnabled );
  // Layer-switch refusals (dirty native stack) surface as editRefused reasons.
  connect( mUndoStack.get(), &PaleoUndoStack::switchRefused, this,
           [this]( const QString &, const QString &reason ) { emit editRefused( reason ); } );
  connect( mActionUndo, &QAction::triggered, this, [this] { mUndoStack->undo(); } );
  connect( mActionRedo, &QAction::triggered, this, [this] { mUndoStack->redo(); } );

  updateActionStates();
}

PaleoEditingToolbar::~PaleoEditingToolbar()
{
  // An open edit session is NOT committed or rolled back here — that call
  // belongs to the host. Only tool/canvas-item resources are torn down.
  if ( mActiveEditTool )
  {
    QgsMapTool *tool = mActiveEditTool;
    mActiveEditTool = nullptr;
    mCanvas->unsetMapTool( tool );
    delete tool; // direct delete: the destructor never runs inside tool code
  }
}

void PaleoEditingToolbar::setLayers( const QList<QgsVectorLayer *> &layers )
{
  mLayers = layers;
  refreshCombo();
  updateActionStates();
}

void PaleoEditingToolbar::setProject( QgsProject *project )
{
  mProject = project;
}

void PaleoEditingToolbar::refreshFromProject()
{
  QgsProject *project = mProject ? mProject.data() : QgsProject::instance();
  setLayers( project ? project->layers<QgsVectorLayer *>() : QList<QgsVectorLayer *>() );
}

void PaleoEditingToolbar::setLayerFilter( LayerFilter filter )
{
  mLayerFilter = filter ? filter : []( const QgsVectorLayer * ) { return true; };
  refreshCombo();
  updateActionStates();
}

void PaleoEditingToolbar::setEditingService( QgisEditingService *service )
{
  mEditingService = service;
}

QgsVectorLayer *PaleoEditingToolbar::currentLayer() const
{
  return qvariant_cast<QgsVectorLayer *>( mLayerCombo->currentData() );
}

void PaleoEditingToolbar::setCurrentLayer( QgsVectorLayer *layer )
{
  if ( !layer )
    return; // clearing the selection is the combo's business (empty list)
  if ( comboIndexForLayer( mLayerCombo, layer ) < 0 )
    return; // must be a listed candidate — unlisted layers are ignored

  // An armed session still owns its layer even when the undo stack is empty
  // (tool selected, no geometry yet). Moving the watcher now would let the
  // next startEditing() roll the old layer back.
  if ( mEditLayer && mEditLayer != layer && mEditLayer->isEditable() )
  {
    emit editRefused( tr( "先保存或取消图层 %1 的编辑，再切换图层" ).arg( mEditLayer->name() ) );
    selectComboLayer( mLayerCombo, mEditLayer );
    updateActionStates();
    return;
  }

  if ( mUndoStack->layer() != layer && !mUndoStack->setLayer( layer ) )
  {
    // Refused: the watched stack still has unsaved commands. Snap the combo
    // back to the watched layer (editRefused was relayed by switchRefused).
    selectComboLayer( mLayerCombo, mUndoStack->layer() );
    updateActionStates();
    return;
  }
  mCanvas->setCurrentLayer( layer );
  selectComboLayer( mLayerCombo, layer );
  updateActionStates();
}

bool PaleoEditingToolbar::isEditing() const
{
  return mEditLayer && mEditLayer->isEditable();
}

// ---------------------------------------------------------------------------

void PaleoEditingToolbar::buildUi()
{
  mToolBar = new QToolBar( this );
  // No icon assets exist yet (header contract) — text-only native buttons.
  mToolBar->setToolButtonStyle( Qt::ToolButtonTextOnly );
  if ( QLayout *toolLayout = mToolBar->layout() )
    toolLayout->setSpacing( 4 ); // spacing xs (DESIGN.md toolbar button gap)

  mLayerCombo = new QComboBox( this );
  mLayerCombo->setPlaceholderText( tr( "选择可编辑图层" ) );
  mStateLabel = new QLabel( this );

  auto newToolAction = [this]( const QString &text, const QString &hint ) -> QAction * {
    QAction *a = new QAction( text, this );
    a->setProperty( "hint", hint ); // restored on re-enable (§35 tooltip cycling)
    a->setToolTip( hint );
    a->setCheckable( true );
    mToolBar->addAction( a );
    return a;
  };
  auto newPlainAction = [this]( const QString &text, const QString &hint ) -> QAction * {
    QAction *a = new QAction( text, this );
    a->setProperty( "hint", hint );
    a->setToolTip( hint );
    mToolBar->addAction( a );
    return a;
  };

  mActionSelect = newToolAction( tr( "选择" ), tr( "框选要素（Shift 追加 / Ctrl 去除）" ) );

  // Add-feature entry: one native QToolButton with a menu of the three capture
  // modes (点/线/面). The children are the checkable group members; the parent
  // action only hosts the affordance — InstantPopup keeps the semantics plain.
  mActionAddFeature = new QAction( tr( "添加" ), this );
  const QString addHint = tr( "添加要素：点 / 线 / 面" );
  mActionAddFeature->setProperty( "hint", addHint );
  mActionAddFeature->setToolTip( addHint );
  QMenu *addMenu = new QMenu( mToolBar );
  mActionAddPoint = new QAction( tr( "添加点" ), this );
  mActionAddPoint->setProperty( "hint", tr( "在当前图层添加点要素" ) );
  mActionAddPoint->setToolTip( mActionAddPoint->property( "hint" ).toString() );
  mActionAddPoint->setCheckable( true );
  addMenu->addAction( mActionAddPoint );
  mActionAddLine = new QAction( tr( "添加线" ), this );
  mActionAddLine->setProperty( "hint", tr( "在当前图层添加线要素" ) );
  mActionAddLine->setToolTip( mActionAddLine->property( "hint" ).toString() );
  mActionAddLine->setCheckable( true );
  addMenu->addAction( mActionAddLine );
  mActionAddPolygon = new QAction( tr( "添加面" ), this );
  mActionAddPolygon->setProperty( "hint", tr( "在当前图层添加面要素" ) );
  mActionAddPolygon->setToolTip( mActionAddPolygon->property( "hint" ).toString() );
  mActionAddPolygon->setCheckable( true );
  addMenu->addAction( mActionAddPolygon );
  QToolButton *addButton = new QToolButton( mToolBar );
  addButton->setDefaultAction( mActionAddFeature );
  addButton->setMenu( addMenu );
  addButton->setPopupMode( QToolButton::InstantPopup );
  addButton->setToolButtonStyle( Qt::ToolButtonTextOnly );
  mToolBar->addWidget( addButton );

  mActionReshape = newToolAction( tr( "整形" ), tr( "沿画线重构所选要素的几何" ) );
  mActionMove = newToolAction( tr( "移动" ), tr( "拖动移动所选要素" ) );
  mActionDeleteFeatures = newToolAction( tr( "删除" ), tr( "删除所选要素" ) );
  mActionVertexEdit = newToolAction( tr( "节点" ), tr( "编辑所选要素的节点" ) );

  mToolBar->addSeparator();
  mActionSave = newPlainAction( tr( "保存" ), tr( "提交当前图层的编辑" ) );
  mActionCancel = newPlainAction( tr( "取消" ), tr( "放弃当前图层的编辑" ) );

  mToolBar->addSeparator();
  mActionUndo = newPlainAction( tr( "撤销" ), tr( "撤销上一步编辑" ) );
  mActionRedo = newPlainAction( tr( "重做" ), tr( "重做被撤销的编辑" ) );

  // Exclusive checkable group: select + the capture modes + the edit tools.
  QActionGroup *toolGroup = new QActionGroup( this );
  toolGroup->setExclusive( true );
  const QList<QAction *> grouped = { mActionSelect, mActionAddPoint, mActionAddLine, mActionAddPolygon,
                                     mActionReshape, mActionMove, mActionDeleteFeatures, mActionVertexEdit };
  for ( QAction *a : grouped )
  {
    toolGroup->addAction( a );
    connect( a, &QAction::triggered, this, &PaleoEditingToolbar::onEditToolTriggered );
  }

  connect( mActionSave, &QAction::triggered, this, &PaleoEditingToolbar::saveEditing );
  connect( mActionCancel, &QAction::triggered, this, &PaleoEditingToolbar::cancelEditing );

  // activated() fires on user picks only — programmatic rebuilds re-sync via
  // setCurrentLayer/refreshCombo and never re-enter this path.
  connect( mLayerCombo, &QComboBox::activated, this, [this]( int index ) {
    setCurrentLayer( qvariant_cast<QgsVectorLayer *>( mLayerCombo->itemData( index ) ) );
  } );

  auto *layout = new QHBoxLayout( this );
  layout->setContentsMargins( 8, 8, 8, 8 ); // spacing sm (panel padding)
  layout->setSpacing( 8 );
  layout->addWidget( mLayerCombo );
  layout->addWidget( mToolBar );
  layout->addStretch( 1 );
  layout->addWidget( mStateLabel );
}

void PaleoEditingToolbar::installTool( QgsMapTool *tool )
{
  if ( mActiveEditTool )
  {
    QgsMapTool *old = mActiveEditTool;
    mActiveEditTool = nullptr;
    mCanvas->unsetMapTool( old ); // deactivates when it is the live tool
    old->deleteLater();           // deferred: callers may sit inside the tool's own signal
  }
  if ( tool )
  {
    mActiveEditTool = tool;
    mCanvas->setMapTool( tool );
  }
}

void PaleoEditingToolbar::refreshCombo()
{
  const QString keepId = currentLayer() ? currentLayer()->id() : QString();
  QgsVectorLayer *watched = mUndoStack ? mUndoStack->layer() : nullptr;

  {
    const QSignalBlocker block( mLayerCombo );
    mLayerCombo->clear();
    for ( QgsVectorLayer *lyr : std::as_const( mLayers ) )
    {
      if ( mLayerFilter && !mLayerFilter( lyr ) )
        continue;
      const QString text = ( lyr == mEditLayer && lyr->isEditable() )
                               ? tr( "● %1" ).arg( lyr->name() ) // session marker
                               : lyr->name();
      mLayerCombo->addItem( text, QVariant::fromValue( lyr ) );
    }
    // restore: previous selection first, else the edit layer, else the first
    // item — priority matters when both are listed in either order
    int idx = -1;
    for ( int i = 0; i < mLayerCombo->count() && idx < 0; ++i )
    {
      QgsVectorLayer *lyr = qvariant_cast<QgsVectorLayer *>( mLayerCombo->itemData( i ) );
      if ( !keepId.isEmpty() && lyr && lyr->id() == keepId )
        idx = i;
    }
    for ( int i = 0; i < mLayerCombo->count() && idx < 0; ++i )
    {
      if ( qvariant_cast<QgsVectorLayer *>( mLayerCombo->itemData( i ) ) == mEditLayer )
        idx = i;
    }
    if ( idx < 0 && mLayerCombo->count() > 0 )
      idx = 0;
    mLayerCombo->setCurrentIndex( idx );
  }

  // The rebuild can change what "current" means — keep canvas/undo watching
  // whatever the combo now shows (a dirty stack keeps its watched layer).
  QgsVectorLayer *sel = currentLayer();
  if ( sel != watched )
  {
    if ( !mUndoStack->setLayer( sel ) )
    {
      sel = mUndoStack->layer();
      selectComboLayer( mLayerCombo, sel );
    }
    mCanvas->setCurrentLayer( sel );
  }
}

void PaleoEditingToolbar::updateActionStates()
{
  // Enable gate: candidates exist (mLayers non-empty). With candidates but an
  // empty filtered combo the buttons stay LIVE so the gesture earns feedback
  // (editRefused at trigger time) instead of a dead button; only a toolbar
  // with no candidates at all disables them, with the §35 reason tooltip.
  // editableTarget() remains the authoritative trigger-time gate.
  const bool hasCandidates = !mLayers.isEmpty();
  const QString noTarget = tr( "先选择一个可编辑图层" );
  const QList<QAction *> toolActions = { mActionSelect, mActionAddFeature, mActionAddPoint,
                                         mActionAddLine, mActionAddPolygon, mActionReshape,
                                         mActionMove, mActionDeleteFeatures, mActionVertexEdit };
  for ( QAction *a : toolActions )
  {
    a->setEnabled( hasCandidates );
    a->setToolTip( hasCandidates ? a->property( "hint" ).toString() : noTarget );
  }

  const bool editing = isEditing();
  const QString noSession = tr( "当前没有进行中的编辑会话" );
  mActionSave->setEnabled( editing );
  mActionSave->setToolTip( editing ? mActionSave->property( "hint" ).toString() : noSession );
  mActionCancel->setEnabled( editing );
  mActionCancel->setToolTip( editing ? mActionCancel->property( "hint" ).toString() : noSession );

  mActionUndo->setEnabled( mUndoStack && mUndoStack->canUndo() );
  mActionUndo->setToolTip( ( mUndoStack && mUndoStack->canUndo() )
                               ? mActionUndo->property( "hint" ).toString()
                               : tr( "没有可撤销的编辑" ) );
  mActionRedo->setEnabled( mUndoStack && mUndoStack->canRedo() );
  mActionRedo->setToolTip( ( mUndoStack && mUndoStack->canRedo() )
                               ? mActionRedo->property( "hint" ).toString()
                               : tr( "没有可重做的编辑" ) );

  updateStateLabel();
}

void PaleoEditingToolbar::updateStateLabel()
{
  QPalette palette = mStateLabel->palette();
  if ( isEditing() )
  {
    // Warning color always carries text (DESIGN.md: 语义色永远配文字)
    mStateLabel->setText( tr( "编辑中：%1" ).arg( mEditLayer->name() ) );
    palette.setColor( QPalette::WindowText, kStateEditingColor );
  }
  else
  {
    mStateLabel->setText( tr( "未编辑" ) );
    palette.setColor( QPalette::WindowText, kStateIdleColor );
  }
  mStateLabel->setPalette( palette );
}

QgsVectorLayer *PaleoEditingToolbar::editableTarget() const
{
  QgsVectorLayer *lyr = currentLayer();
  if ( !lyr || ( mLayerFilter && !mLayerFilter( lyr ) ) )
    return nullptr;
  return lyr;
}

void PaleoEditingToolbar::onEditToolTriggered()
{
  QAction *action = qobject_cast<QAction *>( sender() );
  if ( !action )
    return;

  // NB: the exclusive QActionGroup RE-CHECKS a triggered action, so an armed
  // tool cannot be toggled off by clicking it again — a re-trigger lands here
  // checked and re-arms (same tool kept, see the early-out below). Tools are
  // dropped by switching (group swap) or ending the session (save/cancel).

  QgsVectorLayer *target = editableTarget();
  if ( !target )
  {
    emit editRefused( tr( "先选择一个可编辑图层" ) );
    action->setChecked( false );
    updateActionStates();
    return;
  }
  if ( !target->isEditable() && !startEditing() )
  {
    action->setChecked( false ); // startEditing refused — leave the canvas as-is
    updateActionStates();
    return;
  }
  if ( mActiveEditTool && mActiveEditTool->property( "paleo-action" ).value<QAction *>() == action )
    return; // already armed with this exact tool — idempotent re-trigger

  // One wiring lambda for the whole family: the tool classes share the signal
  // contract but not a common Qt ancestor exposing it, so the concrete pointer
  // type drives the connect.
  auto wireAborted = [this, action]( auto *t ) -> QgsMapTool * {
    connect( t, &std::remove_pointer_t<decltype( t )>::editAborted, this, [this, action] {
      installTool( nullptr ); // unsets + deleteLater (may run inside the emit)
      action->setChecked( false );
    } );
    return t;
  };

  QgsMapTool *tool = nullptr;
  QgsAdvancedDigitizingDockWidget *cadDock = sharedCadDock( this, mCanvas );
  if ( action == mActionSelect )
    tool = new PaleoSelectTool( mCanvas ); // no abort wiring: selection never aborts
  else if ( action == mActionAddPoint )
    tool = wireAborted( new PaleoAddFeatureTool( mCanvas, cadDock, QgsMapToolCapture::CapturePoint, target ) );
  else if ( action == mActionAddLine )
    tool = wireAborted( new PaleoAddFeatureTool( mCanvas, cadDock, QgsMapToolCapture::CaptureLine, target ) );
  else if ( action == mActionAddPolygon )
    tool = wireAborted( new PaleoAddFeatureTool( mCanvas, cadDock, QgsMapToolCapture::CapturePolygon, target ) );
  else if ( action == mActionReshape )
    tool = wireAborted( new PaleoReshapeTool( mCanvas, cadDock, target ) );
  else if ( action == mActionMove )
    tool = wireAborted( new PaleoMoveTool( mCanvas, target ) );
  else if ( action == mActionDeleteFeatures )
    tool = wireAborted( new PaleoDeleteFeatureTool( mCanvas, target ) );
  else if ( action == mActionVertexEdit )
    tool = wireAborted( new PaleoVertexTool( mCanvas, target ) );

  if ( !tool )
  {
    action->setChecked( false );
    return;
  }

  // Relay edit landings; a string-based connect crosses the distinct classes
  // (all declare featureEdited(QString) with identical semantics). The select
  // tool has no such signal — it owns no Q_OBJECT and never edits.
  if ( action != mActionSelect )
    connect( tool, SIGNAL( featureEdited( QString ) ), this, SIGNAL( featureEdited( QString ) ) );

  tool->setProperty( "paleo-action", QVariant::fromValue( action ) ); // idempotence tag

  // The tool is explicitly bound to the target; the canvas current layer is
  // synced too so current-layer fallbacks (and the select utils) agree.
  mCanvas->setCurrentLayer( target );
  installTool( tool );
}

bool PaleoEditingToolbar::startEditing()
{
  QgsVectorLayer *target = editableTarget();
  if ( !target )
  {
    emit editRefused( tr( "先选择一个可编辑图层" ) );
    return false;
  }
  if ( target->isEditable() )
  {
    if ( mEditLayer == target )
      return true; // our own live session — idempotent
    emit editRefused( tr( "图层 %1 已在编辑中" ).arg( target->name() ) );
    return false; // foreign session: never adopted silently
  }

  if ( mEditLayer && mEditLayer->isEditable() && mEditLayer != target )
  {
    emit editRefused( tr( "先保存或取消图层 %1 的编辑，再编辑其他图层" ).arg( mEditLayer->name() ) );
    return false;
  }

  QString err;
  if ( mEditingService )
  {
    if ( !mEditingService->beginEdit( target, &err ) )
    {
      emit editRefused( err );
      return false;
    }
  }
  else if ( !target->startEditing() )
  {
    emit editRefused( tr( "无法开始编辑图层 %1" ).arg( target->name() ) );
    return false;
  }

  mEditLayer = target;
  emit editingStarted( target->id() );
  refreshCombo(); // ● marker moves to the session layer
  updateActionStates();
  return true;
}

bool PaleoEditingToolbar::saveEditing()
{
  QgsVectorLayer *layer = mEditLayer;
  if ( !layer || !layer->isEditable() )
  {
    emit editRefused( tr( "当前没有进行中的编辑会话" ) );
    return false;
  }

  QString err;
  const bool ok = mEditingService ? mEditingService->commitEdit( layer, &err )
                                  : layer->commitChanges();
  if ( !ok )
  {
    // Provider refused the commit: KEEP the session (the user may fix and
    // retry or roll back); only a rollBack stops with saved=false.
    emit editRefused( mEditingService ? err : tr( "提交图层 %1 的编辑失败" ).arg( layer->name() ) );
    return false;
  }

  const QString id = layer->id();
  mEditLayer = nullptr;
  installTool( nullptr ); // the edit tool rode the session — park the canvas
  uncheckEditTools( this );
  emit editingStopped( id, true );
  refreshCombo();
  updateActionStates(); // commit cleared the native stack → undo/redo off
  return true;
}

bool PaleoEditingToolbar::cancelEditing()
{
  QgsVectorLayer *layer = mEditLayer;
  if ( !layer || !layer->isEditable() )
  {
    emit editRefused( tr( "当前没有进行中的编辑会话" ) );
    return false;
  }

  const bool ok = mEditingService ? mEditingService->rollbackEdit( layer )
                                  : layer->rollBack();
  if ( !ok )
  {
    emit editRefused( tr( "回滚图层 %1 失败" ).arg( layer->name() ) );
    return false;
  }

  const QString id = layer->id();
  mEditLayer = nullptr;
  installTool( nullptr );
  uncheckEditTools( this );
  emit editingStopped( id, false );
  refreshCombo();
  updateActionStates();
  return true;
}
