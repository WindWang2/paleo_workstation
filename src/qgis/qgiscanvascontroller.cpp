#include "qgiscanvascontroller.h"

#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrectangle.h>

#include <QColor>
#include <QWidget>

// §41.3 SelectionContext debounce: the broadcast chain (map→well→seismic→map)
// must not ping-pong. The header fixes m_broadcasting as the public in-flight
// flag; nesting depth and the coalesce ("a re-broadcast is owed") bit are
// implementation detail, kept here at file scope so the header stays untouched.
// GUI-thread only, like every QObject service on the spine.
namespace
{
  struct BroadcastGuard
  {
    int depth = 0;        // unmatched beginSelectionBroadcast() calls
    bool pending = false; // a nested begin was swallowed → owe one coalesced re-broadcast
  };
  QHash<QgisCanvasController *, BroadcastGuard> s_guards;
}

QgisCanvasController::QgisCanvasController( QObject *parent )
  : QObject( parent )
{
}

QgisCanvasController::~QgisCanvasController()
{
  s_guards.remove( this );
  // The canvas is created parentless; if it was embedded in a widget that died,
  // the destroyed() hook below already nulled m_canvas — no double delete.
  delete m_canvas;
}

QgsMapCanvas *QgisCanvasController::canvas()
{
  if ( !m_canvas )
  {
    m_canvas = new QgsMapCanvas( nullptr ); // parentless; embedder may reparent
    m_canvas->enableAntiAliasing( true );
    m_canvas->setCanvasColor( QColor( QStringLiteral( "#FFFFFF" ) ) ); // DESIGN.md colors.surface
    // If an embedding widget parents the canvas and outlives/destroys it,
    // drop the dangling pointer before ~QgisCanvasController runs.
    connect( m_canvas, &QObject::destroyed, this, [this] { m_canvas = nullptr; } );
  }
  return m_canvas;
}

void QgisCanvasController::setMapTool( QgsMapTool *tool )
{
  if ( !tool )
  {
    deactivateTool(); // QgsMapCanvas::setMapTool(nullptr) is a no-op in QGIS
    return;
  }
  canvas()->setMapTool( tool ); // deactivates previous; reactivates if same tool
  // If the tool is deleted while active, canvas clears its own pointer via
  // mapToolDestroyed; mirror that so activeTool() never dangles.
  connect( tool, &QObject::destroyed, this, [this, tool] {
    if ( m_tool == tool )
      m_tool = nullptr;
  } );
  m_tool = tool;
}

QgsMapTool *QgisCanvasController::activeTool() const
{
  return m_tool;
}

void QgisCanvasController::deactivateTool()
{
  if ( m_canvas && m_tool )
    m_canvas->unsetMapTool( m_tool ); // proper deactivate path (Esc, §42.15)
  m_tool = nullptr;
}

void QgisCanvasController::zoomToFullExtent()
{
  if ( m_canvas )
    m_canvas->zoomToFullExtent();
}

void QgisCanvasController::zoomToLayer( const QString &layerId )
{
  if ( !m_canvas )
    return;
  // QgisLayerService instances are owned by QgsProject — resolve there.
  QgsMapLayer *l = QgsProject::instance()->mapLayer( layerId );
  if ( !l )
    return;
  const QgsRectangle ext = l->extent();
  if ( ext.isEmpty() )
    return;
  m_canvas->setExtent( ext );
  m_canvas->refresh();
}

void QgisCanvasController::beginSelectionBroadcast()
{
  BroadcastGuard &g = s_guards[this];
  if ( m_broadcasting )
    g.pending = true;    // echo arrived mid-broadcast → swallow it, owe one re-broadcast
  else
    m_broadcasting = true; // open the guarded window; callers check broadcasting()
  ++g.depth;
}

void QgisCanvasController::endSelectionBroadcast()
{
  auto it = s_guards.find( this );
  if ( it == s_guards.end() || it->depth == 0 )
    return;                  // unmatched end — ignore, never wedges the flag
  if ( --it->depth > 0 )
    return;                  // inner end of a nested pair — outer still in flight

  m_broadcasting = false;    // settle first: the emit below is a fresh broadcast cycle
  const bool oweBroadcast = it->pending;
  s_guards.erase( it );
  if ( oweBroadcast )
    emit selectionBroadcast( QStringList(), QStringLiteral( "coalesced" ) );
}
