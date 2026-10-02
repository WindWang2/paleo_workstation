// 层：视图
#include "editingtools.h"
#include "../maptools/capturehelpers.h"

#include "../../qgis/qgiseditingservice.h"

#include <memory>
#include <utility>

#include <QCursor>
#include <QKeyEvent>
#include <QList>
#include <QPair>

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgscurve.h>
#include <qgscurvepolygon.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgslinestring.h>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgspolygon.h>
#include <qgsrubberband.h>
#include <qgstolerance.h>
#include <qgsvectorlayer.h>
#include <qgswkbtypes.h>

// ---------------------------------------------------------------------------
// PaleoAddFeatureTool
// ---------------------------------------------------------------------------

PaleoAddFeatureTool::PaleoAddFeatureTool( QgsMapCanvas *canvas,
    QgsAdvancedDigitizingDockWidget *cadDock,
    QgsMapToolCapture::CaptureMode mode,
    QgsVectorLayer *layer )
  : QgsMapToolCapture( canvas, CaptureHelpers::resolveCadDock( canvas, cadDock ), mode )
  , mLayer( layer )
{
  setToolName( tr( "添加要素" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

PaleoAddFeatureTool::~PaleoAddFeatureTool() = default;

void PaleoAddFeatureTool::activate()
{
  // Base chain: QgsMapToolEdit arms the canvas cursor, QgsMapToolAdvancedDigitizing
  // registers with the CAD dock, QgsMapToolCapture adds the extra snap layer.
  QgsMapToolCapture::activate();
  startCapturing(); // capture-mode state machine live ahead of the first vertex
}

void PaleoAddFeatureTool::deactivate()
{
  stopCapturing(); // drop rubber bands + capture curve (idempotent)
  QgsMapToolCapture::deactivate();
}

QgsVectorLayer *PaleoAddFeatureTool::targetLayer() const
{
  // QgsMapToolEdit::currentVectorLayer() is non-const, so the fallback goes
  // through the const canvas() accessor instead (canvas()->currentLayer()).
  return mLayer ? mLayer.data() : qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
}

void PaleoAddFeatureTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit editAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolCapture::keyPressEvent( e ); // Esc → stopCapturing(), e->ignore()
}

void PaleoAddFeatureTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  // Right-click commits once the base threshold is met (line ≥2, polygon ≥3);
  // below it the base just stopCapturing()s without invoking a completion
  // hook — surface that as a cancel gesture. Point mode has no threshold (any
  // left click commits via pointCaptured), so right-clicks stay with the base.
  const CaptureMode m = mode();
  const int threshold = m == CaptureLine ? 2 : ( m == CapturePolygon ? 3 : 0 );
  const bool cancelClick = e->button() == Qt::RightButton && threshold > 0 && size() < threshold;
  QgsMapToolCapture::cadCanvasReleaseEvent( e );
  if ( cancelClick )
    emit editAborted();
}

void PaleoAddFeatureTool::pointCaptured( const QgsPoint &point )
{
  // The hook delivers the point already in the target layer's CRS (the capture
  // went through nextPoint()'s map→layer transform), so commit is CRS-safe.
  // Z/M dimensions survive the copy when the layer schema carries them.
  commitFeature( QgsGeometry( new QgsPoint( point ) ) );
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

void PaleoAddFeatureTool::lineCaptured( const QgsCurve *line )
{
  // The base calls lineCaptured( curveToAdd.release() ): ownership passes here.
  const std::unique_ptr<const QgsCurve> ownedLine( line );
  if ( !line || line->isEmpty() )
    return;

  // Straight-segment capture arrives wrapped in (possibly) compound curves;
  // flatten to a plain QgsLineString unless the layer stores curves natively,
  // so the committed WKB type matches the layer schema (the memory provider
  // accepts anything, file providers may not).
  if ( targetLayer() && !QgsWkbTypes::isCurvedType( targetLayer()->wkbType() ) )
    commitFeature( QgsGeometry( ownedLine->curveToLine() ) ); // curveToLine: factory, QgsGeometry adopts
  else
    commitFeature( QgsGeometry( ownedLine->clone() ) );
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

void PaleoAddFeatureTool::polygonCaptured( const QgsCurvePolygon *polygon )
{
  // Borrowed pointer: the base calls polygonCaptured( poly.get() ) and keeps
  // ownership (contrast lineCaptured), so clone before committing.
  if ( !polygon || polygon->isEmpty() || !polygon->exteriorRing() )
    return;

  if ( targetLayer() && QgsWkbTypes::isCurvedType( targetLayer()->wkbType() ) )
  {
    commitFeature( QgsGeometry( polygon->clone() ) );
  }
  else
  {
    // Flatten the rings to a plain QgsPolygon: straight-segment capture yields
    // compound-curve rings, and a non-curved layer schema expects linear rings
    // (setExteriorRing/addInteriorRing take ownership of the curveToLine copies
    // and close open rings).
    std::unique_ptr<QgsPolygon> flat( new QgsPolygon() );
    flat->setExteriorRing( polygon->exteriorRing()->curveToLine() );
    for ( int i = 0; i < polygon->numInteriorRings(); ++i )
    {
      const QgsCurve *ring = polygon->interiorRing( i );
      if ( ring )
        flat->addInteriorRing( ring->curveToLine() );
    }
    commitFeature( QgsGeometry( flat.release() ) );
  }
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

bool PaleoAddFeatureTool::commitFeature( QgsGeometry geometry )
{
  QgsVectorLayer *vl = targetLayer();
  if ( !vl || !vl->isEditable() )
  {
    // The host (PaleoEditingToolbar) owns edit sessions; a refusal is a
    // warning, NOT an editAborted() — no user gesture failed here.
    emit messageEmitted( vl ? tr( "无法添加要素：%1 不在编辑状态" ).arg( vl->name() )
                            : tr( "无法添加要素：没有目标图层" ),
                         Qgis::MessageLevel::Warning );
    return false;
  }

  // 拓扑提交门（QGIS_NATIVE_ADOPTION）：非法几何（自相交环等）如实拒入
  // edit buffer——原生 QgsGeometryValidator 错误文本，不静默修形。
  const QString geomErr =
      QgisEditingService::geometryCommitError( geometry, tr( "绘制的要素" ) );
  if ( !geomErr.isEmpty() )
  {
    emit messageEmitted( geomErr, Qgis::MessageLevel::Warning );
    return false;
  }

  QgsFeature feature( vl->fields() );
  feature.setGeometry( geometry );
  vl->beginEditCommand( tr( "添加要素" ) );
  if ( vl->addFeature( feature ) )
  {
    vl->endEditCommand(); // one edit command per gesture → one native undo step
    ++mCommittedCount;
    emit featureEdited( vl->id() );
    return true;
  }
  vl->destroyEditCommand(); // addFeature refused (e.g. geometry-type mismatch)
  return false;
}

// ---------------------------------------------------------------------------
// PaleoReshapeTool
// ---------------------------------------------------------------------------

PaleoReshapeTool::PaleoReshapeTool( QgsMapCanvas *canvas,
                                    QgsAdvancedDigitizingDockWidget *cadDock,
                                    QgsVectorLayer *layer )
  : QgsMapToolCapture( canvas, CaptureHelpers::resolveCadDock( canvas, cadDock ), CaptureLine )
  , mLayer( layer )
{
  setToolName( tr( "整形要素" ) );
  setCursor( QCursor( Qt::CrossCursor ) );
  setCurrentCaptureTechnique( Qgis::CaptureTechnique::StraightSegments );
}

PaleoReshapeTool::~PaleoReshapeTool() = default;

void PaleoReshapeTool::activate()
{
  // Same base chain as PaleoAddFeatureTool::activate().
  QgsMapToolCapture::activate();
  startCapturing(); // capture-mode state machine live ahead of the first vertex
}

void PaleoReshapeTool::deactivate()
{
  stopCapturing(); // drop rubber bands + capture curve (idempotent)
  QgsMapToolCapture::deactivate();
}

QgsVectorLayer *PaleoReshapeTool::targetLayer() const
{
  return mLayer ? mLayer.data() : qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
}

void PaleoReshapeTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit editAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolCapture::keyPressEvent( e ); // Esc → stopCapturing(), e->ignore()
}

void PaleoReshapeTool::cadCanvasReleaseEvent( QgsMapMouseEvent *e )
{
  // Right-click commits the reshape line once ≥2 vertices exist (base path);
  // with fewer it is a cancel gesture — the base just stopCapturing()s without
  // emitting lineCaptured(), so surface that as editAborted().
  const bool cancelClick = e->button() == Qt::RightButton && size() < 2;
  QgsMapToolCapture::cadCanvasReleaseEvent( e );
  if ( cancelClick )
    emit editAborted();
}

void PaleoReshapeTool::lineCaptured( const QgsCurve *line )
{
  // The base calls lineCaptured( curveToAdd.release() ): ownership passes here.
  const std::unique_ptr<const QgsCurve> ownedLine( line );
  if ( !line || line->isEmpty() )
    return;

  QgsVectorLayer *vl = targetLayer();
  if ( !vl || !vl->isEditable() )
  {
    emit messageEmitted( vl ? tr( "无法整形：%1 不在编辑状态" ).arg( vl->name() )
                            : tr( "无法整形：没有目标图层" ),
                         Qgis::MessageLevel::Warning );
    return;
  }

  const QgsFeatureIds selected = vl->selectedFeatureIds();
  if ( selected.isEmpty() )
  {
    emit messageEmitted( tr( "请先选择要整形的要素" ), Qgis::MessageLevel::Warning );
    return;
  }

  // Flatten the (possibly compound) capture curve: reshapeGeometry() takes a
  // plain QgsLineString, and the capture curve is already stored in the layer
  // CRS by the base's nextPoint() transform — no reprojection needed here.
  const std::unique_ptr<QgsLineString> reshapeLine( ownedLine->curveToLine() );
  const QgsRectangle lineBBox = reshapeLine->boundingBox();

  vl->beginEditCommand( tr( "整形要素" ) );
  int changed = 0;
  for ( QgsFeatureId fid : selected )
  {
    const QgsFeature feature = vl->getFeature( fid );
    const QgsGeometry original = feature.geometry();
    if ( original.isNull() )
      continue;

    // Cheap bbox pre-filter: skip features the reshape line cannot touch —
    // reshapeGeometry() on a non-intersecting pair would waste a GEOS pass
    // (and in QGIS 4.2 reports it as an operation failure, not a change).
    if ( !original.boundingBoxIntersects( lineBBox ) )
      continue;

    // QgsGeometry is implicitly shared: the copy is a cheap ref-count clone and
    // reshapeGeometry() detaches before mutating, leaving the snapshot intact.
    QgsGeometry updated( original );
    if ( updated.reshapeGeometry( *reshapeLine ) == Qgis::GeometryOperationResult::Success
         && vl->changeGeometry( fid, updated ) )
    {
      ++changed; // Success ⇒ the geometry was actually reshaped (QGIS 4.2)
    }
  }

  if ( changed > 0 )
  {
    vl->endEditCommand(); // whole batch = one edit command = one native undo step
    mReshapedCount += changed;
    emit featureEdited( vl->id() );
  }
  else
  {
    vl->destroyEditCommand(); // nothing intersected — leave the edit buffer clean
  }
  stopCapturing(); // clean capture state/rubber band (idempotent; base also calls it)
}

// ---------------------------------------------------------------------------
// PaleoMoveTool
// ---------------------------------------------------------------------------

PaleoMoveTool::PaleoMoveTool( QgsMapCanvas *canvas, QgsVectorLayer *layer )
  : QgsMapToolEdit( canvas ) // NOT QgsMapToolAdvancedDigitizing: events arrive at the plain canvas* hooks
  , mLayer( layer )
{
  setToolName( tr( "移动要素" ) );
  setCursor( QCursor( Qt::SizeAllCursor ) );
}

PaleoMoveTool::~PaleoMoveTool()
{
  clearDragState(); // drop snapshot geometries + preview band before bases tear down
}

void PaleoMoveTool::activate()
{
  QgsMapToolEdit::activate();
}

void PaleoMoveTool::deactivate()
{
  clearDragState(); // never leak a half-flown drag into the next activation
  QgsMapToolEdit::deactivate();
}

QgsVectorLayer *PaleoMoveTool::targetLayer() const
{
  return mLayer ? mLayer.data() : qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
}

void PaleoMoveTool::canvasPressEvent( QgsMapMouseEvent *e )
{
  QgsMapToolEdit::canvasPressEvent( e ); // empty base hook; chained for parity
  if ( e->button() != Qt::LeftButton )
    return;

  QgsVectorLayer *vl = targetLayer();
  if ( !vl || !vl->isEditable() )
  {
    // The host owns edit sessions; a refusal is a warning, not an abort.
    emit messageEmitted( vl ? tr( "无法移动要素：%1 不在编辑状态" ).arg( vl->name() )
                            : tr( "无法移动要素：没有目标图层" ),
                         Qgis::MessageLevel::Warning );
    return;
  }

  const QgsFeatureIds selected = vl->selectedFeatureIds();
  if ( selected.isEmpty() )
  {
    emit messageEmitted( tr( "请先选择要移动的要素" ), Qgis::MessageLevel::Warning );
    return;
  }

  // Drag origin + per-fid geometry snapshots in the layer CRS: the preview and
  // the commit translate copies of these; the edit buffer only ever sees
  // changeGeometry() results, so an aborted drag leaves no trace.
  clearDragState(); // paranoia: gestures never stack
  mStartPoint = new QgsPointXY( toLayerCoordinates( vl, e->mapPoint() ) );
  mLastPoint = new QgsPointXY( *mStartPoint );
  for ( QgsFeatureId fid : selected )
  {
    const QgsFeature feature = vl->getFeature( fid );
    if ( !feature.hasGeometry() )
      continue;
    mSnapshots.append( qMakePair( fid, new QgsGeometry( feature.geometry() ) ) );
  }
  if ( mSnapshots.isEmpty() )
  {
    delete mStartPoint;
    delete mLastPoint;
    mStartPoint = nullptr;
    mLastPoint = nullptr;
    emit messageEmitted( tr( "所选要素没有可移动的几何" ), Qgis::MessageLevel::Warning );
    return;
  }

  // createRubberBand() hands ownership to the caller; clearDragState()/the
  // destructor delete it, so it never outlives the tool.
  mPreviewBand = createRubberBand( vl->geometryType() );
  mPreviewBand->show();
  mDragging = true;
}

void PaleoMoveTool::canvasMoveEvent( QgsMapMouseEvent *e )
{
  QgsMapToolEdit::canvasMoveEvent( e ); // empty base hook; chained for parity
  if ( !mDragging )
    return;

  QgsVectorLayer *vl = targetLayer();
  if ( !vl )
    return;

  *mLastPoint = toLayerCoordinates( vl, e->mapPoint() );
  const double dx = mLastPoint->x() - mStartPoint->x();
  const double dy = mLastPoint->y() - mStartPoint->y();

  // Native preview only: translated snapshot clones painted through the
  // QgsRubberBand from createRubberBand(). For a single-feature selection
  // reset()+addGeometry() is setToGeometry() with one extra step; the loop
  // keeps one band painting a whole multi-feature selection.
  mPreviewBand->reset( vl->geometryType() );
  for ( const QPair<qint64, QgsGeometry *> &snapshot : std::as_const( mSnapshots ) )
  {
    QgsGeometry translated( *snapshot.second ); // COW clone; translate() detaches
    translated.translate( dx, dy );             // native mutator, no hand-rolled math
    mPreviewBand->addGeometry( translated, vl );
  }
}

void PaleoMoveTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  QgsMapToolEdit::canvasReleaseEvent( e ); // empty base hook; chained for parity
  if ( !mDragging || e->button() != Qt::LeftButton )
    return;

  QgsVectorLayer *vl = targetLayer();
  if ( !vl || !vl->isEditable() )
  {
    // Layer left edit mode mid-drag: drop the gesture without an edit command
    // (warning, not abort — the drag itself was never committed).
    emit messageEmitted( vl ? tr( "无法移动要素：%1 已不在编辑状态" ).arg( vl->name() )
                            : tr( "无法移动要素：没有目标图层" ),
                         Qgis::MessageLevel::Warning );
    clearDragState();
    return;
  }

  const QgsPointXY end = toLayerCoordinates( vl, e->mapPoint() );
  const double dx = end.x() - mStartPoint->x();
  const double dy = end.y() - mStartPoint->y();
  if ( qgsDoubleNear( dx, 0.0 ) && qgsDoubleNear( dy, 0.0 ) )
  {
    clearDragState(); // a click without displacement is not a move gesture
    return;
  }

  vl->beginEditCommand( tr( "移动要素" ) );
  int moved = 0;
  for ( const QPair<qint64, QgsGeometry *> &snapshot : std::as_const( mSnapshots ) )
  {
    QgsGeometry translated( *snapshot.second ); // COW clone; translate() detaches
    translated.translate( dx, dy );
    if ( vl->changeGeometry( snapshot.first, translated ) )
      ++moved;
  }

  if ( moved > 0 )
  {
    vl->endEditCommand(); // one edit command per drag → one native undo step
    mMovedCount += moved;
    emit featureEdited( vl->id() );
  }
  else
  {
    vl->destroyEditCommand(); // every changeGeometry refused — stay clean
  }
  clearDragState();
}

void PaleoMoveTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
  {
    // Cancel the in-flight drag: preview torn down, edit buffer untouched.
    // Also emitted with nothing in flight — the owner tears the tool down.
    clearDragState();
    emit editAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  }
  QgsMapToolEdit::keyPressEvent( e );
}

void PaleoMoveTool::clearDragState()
{
  delete mStartPoint;
  delete mLastPoint;
  mStartPoint = nullptr;
  mLastPoint = nullptr;
  for ( const QPair<qint64, QgsGeometry *> &snapshot : std::as_const( mSnapshots ) )
    delete snapshot.second;
  mSnapshots.clear();
  delete mPreviewBand; // caller-owned per createRubberBand(); delete, not hide
  mPreviewBand = nullptr;
  mDragging = false;
}

// ---------------------------------------------------------------------------
// PaleoDeleteFeatureTool
// ---------------------------------------------------------------------------

PaleoDeleteFeatureTool::PaleoDeleteFeatureTool( QgsMapCanvas *canvas, QgsVectorLayer *layer )
  : QgsMapToolEdit( canvas ) // NOT QgsMapToolAdvancedDigitizing: plain canvas* hooks
  , mLayer( layer )
{
  setToolName( tr( "删除要素" ) );
  setCursor( QCursor( Qt::ArrowCursor ) );
}

PaleoDeleteFeatureTool::~PaleoDeleteFeatureTool() = default;

void PaleoDeleteFeatureTool::activate()
{
  QgsMapToolEdit::activate();
}

void PaleoDeleteFeatureTool::deactivate()
{
  QgsMapToolEdit::deactivate();
}

QgsVectorLayer *PaleoDeleteFeatureTool::targetLayer() const
{
  return mLayer ? mLayer.data() : qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
}

void PaleoDeleteFeatureTool::canvasReleaseEvent( QgsMapMouseEvent *e )
{
  QgsMapToolEdit::canvasReleaseEvent( e ); // empty base hook; chained for parity
  if ( e->button() != Qt::LeftButton )
    return;

  QgsVectorLayer *vl = targetLayer();
  if ( !vl || !vl->isEditable() )
  {
    // The host owns edit sessions; a refusal is a warning, not an abort.
    emit messageEmitted( vl ? tr( "无法删除要素：%1 不在编辑状态" ).arg( vl->name() )
                            : tr( "无法删除要素：没有目标图层" ),
                         Qgis::MessageLevel::Warning );
    return;
  }

  // 命中式删除（QGIS 节点工具惯例：点击作用于光标下的要素）——不再一键删
  // 整个选区。搜索半径用原生顶点搜索容差（地图单位），点/线/面统一走
  // 容差圆盘 intersects。
  const double radius = QgsTolerance::vertexSearchRadius( canvas()->mapSettings() );
  const QgsGeometry disc = QgsGeometry::fromPointXY( e->mapPoint() ).buffer( radius, 8 );
  QgsFeatureIds hits;
  QgsFeature feature;
  QgsFeatureIterator it = vl->getFeatures( QgsFeatureRequest()
                                             .setFilterRect( disc.boundingBox() )
                                             .setFlags( Qgis::FeatureRequestFlag::ExactIntersect ) );
  while ( it.nextFeature( feature ) )
  {
    if ( feature.hasGeometry() && feature.geometry().intersects( disc ) )
      hits.insert( feature.id() );
  }

  if ( hits.isEmpty() )
  {
    // Nothing under the cursor: warn, but do NOT emit editAborted().
    emit messageEmitted( tr( "点击位置没有可删除的要素" ), Qgis::MessageLevel::Warning );
    return;
  }

  vl->beginEditCommand( tr( "删除要素" ) );
  const bool ok = vl->deleteFeatures( hits );
  if ( ok )
  {
    vl->endEditCommand(); // one edit command per click → one native undo step
    mDeletedCount += static_cast<int>( hits.size() );
    emit featureEdited( vl->id() );
    emit messageEmitted( tr( "已删除 %1 个要素" ).arg( hits.size() ), Qgis::MessageLevel::Info );
  }
  else
  {
    vl->destroyEditCommand(); // refused — stay clean
  }
}

void PaleoDeleteFeatureTool::keyPressEvent( QKeyEvent *e )
{
  if ( e->key() == Qt::Key_Escape )
    emit editAborted(); // §42.15: owner deactivates the tool via unsetMapTool()
  QgsMapToolEdit::keyPressEvent( e );
}
