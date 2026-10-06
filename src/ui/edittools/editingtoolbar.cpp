// 层：视图
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
#include <QMessageBox>
#include <QPoint>
#include <QRect>
#include <QToolBar>
#include <QToolButton>
#include <QVariant>
#include <QSpinBox>
#include <QSettings>
#include <QPushButton>
#include <qgssnappingutils.h>

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
#include "../paleoicons.h"
#include "../paleotheme.h"
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

    {
      setToolName( QCoreApplication::translate( "PaleoEditingToolbar", "选择要素" ) );
      setCursor( QCursor( Qt::ArrowCursor ) );
    }

    Flags flags() const override { return ShowContextMenu; }

    ~PaleoSelectTool() override
    {
      // QGIS 4: QgsMapCanvasItem is no longer a QObject — delete, don't hide
      delete mRubberBand;
    }

    void activate() override
    {
      if ( !mRubberBand )
        mRubberBand = new QgsRubberBand( canvas(), Qgis::GeometryType::Polygon );
      QgsMapTool::activate();
    }

    void deactivate() override
    {
      mDragging = false;
      delete mRubberBand;
      mRubberBand = nullptr;
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
      // D6 点选：零尺寸按 ±4 设备像素膨胀成小矩形——单击也能拾取点要素
      // （井位层地图→表联动靠这条路径把 fid 送进 selectionChanged）。
      if ( rect.width() < 1 && rect.height() < 1 )
        rect.adjust( -4, -4, 4, 4 );
      QgsMapToolSelectUtils::setRubberBand( canvas(), rect, mRubberBand );

      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
      if ( !vl )
      {
        emit messageEmitted( QCoreApplication::translate( "PaleoEditingToolbar",
                                   "选择操作需要当前图层为矢量图层" ),
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
                                     "无法将选择矩形转换到图层坐标系" ),
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
  connect(mCanvas, &QObject::destroyed, this, [this] {
    // The scene owns its graphics items and deletes them with the canvas.
    // A host may destroy that child before this toolbar; do not delete twice.
    //（mCanvas 现为 QPointer，析构自动置空——钩子只处理画布子项。）
    mEditHighlight.release();
  });
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

  connect( mUndoStack.get(), &PaleoUndoStack::canUndoChanged, this, &PaleoEditingToolbar::updateActionStates );
  connect( mUndoStack.get(), &PaleoUndoStack::canRedoChanged, this, &PaleoEditingToolbar::updateActionStates );
  updateActionStates();
  watchProject( nullptr ); // 默认监听进程级 QgsProject::instance()
}

PaleoEditingToolbar::~PaleoEditingToolbar()
{
  // An open edit session is NOT committed or rolled back here — that call
  // belongs to the host. Only tool/canvas-item resources are torn down.
  if ( mActiveEditTool )
  {
    QgsMapTool *tool = mActiveEditTool;
    mActiveEditTool = nullptr;
    if (mCanvas)
      mCanvas->unsetMapTool( tool );
    delete tool; // direct delete: the destructor never runs inside tool code
  }
}

void PaleoEditingToolbar::setLayers( const QList<QgsVectorLayer *> &layers )
{
  for ( const auto &layer : std::as_const( mLayers ) )
    if ( layer )
      disconnect( layer, nullptr, this, nullptr );
  for (const auto &connection : mHighlightConnections)
    disconnect(connection);
  mHighlightConnections.clear();
  mLayers.clear();
  for ( QgsVectorLayer *layer : layers )
  {
    if ( !layer )
      continue;
    mLayers.append( layer );
    connect( layer, &QObject::destroyed, this, [this] {
      // The combo stores raw QVariant pointers: remove them before any lookup.
      { const QSignalBlocker block( mLayerCombo ); mLayerCombo->clear(); }
      installTool( nullptr );
      refreshCombo();
      updateActionStates();
    } );
    connect( layer, &QgsVectorLayer::readOnlyChanged, this, &PaleoEditingToolbar::updateActionStates );
    connect( layer, &QgsVectorLayer::supportsEditingChanged, this, &PaleoEditingToolbar::updateActionStates );
  }
  if (isEditing())
  {
    mHighlightConnections.append(connect(mEditLayer, &QgsVectorLayer::selectionChanged, this, [this] { updateEditHighlight(); }));
    mHighlightConnections.append(connect(mEditLayer, &QgsVectorLayer::geometryChanged, this, [this] { updateEditHighlight(); }));
    updateEditHighlight();
  }
  refreshCombo();
  updateActionStates();
}

void PaleoEditingToolbar::setProject( QgsProject *project )
{
  QgsProject *previous = mProject ? mProject.data() : QgsProject::instance();
  // 工程边界（主线3）：编辑中的图层属于旧工程——切走前先收尾会话
  //（提交优先、失败回滚），不能等图层随旧工程析构丢缓冲。
  if ( isEditing() && project != previous )
    finalizeSession( tr( "切换工程：编辑%1" ) );

  mProject = project;
  watchProject( project );
  // Adopt the stored flags without writing them back (toggled would re-set).
  const QSignalBlocker block( mActionTopological );
  const QSignalBlocker blockCross( mActionCrossLayerTopo );
  mActionTopological->setChecked( project ? project->topologicalEditing() : false );
  mActionCrossLayerTopo->setChecked(
      project && project->readNumEntry( QStringLiteral( "paleo" ),
                                        QStringLiteral( "crossLayerTopologicalEditing" ), 0 ) != 0 );
}

void PaleoEditingToolbar::refreshFromProject()
{
  QgsProject *project = mProject ? mProject.data() : QgsProject::instance();
  setLayers( project ? project->layers<QgsVectorLayer *>() : QList<QgsVectorLayer *>() );
  // The persisted topo flag changes with the project — adopt it without
  // firing toggled (which would write it straight back).
  const QSignalBlocker block( mActionTopological );
  const QSignalBlocker blockCross( mActionCrossLayerTopo );
  mActionTopological->setChecked( project && project->topologicalEditing() );
  mActionCrossLayerTopo->setChecked(
      project && project->readNumEntry( QStringLiteral( "paleo" ),
                                        QStringLiteral( "crossLayerTopologicalEditing" ), 0 ) != 0 );
}

void PaleoEditingToolbar::setLayerFilter( LayerFilter filter )
{
  mLayerFilter = filter ? filter : []( const QgsVectorLayer * ) { return true; };
  refreshCombo();
  updateActionStates();
}

void PaleoEditingToolbar::setEditingService( QgisEditingService *service )
{
  if ( mEditingService == service )
    return;
  if ( mEditingService )
    disconnect( mEditingService, nullptr, this, nullptr );

  mEditingService = service;
  if ( !mEditingService )
    return;

  mEditingService->setUndoDepth(mUndoDepth->value());
  connect(mEditingService, &QgisEditingService::availabilityChanged, this, &PaleoEditingToolbar::updateActionStates);
  connect(mEditingService, &QgisEditingService::editFailed, this, &PaleoEditingToolbar::editRefused);
  auto syncExternalEditEnd = [this]( const QString &layerId, bool saved ) {
    if ( mEditLayer && ( mEditLayer->id() == layerId ||
         mEditLayer->customProperty( QStringLiteral( "paleoLayerId" ) ).toString() == layerId ) )
    {
      finishSession(mEditLayer->id(), saved);
    }
  };

  connect( mEditingService, &QgisEditingService::editCommitted, this,
           [syncExternalEditEnd]( const QString &layerId ) {
             syncExternalEditEnd( layerId, true );
           } );
  connect( mEditingService, &QgisEditingService::editRolledBack, this,
           [syncExternalEditEnd]( const QString &layerId ) {
             syncExternalEditEnd( layerId, false );
           } );
}

QgsVectorLayer *PaleoEditingToolbar::currentLayer() const
{
  QgsVectorLayer *selected = qvariant_cast<QgsVectorLayer *>( mLayerCombo->currentData() );
  for ( const auto &layer : mLayers )
    if ( layer && layer.data() == selected )
      return selected;
  return nullptr;
}

void PaleoEditingToolbar::setCurrentLayer( QgsVectorLayer *layer )
{
  if ( layer && comboIndexForLayer( mLayerCombo, layer ) < 0 )
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
  selectComboLayer( mLayerCombo, layer );
  mCanvas->setCurrentLayer( layer );
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
  // DESIGN.md ribbon-button：icon-over-text。图标 = vendor QGIS default
  // 主题 svg（qrc 解析，安装路径无关）；QGIS 缺的语义走 PaleoIcons 自绘。
  mToolBar->setToolButtonStyle( Qt::ToolButtonTextUnderIcon );
  mToolBar->setIconSize(PaleoIcons::toolbarSize()); // 9pt 正文的密度比，非 QGIS 24px
  if ( QLayout *toolLayout = mToolBar->layout() )
    toolLayout->setSpacing(PaleoTheme::tokens().spacingXs); // spacing xs (DESIGN.md toolbar button gap)

  mLayerCombo = new QComboBox( this );
  mLayerCombo->setPlaceholderText( tr( "选择可编辑图层" ) );
  mStateLabel = new QLabel( this );

  auto newToolAction = [this]( const QString &text, const QString &hint,
                               const QString &iconName ) -> QAction * {
    QAction *a = new QAction( PaleoIcons::qgisTheme( iconName ), text, this );
    a->setProperty( "hint", hint ); // restored on re-enable (§35 tooltip cycling)
    a->setToolTip( hint );
    a->setCheckable( true );
    mToolBar->addAction( a );
    return a;
  };
  auto newPlainAction = [this]( const QString &text, const QString &hint,
                                const QString &iconName ) -> QAction * {
    QAction *a = new QAction( PaleoIcons::qgisTheme( iconName ), text, this );
    a->setProperty( "hint", hint );
    a->setToolTip( hint );
    mToolBar->addAction( a );
    return a;
  };

  mActionEditMode = newToolAction(tr("编辑模式"), tr("进入编辑；退出时保存或放弃本段编辑"),
                                  QStringLiteral("mActionToggleEditing.svg"));
  mActionEditMode->setObjectName(QStringLiteral("actionEditMode"));
  connect(mActionEditMode, &QAction::triggered, this, [this](bool on) {
    if (on)
      startEditing();
    else if (isEditing())
    {
      if (!mEditLayer->isModified())
        cancelEditing();
      else
      {
        QMessageBox prompt(QMessageBox::Question, tr("退出编辑"),
                           tr("本段编辑尚未确认保存。保存编辑，或放弃本段修改？"),
                           QMessageBox::NoButton, this);
        auto *save = prompt.addButton(tr("保存"), QMessageBox::AcceptRole);
        auto *discard = prompt.addButton(tr("放弃"), QMessageBox::DestructiveRole);
        prompt.addButton(tr("取消"), QMessageBox::RejectRole);
        prompt.setDefaultButton(save);
        prompt.exec();
        if (prompt.clickedButton() == save)
          saveEditing();
        else if (prompt.clickedButton() == discard)
          cancelEditing();
      }
    }
    updateActionStates();
  });
  mActionSelect = newToolAction( tr( "选择" ), tr( "单击或框选要素（Shift 追加 / Ctrl 去除）" ),
                                 QStringLiteral( "mActionSelectRectangle.svg" ) );

  // Add-feature entry: one native QToolButton with a menu of the three capture
  // modes (点/线/面). The children are the checkable group members; the parent
  // action only hosts the affordance — InstantPopup keeps the semantics plain.
  mActionAddFeature = new QAction( PaleoIcons::qgisTheme( QStringLiteral( "mActionAdd.svg" ) ),
                                   tr( "添加" ), this );
  const QString addHint = tr( "添加要素：点 / 线 / 面" );
  mActionAddFeature->setProperty( "hint", addHint );
  mActionAddFeature->setToolTip( addHint );
  QMenu *addMenu = new QMenu( mToolBar );
  auto newCaptureAction = [this, addMenu]( const QString &text, const QString &hint,
                                         const QString &iconName ) -> QAction * {
    QAction *a = new QAction( PaleoIcons::qgisTheme( iconName ), text, this );
    a->setProperty( "hint", hint );
    a->setToolTip( hint );
    a->setCheckable( true );
    addMenu->addAction( a );
    return a;
  };
  mActionAddPoint = newCaptureAction( tr( "添加点" ), tr( "单击添加点；Esc 结束工具" ),
                                      QStringLiteral( "mActionCapturePoint.svg" ) );
  mActionAddLine = newCaptureAction( tr( "添加线" ), tr( "左键添加节点，右键完成线；Esc 结束工具" ),
                                     QStringLiteral( "mActionCaptureLine.svg" ) );
  mActionAddPolygon = newCaptureAction( tr( "添加面" ), tr( "左键添加节点，右键完成面；Esc 结束工具" ),
                                        QStringLiteral( "mActionCapturePolygon.svg" ) );
  // 菜单挂在动作上：宿主（ribbon「要素编辑」组）用同一颗动作建按钮时也带
  // 点/线/面下拉。
  mActionAddFeature->setMenu( addMenu );
  addMenu->setToolTipsVisible( true );
  mActionAddFeature->setCheckable( true );
  connect( mCanvas, &QgsMapCanvas::mapToolSet, this, [this]( QgsMapTool *tool, QgsMapTool * ) {
    QAction *active = tool ? tool->action() : nullptr;
    for ( QAction *action : { mActionSelect, mActionAddPoint, mActionAddLine,
                              mActionAddPolygon, mActionReshape, mActionMove,
                              mActionDeleteFeatures, mActionVertexEdit } )
      action->setChecked( action == active );
    const bool capture = active == mActionAddPoint || active == mActionAddLine || active == mActionAddPolygon;
    mActionAddFeature->setChecked( capture );
    mActionAddFeature->setText( capture ? active->text() : tr( "添加" ) );
  } );
  QToolButton *addButton = new QToolButton( mToolBar );
  addButton->setDefaultAction( mActionAddFeature );
  addButton->setMenu( addMenu );
  addButton->setPopupMode( QToolButton::InstantPopup );
  addButton->setToolButtonStyle( Qt::ToolButtonTextUnderIcon );
  mToolBar->addWidget( addButton );

  mActionReshape = newToolAction( tr( "整形" ), tr( "沿画线重构所选要素的几何" ),
                                  QStringLiteral( "mActionReshape.svg" ) );
  mActionMove = newToolAction( tr( "移动" ), tr( "拖动移动所选要素" ),
                               QStringLiteral( "mActionMoveFeature.svg" ) );
  mActionDeleteFeatures = newToolAction( tr( "删除" ), tr( "单击删除光标下的要素（命中式，不删整个选区）" ),
                                         QStringLiteral( "mActionDeleteSelected.svg" ) );
  mActionVertexEdit = newToolAction( tr( "节点" ), tr( "编辑所选要素的节点" ),
                                     QStringLiteral( "mActionVertexTool.svg" ) );
  // Mode toggle (checkable, NOT in the exclusive tool group): same-layer
  // topological editing for the vertex tool — coincident vertices move/insert/
  // delete together. State mirrors QgsProject::topologicalEditing so it
  // persists in the .qgz; toggling also pushes live into an armed vertex tool.
  mActionTopological = newToolAction( tr( "拓扑" ), tr( "拓扑编辑：共边节点随选区节点一起动/增/删" ),
                                      QStringLiteral( "mIconTopologicalEditing.svg" ) );
  connect( mActionTopological, &QAction::toggled, this, [this]( bool on ) {
    QgsProject *project = mProject ? mProject.data() : QgsProject::instance();
    if ( project && project->topologicalEditing() != on )
      project->setTopologicalEditing( on );
    if ( auto *vt = qobject_cast<PaleoVertexTool *>( mActiveEditTool.data() ) )
      vt->setTopologicalEditingEnabled( on || (currentLayer() && currentLayer()->customProperty("paleo/requireFaciesTopology").toBool()) );
    updateActionStates(); // 跨层开关的可用性随拓扑开关联动
  } );

  // 跨层拓扑（mapping 主线2）：写集延伸到「同 CRS 且处于编辑会话的相邻层」。
  // 参与层必须可写（native 语义），undo 按层各一步。持久化走工程自定义属性
  // paleo/crossLayerTopologicalEditing（随 .qgz）。仅在拓扑编辑开启时可用。
  mActionCrossLayerTopo = newToolAction( tr( "跨层" ), tr( "跨层拓扑：共点节点延伸到同 CRS 的其他编辑层（各层 undo 独立）" ),
                                         QStringLiteral( "mIconTopologicalEditing.svg" ) );
  mActionCrossLayerTopo->setObjectName( QStringLiteral( "actionCrossLayerTopo" ) );
  connect( mActionCrossLayerTopo, &QAction::toggled, this, [this]( bool on ) {
    QgsProject *project = mProject ? mProject.data() : QgsProject::instance();
    if ( project )
      project->writeEntry( QStringLiteral( "paleo" ), QStringLiteral( "crossLayerTopologicalEditing" ), on );
    if ( auto *vt = qobject_cast<PaleoVertexTool *>( mActiveEditTool.data() ) )
      vt->setCrossLayerTopologyEnabled( on && !QgisEditingService::isConstraintLayer(currentLayer()) );
  } );

  mToolBar->addSeparator();
  mActionSave = newPlainAction( tr( "保存编辑" ), tr( "提交当前图层的编辑" ),
                                QStringLiteral( "mActionSaveEdits.svg" ) );
  mActionCancel = newPlainAction( tr( "放弃编辑" ), tr( "放弃当前图层的编辑" ),
                                  QStringLiteral( "mActionCancelEdits.svg" ) );

  mToolBar->addSeparator();
  mActionUndo = newPlainAction( tr( "撤销" ), tr( "撤销上一步编辑" ),
                                QStringLiteral( "mActionUndo.svg" ) );
  mActionRedo = newPlainAction( tr( "重做" ), tr( "重做被撤销的编辑" ),
                                QStringLiteral( "mActionRedo.svg" ) );

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
  connect( mActionCancel, &QAction::triggered, this, [this] {
    // 一键 rollBack 整个编辑会话：有未提交命令时先确认（程序化 cancelEditing()
    // 不确认——调用方已决断；消息措辞如实报出爆炸半径）。
    QgsVectorLayer *layer = mEditLayer;
    if ( layer && layer->isEditable() && layer->undoStack() && layer->undoStack()->canUndo() )
    {
      const auto choice = QMessageBox::question(
        this, tr( "放弃编辑" ),
        tr( "将放弃图层 %1 的全部未保存编辑（%2 条命令），是否继续？" )
          .arg( layer->name() )
          .arg( layer->undoStack()->index() ),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel );
      if ( choice != QMessageBox::Ok )
        return;
    }
    cancelEditing();
  } );

  // activated() fires on user picks only — programmatic rebuilds re-sync via
  // setCurrentLayer/refreshCombo and never re-enter this path.
  connect( mLayerCombo, &QComboBox::activated, this, [this]( int index ) {
    setCurrentLayer( qvariant_cast<QgsVectorLayer *>( mLayerCombo->itemData( index ) ) );
  } );

  auto *layout = new QHBoxLayout( this );
  layout->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // spacing sm (panel padding)
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  layout->addWidget( mLayerCombo );
  layout->addWidget( mToolBar );
  layout->addStretch( 1 );
  layout->addWidget( mStateLabel );
  mSnapTolerance = new QSpinBox(this);
  mSnapTolerance->setObjectName(QStringLiteral("editingSnapToleranceSpin"));
  mSnapTolerance->setRange(1, 100);
  mSnapTolerance->setFont(PaleoTheme::monoFont());
  mSnapTolerance->setSuffix(tr(" px"));
  mSnapTolerance->setAccessibleName(tr("捕捉容差"));
  mSnapTolerance->setToolTip(tr("井点、层位边界与既有约束顶点/线段的屏幕捕捉容差"));
  mSnapTolerance->setValue(QSettings().value(QStringLiteral("editing/snapTolerancePx"), 10).toInt());
  mToolBar->addWidget(mSnapTolerance);
  connect(mSnapTolerance, &QSpinBox::valueChanged, this, [this](int px) {
    auto config = mCanvas->snappingUtils()->config();
    config.setEnabled(true);
    config.setMode(Qgis::SnappingMode::AllLayers);
    config.setTypeFlag(Qgis::SnappingType::Vertex | Qgis::SnappingType::Segment);
    config.setTolerance(px);
    config.setUnits(Qgis::MapToolUnit::Pixels);
    mCanvas->snappingUtils()->setConfig(config);
    if (auto *project = mProject ? mProject.data() : QgsProject::instance())
    {
      // 同 paleomainwindow 镜像点：config 须挂 project 指针，否则重开工程
      // 时 readProject 在空 project 上调 mapLayer 崩溃。
      config.setProject(project);
      project->setSnappingConfig(config);
    }
    QSettings().setValue(QStringLiteral("editing/snapTolerancePx"), px);
  });
  auto config = mCanvas->snappingUtils()->config();
  config.setTolerance(mSnapTolerance->value());
  config.setUnits(Qgis::MapToolUnit::Pixels);
  mCanvas->snappingUtils()->setConfig(config);
  mUndoDepth = new QSpinBox(this);
  mUndoDepth->setObjectName(QStringLiteral("editingUndoDepthSpin"));
  mUndoDepth->setRange(1, 10000);
  mUndoDepth->setFont(PaleoTheme::monoFont());
  mUndoDepth->setValue(QSettings().value(QStringLiteral("editing/undoDepth"), 100).toInt());
  mUndoDepth->setAccessibleName(tr("撤销栈深度"));
  mUndoDepth->setToolTip(tr("下一段编辑会话的最大撤销命令数；放弃编辑仍恢复整段起始快照"));
  mToolBar->addWidget(mUndoDepth);
  connect(mUndoDepth, &QSpinBox::valueChanged, this, [this](int depth) {
    QSettings().setValue(QStringLiteral("editing/undoDepth"), depth);
    if (mEditingService)
      mEditingService->setUndoDepth(depth);
  });

}

void PaleoEditingToolbar::installTool( QgsMapTool *tool )
{
  if ( mActiveEditTool )
  {
    QgsMapTool *old = mActiveEditTool;
    mActiveEditTool = nullptr;
    if (mCanvas)
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
    for ( const auto &candidate : std::as_const( mLayers ) )
    {
      QgsVectorLayer *lyr = candidate.data();
      if ( !lyr )
        continue;
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
  QgsVectorLayer *target = editableTarget();
  const bool hasTarget = target && target->isValid() && target->isSpatial();
  const QString noTarget = tr( "先在图层树或图层下拉框选择矢量图层" );
  const QString noEdit = !hasTarget ? noTarget
      : target->readOnly() ? tr( "当前图层为只读，可选择要素，不能修改" )
      : !target->supportsEditing() ? tr( "当前图层的数据源不支持编辑" )
      : QgisEditingService::isConstraintLayer(target) && !mEditingService ? tr("约束编辑需要工程 store 同步服务")
      : mEditingService ? mEditingService->availabilityError(target) : QString();
  const auto gate = []( QAction *action, const QString &reason ) {
    action->setEnabled( reason.isEmpty() );
    action->setToolTip( reason.isEmpty() ? action->property( "hint" ).toString() : reason );
    action->setStatusTip( action->toolTip() );
  };
  gate(mActionEditMode, noEdit);
  {
    QSignalBlocker blocked(mActionEditMode);
    mActionEditMode->setChecked(isEditing());
  }
  gate( mActionSelect, hasTarget ? QString() : noTarget );
  for ( QAction *action : { mActionAddFeature, mActionMove, mActionDeleteFeatures,
                            mActionVertexEdit, mActionTopological } )
    gate( action, noEdit );
  if (target && target->customProperty("paleo/requireFaciesTopology").toBool()) {
    if (!mActionTopological->isChecked()) mActionTopological->setChecked(true);
    gate(mActionTopological, tr("三级相面共用边界，必须保持拓扑编辑"));
  }
  // 跨层拓扑叠加在拓扑编辑之上：拓扑关或不可编辑 → 禁用并带 reason。
  gate( mActionCrossLayerTopo, !noEdit.isEmpty() ? noEdit
          : !mActionTopological->isChecked() ? tr( "先开启拓扑编辑，再考虑跨层联动" ) : QString() );
  const auto geometryGate = [&]( QAction *action, Qgis::GeometryType geometry,
                                  const QString &reason ) {
    gate( action, !noEdit.isEmpty() ? noEdit
                : target->geometryType() != geometry ? reason : QString() );
  };
  geometryGate( mActionAddPoint, Qgis::GeometryType::Point, tr( "添加点仅适用于点图层" ) );
  geometryGate( mActionAddLine, Qgis::GeometryType::Line, tr( "添加线仅适用于线图层" ) );
  geometryGate( mActionAddPolygon, Qgis::GeometryType::Polygon, tr( "添加面仅适用于面图层" ) );
  gate( mActionReshape, !noEdit.isEmpty() ? noEdit
      : target->geometryType() == Qgis::GeometryType::Point ? tr( "整形仅适用于线或面图层" ) : QString() );

  if (QgisEditingService::isConstraintLayer(target))
  {
    const QString capture = tr("约束新增请使用约束页的类型化绘制入口");
    for (QAction *a : {mActionAddFeature, mActionAddPoint, mActionAddLine, mActionAddPolygon})
      gate(a, capture);
    gate(mActionCrossLayerTopo, tr("约束编辑使用单图层与 store 一致的撤销会话"));
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
  emit stateChanged();
}

void PaleoEditingToolbar::updateStateLabel()
{
  QPalette palette = mStateLabel->palette();
  // DESIGN.md text-muted——状态文字（浏览/编辑中）是说明性 chrome，非告警。
  const QColor stateColor = PaleoTheme::tokens().textMuted;
  if ( isEditing() )
  {
    // Editing is an ordinary operation, not an error or review warning.
    mStateLabel->setText( tr( "编辑中：%1" ).arg( mEditLayer->name() ) );
    palette.setColor( QPalette::WindowText, stateColor );
  }
  else
  {
    mStateLabel->setText( currentLayer() ? tr( "浏览：%1" ).arg( currentLayer()->name() )
                                        : tr( "未选择矢量图层" ) );
    palette.setColor( QPalette::WindowText, stateColor );
  }
  mStateLabel->setPalette( palette );
}

void PaleoEditingToolbar::updateEditHighlight()
{
  if (!mCanvas || !mEditLayer || !mEditLayer->isEditable())
  {
    mEditHighlight.reset();
    return;
  }
  if (!mEditHighlight)
    mEditHighlight = std::make_unique<QgsRubberBand>(mCanvas, Qgis::GeometryType::Line);
  mEditHighlight->reset(Qgis::GeometryType::Line);
  mEditHighlight->setColor(mCanvas->mapSettings().selectionColor());
  mEditHighlight->setLineStyle(Qt::DashLine);
  for (const auto &feature : mEditLayer->selectedFeatures())
    mEditHighlight->addGeometry(feature.geometry(), mEditLayer);
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
  if ( action != mActionSelect && !target->isEditable() && !startEditing() )
  {
    action->setChecked( false ); // startEditing refused — leave the canvas as-is
    updateActionStates();
    return;
  }
  if ( mActiveEditTool && mCanvas->mapTool() == mActiveEditTool
       && mActiveEditTool->property( "paleo-action" ).value<QAction *>() == action )
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
  {
    auto *vt = new PaleoVertexTool( mCanvas, target );
    vt->setTopologicalEditingEnabled( mActionTopological->isChecked() || target->customProperty("paleo/requireFaciesTopology").toBool() );
    vt->setCrossLayerTopologyEnabled( mActionCrossLayerTopo->isChecked() && !QgisEditingService::isConstraintLayer(target) );
    tool = wireAborted( vt );
  }

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
  tool->setAction( action ); // native activation/deactivation owns the checkmark

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
  if (QgisEditingService::isConstraintLayer(target) && !mEditingService)
  {
    emit editRefused(tr("约束编辑需要工程 store 同步服务"));
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
  mHighlightConnections.append(connect(target, &QgsVectorLayer::selectionChanged, this, [this] { updateEditHighlight(); }));
  mHighlightConnections.append(connect(target, &QgsVectorLayer::geometryChanged, this, [this] { updateEditHighlight(); }));
  updateEditHighlight();
  emit editingStarted( target->id() );
  refreshCombo(); // ● marker moves to the session layer
  updateActionStates();
  return true;
}

void PaleoEditingToolbar::finishSession(const QString &id, bool saved)
{
  // Service commits synchronously notify us before saveEditing() returns.
  // Consume the session once so each edit produces exactly one new version.
  if (!mEditLayer || mEditLayer->id() != id)
    return;
  for (const auto &connection : mHighlightConnections)
    disconnect(connection);
  mHighlightConnections.clear();
  mEditHighlight.reset();
  mEditLayer = nullptr;
  installTool(nullptr);
  uncheckEditTools(this);
  emit editingStopped(id, saved);
  refreshCombo();
  updateActionStates();
}

bool PaleoEditingToolbar::saveEditing()
{
  QgsVectorLayer *layer = mEditLayer;
  if ( !layer || !layer->isEditable() )
  {
    emit editRefused( tr( "当前没有进行中的编辑会话" ) );
    return false;
  }

  const QString id = layer->id();
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

  finishSession(id, true);
  return true;
}

void PaleoEditingToolbar::finalizeSession( const QString &reason )
{
  QgsVectorLayer *layer = mEditLayer;
  if ( !layer || !layer->isEditable() )
    return; // 无会话（或会话已在外部结束）：幂等

  const QString id = layer->id();
  QString err;
  bool saved = mEditingService ? mEditingService->commitEdit( layer, &err )
                               : layer->commitChanges();
  if ( !saved )
  {
    // Provider 拒绝提交：工程边界上不能保留会话——回滚兜底（错误经 reason 上报）。
    saved = false;
    if ( !( mEditingService ? mEditingService->rollbackEdit( layer ) : layer->rollBack() ) )
      emit editRefused( tr( "回滚图层 %1 失败" ).arg( layer->name() ) );
  }

  finishSession(id, saved);
  emit editRefused( reason.arg( saved ? tr( "已提交" ) : tr( "已放弃" ) ) );
}

void PaleoEditingToolbar::watchProject( QgsProject *project )
{
  QgsProject *watched = project ? project : QgsProject::instance();
  if ( mWatchedProject == watched )
    return;
  if ( mWatchedProject )
  {
    disconnect( mWatchedProject.data(),
                qOverload<const QStringList &>( &QgsProject::layersWillBeRemoved ), this, nullptr );
    disconnect( mWatchedProject.data(), &QgsProject::cleared, this, nullptr );
  }
  mWatchedProject = watched;
  if ( !watched )
    return;

  // 编辑层即将被移除（含 clear()/切工程 的移除波）：先收尾会话——此时图层
  // 对象仍活着，提交窗口还在；拖到析构就只能是静默丢缓冲。
  connect( watched, qOverload<const QStringList &>( &QgsProject::layersWillBeRemoved ), this,
           [this]( const QStringList &ids ) {
             if ( !mEditLayer || !ids.contains( mEditLayer->id() ) )
               return;
             finalizeSession( tr( "编辑图层被移除：编辑%1" ) );
           } );
  // clear() 收尾波（layersWillBeRemoved 已处理会话；这里只清残余状态）。
  connect( watched, &QgsProject::cleared, this, [this] {
    if ( !mEditLayer )
    {
      updateActionStates();
      return;
    }
    const QString id = mEditLayer->id();
    mEditLayer = nullptr; // 缓冲随层析构丢弃
    installTool( nullptr );
    uncheckEditTools( this );
    emit editingStopped( id, false );
    emit editRefused( tr( "工程已清空：编辑已丢弃" ) );
    refreshCombo();
    updateActionStates();
  } );
}

bool PaleoEditingToolbar::cancelEditing()
{
  QgsVectorLayer *layer = mEditLayer;
  if ( !layer || !layer->isEditable() )
  {
    emit editRefused( tr( "当前没有进行中的编辑会话" ) );
    return false;
  }

  const QString id = layer->id();
  const bool ok = mEditingService ? mEditingService->rollbackEdit( layer )
                                  : layer->rollBack();
  if ( !ok )
  {
    emit editRefused( tr( "回滚图层 %1 失败" ).arg( layer->name() ) );
    return false;
  }

  finishSession(id, false);
  return true;
}
