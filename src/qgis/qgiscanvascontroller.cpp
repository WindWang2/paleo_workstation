// 层：QGIS 封装
#include "qgiscanvascontroller.h"

#include "qgisprojectservice.h"
#include "projectmapreference.h"
#include "../catalog/datacatalog.h"

#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgslayertree.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsrectangle.h>
#include <qgssnappingutils.h>

#include <QColor>
#include <QWidget>
#include <QSettings>
#include <algorithm>

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
  delete m_bridge;      // references the canvas — die before it
  m_bridge = nullptr;   // the destroyed() hook below re-runs on delete m_canvas
  // The canvas is created parentless; if it was embedded in a widget that died,
  // the destroyed() hook below already nulled m_canvas — no double delete.
  delete m_canvas;
}

QgsSnappingConfig QgisCanvasController::nativeSnappingConfig()
{
  QgsSnappingConfig cfg;
  cfg.setEnabled( true );
  cfg.setMode( Qgis::SnappingMode::AllLayers );           // 全图层捕捉（编图工位默认）
  cfg.setTypeFlag( Qgis::SnappingType::Vertex |
                   Qgis::SnappingType::Segment ); // 顶点+边
  cfg.setTolerance(std::clamp(QSettings().value(QStringLiteral("editing/snapTolerancePx"), 10).toInt(), 1, 100));
  cfg.setUnits( Qgis::MapToolUnit::Pixels );               // 屏幕像素容差
  return cfg;
}

QgsMapCanvas *QgisCanvasController::canvas()
{
  if ( !m_canvas )
  {
    m_canvas = new QgsMapCanvas( nullptr ); // parentless; embedder may reparent
    m_canvas->enableAntiAliasing( true );
    m_canvas->setCanvasColor( QColor( QStringLiteral( "#FFFFFF" ) ) ); // DESIGN.md colors.surface
    // 捕捉原生启用（QGIS_NATIVE_ADOPTION）：QgsMapToolCapture/编辑工具经
    // canvas->snappingUtils() 自动拾取配置——这里是唯一接线点。
    m_canvas->snappingUtils()->setConfig( nativeSnappingConfig() );
    // If an embedding widget parents the canvas and outlives/destroys it,
    // drop the dangling pointer before ~QgisCanvasController runs.
    connect( m_canvas, &QObject::destroyed, this, [this] {
      m_canvas = nullptr;
      delete m_bridge;   // holds a canvas pointer — must not outlive it
      m_bridge = nullptr;
    } );
    bindProject();
  }
  return m_canvas;
}

void QgisCanvasController::applyProjectCrs( QgsProject *project )
{
  if ( !project )
    return;
  // 工程服务已按显式配准设置 CRS 和转换上下文；无配准才使用局部网格。
  if (!project->crs().isValid())
    project->setCrs(QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  if ( m_canvas )
  {
    m_canvas->mapSettings().setTransformContext(project->transformContext());
    m_canvas->setDestinationCrs(project->crs());
  }
}

void QgisCanvasController::bindProject()
{
  if ( !m_canvas )
    return;

  // Production layers live on QgisProjectService's QgsProject, not the
  // singleton. The composition root parents every service to AppContext, so
  // the service resolves as a sibling through the parent chain; a bare
  // controller (tests) falls back to QgsProject::instance(), matching the
  // layer service's documented fallback.
  QgisProjectService *svc = nullptr;
  for ( QObject *p = parent(); p && !svc; p = p->parent() )
    svc = p->findChild<QgisProjectService *>();
  QgsProject *proj = svc ? svc->project() : nullptr;
  if ( !proj )
    proj = QgsProject::instance();
  if ( !proj )
    return;

  // Apply the configured project CRS BEFORE creating the bridge: it snapshots
  // first-layer CRS state at construction and on the first setCanvasLayers.
  applyProjectCrs( proj );
  m_canvas->setProject( proj ); // canvas-scoped lookups resolve against this project

  // The bridge watches the layer tree and defers canvas-layer updates onto the
  // event loop — lazily instantiated layers appear live, released/removed ones
  // disappear, and project clear/read wipes the set. No manual setLayers needed.
  delete m_bridge;
  m_bridge = new QgsLayerTreeMapCanvasBridge( proj->layerTreeRoot(), m_canvas, this );
  m_bridge->setAutoSetupOnFirstLayer(false);

  if ( svc )
    connect( svc, &QgisProjectService::projectOpened, this, [this, svc] {
      QgsProject *p = svc->project();
      applyProjectCrs( p );
      // Rebind defensively: read() may rebuild the tree under the bridge.
      if ( p && m_canvas )
      {
        delete m_bridge;
        m_bridge = new QgsLayerTreeMapCanvasBridge( p->layerTreeRoot(), m_canvas, this );
        m_bridge->setAutoSetupOnFirstLayer(false);
      }
    } );
  if (svc)
    connect(svc, &QgisProjectService::mapConfigurationChanged, this, [this, svc] {
      if (!m_canvas) return;
      const auto oldCrs = m_canvas->mapSettings().destinationCrs();
      const auto newCrs = svc->project()->crs();
      auto extent = m_canvas->extent();
      try {
        auto context = svc->project()->transformContext();
        if (!context.hasTransform(oldCrs, newCrs)) context = m_canvas->mapSettings().transformContext();
        QgsCoordinateTransform transform(oldCrs, newCrs, context);
        if (transform.isValid()) extent = transform.transformBoundingBox(extent);
      } catch (const QgsCsException &) { extent = {}; }
      applyProjectCrs(svc->project());
      if (!extent.isEmpty()) m_canvas->setExtent(extent);
      else zoomToFullExtent();
      m_canvas->refresh();
    });
}

void QgisCanvasController::setMapTool( QgsMapTool *tool )
{
  if ( !tool )
  {
    deactivateTool(); // QgsMapCanvas::setMapTool(nullptr) is a no-op in QGIS
    return;
  }
  canvas()->setMapTool( tool ); // deactivates previous; reactivates if same tool
}

QgsMapTool *QgisCanvasController::activeTool() const
{
  return m_canvas ? m_canvas->mapTool() : nullptr;
}

void QgisCanvasController::deactivateTool()
{
  if ( QgsMapTool *tool = activeTool() )
    m_canvas->unsetMapTool( tool ); // includes tools installed by native widgets
}

void QgisCanvasController::zoomToFullExtent()
{
  if (!m_canvas) return;
  QgsRectangle extent;
  for (auto *layer : m_canvas->layers())
    if (!layer->customProperty("paleoBasemap").toBool()) {
      auto bounds = layer->extent();
      if (!bounds.isNull() && bounds.isEmpty()) bounds.grow(50);
      extent.combineExtentWith(paleo::mapreference::mapExtent(m_canvas, bounds, layer->crs()));
    }
  if (!extent.isEmpty()) {
    extent.scale(1.08);
    m_canvas->setExtent(extent);
    m_canvas->refresh();
  } else m_canvas->zoomToFullExtent();
}

void QgisCanvasController::setLayerResolver( LayerResolver resolver )
{
  m_layerResolver = std::move( resolver );
}

void QgisCanvasController::zoomToLayer( const QString &layerId )
{
  if ( !m_canvas )
    return;
  // Production layers live on QgisProjectService's QgsProject and are keyed
  // by manifest id, which is not QgsMapLayer::id(). The app installs a resolver
  // that goes through QgisLayerService — deliberately no QgsProject::instance()
  // fallback: a manifest id must never resolve against the singleton, and a
  // bare controller (tests) simply no-ops.
  QgsMapLayer *l = m_layerResolver ? m_layerResolver( layerId ) : nullptr;
  if ( !l )
    return;
  QgsRectangle ext = paleo::mapreference::mapExtent(m_canvas, l->extent(), l->crs());
  if ( ext.isEmpty() )
  {
    // 图层 CRS 与画布 CRS 无可用转换（如未配准工程里的 WGS84 图层）：
    // 回退按图层原始范围缩放——视图至少移动到图层坐标域，静默不动更糟。
    if ( l->extent().isNull() )
      return;
    ext = l->extent();
    ext.scale( 1.08 );
  }
  m_canvas->setExtent( ext );
  m_canvas->refresh();
}

void QgisCanvasController::zoomToPoint( double x, double y )
{
  if ( !m_canvas )
    return;
  // 保留当前视野宽高（用户选好的比例尺），视野无效时给一个 ~1km 的
  // 工程网格窗口，先按工程转换上下文换算到画布坐标。
  const auto local = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  QgsPointXY center;
  if (!paleo::mapreference::transformPoint(m_canvas, QgsPointXY(x, y), local,
                                          m_canvas->mapSettings().destinationCrs(), &center)) return;
  const auto fallback = paleo::mapreference::mapExtent(m_canvas, QgsRectangle(x-500, y-500, x+500, y+500), local);
  QgsRectangle e = m_canvas->extent();
  const double hw = e.isEmpty() ? fallback.width()/2 : e.width()/2;
  const double hh = e.isEmpty() ? fallback.height()/2 : e.height()/2;
  m_canvas->setExtent(QgsRectangle(center.x()-hw, center.y()-hh, center.x()+hw, center.y()+hh));
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
