// 层：QGIS 封装
#include "topologicalindex.h"

#include <qgscoordinatereferencesystem.h>
#include <qgsgeometry.h>
#include <qgspointlocator.h>
#include <qgsvectorlayer.h>

QgisTopologicalIndex::QgisTopologicalIndex( QObject *parent )
  : QObject( parent )
{
}

QgisTopologicalIndex::~QgisTopologicalIndex()
{
  for ( LayerEntry &e : m_entries )
    delete e.locator;
}

void QgisTopologicalIndex::setLayers( QgsVectorLayer *scope, const QList<QgsVectorLayer *> &others )
{
  m_scope = scope;
  QList<QgsVectorLayer *> wanted;
  if ( scope )
    wanted.append( scope );
  for ( QgsVectorLayer *l : others )
    if ( l && !wanted.contains( l ) )
      wanted.append( l );
  // scope 恒为首条（查询的 CRS/坐标系语义锚点）。

  // 增量增删：仍登记的层保留原条目（脏位随编辑信号存活），离场的摘除。
  for ( int i = m_entries.size() - 1; i >= 0; --i )
  {
    if ( !wanted.contains( m_entries.at( i ).layer.data() ) )
    {
      delete m_entries[i].locator;
      m_entries.removeAt( i );
    }
  }
  for ( QgsVectorLayer *l : wanted )
  {
    if ( !entryFor( l ) )
    {
      LayerEntry e;
      e.layer = l;
      m_entries.append( e );
      watchLayer( l );
    }
  }
}

void QgisTopologicalIndex::watchLayer( QgsVectorLayer *layer )
{
  const QPointer<QgsVectorLayer> lp = layer;
  // 任一编辑信号 → 整层标脏（粗粒度）：locator 内部对提交/回滚的跟踪不可
  // 靠（fid 重排无信号），宁可多重建不可带脏查询。
  connect( layer, &QgsVectorLayer::geometryChanged, this, [this, lp]( QgsFeatureId, const QgsGeometry & ) {
    markDirty( lp );
  } );
  connect( layer, &QgsVectorLayer::featureAdded, this, [this, lp]( QgsFeatureId ) {
    markDirty( lp );
  } );
  connect( layer, &QgsVectorLayer::featureDeleted, this, [this, lp]( QgsFeatureId ) {
    markDirty( lp );
  } );
  connect( layer, &QgsVectorLayer::afterRollBack, this, [this, lp] {
    markDirty( lp );
  } );
  // 会话边界（提交的 fid 重排、回滚的整批还原都在此收口）。
  connect( layer, &QgsVectorLayer::editingStarted, this, [this, lp] {
    markDirty( lp );
  } );
  connect( layer, &QgsVectorLayer::editingStopped, this, [this, lp] {
    markDirty( lp );
  } );
  connect( layer, &QObject::destroyed, this, [this, lp] {
    for ( int i = m_entries.size() - 1; i >= 0; --i )
    {
      if ( m_entries.at( i ).layer == lp || m_entries.at( i ).layer.data() == nullptr )
      {
        delete m_entries[i].locator;
        m_entries.removeAt( i );
      }
    }
  } );
}

void QgisTopologicalIndex::markDirty( const QPointer<QgsVectorLayer> &layer )
{
  if ( LayerEntry *e = entryFor( layer.data() ) )
    e->dirty = true;
}

QgisTopologicalIndex::LayerEntry *QgisTopologicalIndex::entryFor( QgsVectorLayer *layer )
{
  for ( LayerEntry &e : m_entries )
    if ( e.layer.data() == layer )
      return &e;
  return nullptr;
}

void QgisTopologicalIndex::ensureReady( LayerEntry &e )
{
  if ( !e.layer )
    return;
  if ( !e.dirty && e.locator && e.locator->hasIndex() )
    return;

  delete e.locator;
  e.locator = new QgsPointLocator( e.layer ); // 无目标 CRS：索引 = 层坐标
  e.locator->init( -1, /*relaxed=*/true );    // R-tree 构建走任务线程
  if ( e.locator->isIndexing() )
    e.locator->waitForIndexingFinished(); // 有界等待（数百要素毫秒级）
  e.dirty = false;
}

QList<QgisTopologicalIndex::VertexHit> QgisTopologicalIndex::verticesNear(
    const QgsPointXY &pos, double radius, bool includeOthers )
{
  QList<VertexHit> out;
  if ( !m_scope )
    return out;
  const QgsCoordinateReferenceSystem scopeCrs = m_scope->crs();
  for ( LayerEntry &e : m_entries )
  {
    if ( !e.layer )
      continue;
    if ( e.layer != m_scope && !includeOthers )
      continue;
    if ( !( e.layer->crs() == scopeCrs ) ) // 同 CRS 防御（登记面已把关）
      continue;
    ensureReady( e );
    if ( !e.locator )
      continue;
    const QgsPointLocator::MatchList matches = e.locator->verticesInRect( pos, radius );
    for ( const QgsPointLocator::Match &m : matches )
    {
      if ( !m.hasVertex() )
        continue;
      VertexHit h;
      h.layer = e.layer;
      h.fid = m.featureId();
      h.vertexNr = m.vertexIndex();
      h.pos = m.point();
      out.append( h );
    }
  }
  return out;
}

QList<QgisTopologicalIndex::EdgeHit> QgisTopologicalIndex::edgesNear(
    const QgsRectangle &rect, bool includeOthers )
{
  QList<EdgeHit> out;
  if ( !m_scope )
    return out;
  const QgsCoordinateReferenceSystem scopeCrs = m_scope->crs();
  for ( LayerEntry &e : m_entries )
  {
    if ( !e.layer )
      continue;
    if ( e.layer != m_scope && !includeOthers )
      continue;
    if ( !( e.layer->crs() == scopeCrs ) )
      continue;
    ensureReady( e );
    if ( !e.locator )
      continue;
    const QgsPointLocator::MatchList matches = e.locator->edgesInRect( rect );
    for ( const QgsPointLocator::Match &m : matches )
    {
      if ( !m.hasEdge() )
        continue;
      EdgeHit h;
      h.layer = e.layer;
      h.fid = m.featureId();
      h.vertexNr = m.vertexIndex();
      m.edgePoints( h.p1, h.p2 );
      out.append( h );
    }
  }
  return out;
}

bool QgisTopologicalIndex::isLayerIndexed( QgsVectorLayer *layer ) const
{
  for ( const LayerEntry &e : m_entries )
    if ( e.layer.data() == layer )
      return !e.dirty && e.locator && e.locator->hasIndex();
  return false;
}

int QgisTopologicalIndex::indexedLayerCount() const
{
  int n = 0;
  for ( const LayerEntry &e : m_entries )
    if ( !e.dirty && e.locator && e.locator->hasIndex() )
      ++n;
  return n;
}
