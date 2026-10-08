// 层：QGIS 封装
#include "previewidentify.h"

#include <QPointer>
#include <QSet>

#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsmaplayer.h>
#include <qgsrasterblock.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsspatialindex.h>
#include <qgsvectorlayer.h>

#include <cmath>

namespace
{
QVariantMap attributesToMap( QgsVectorLayer *layer, const QgsFeature &feature )
{
  QVariantMap out;
  if ( !layer )
    return out;
  const QgsFields fields = layer->fields();
  const QgsAttributes attrs = feature.attributes();
  for ( int i = 0; i < fields.size() && i < attrs.size(); ++i )
    out.insert( fields.at( i ).name(), attrs.at( i ) );
  return out;
}
} // namespace

PreviewIdentifyCore::PreviewIdentifyCore( QObject *parent )
  : QObject( parent )
{
}

PreviewIdentifyCore::~PreviewIdentifyCore() = default;

QgsSpatialIndex *PreviewIdentifyCore::ensureIndex( QgsVectorLayer *layer )
{
  if ( !layer || !layer->isValid() )
    return nullptr;
  auto it = m_indexCache.find( layer );
  if ( it != m_indexCache.end() && it.value() )
    return it.value().data();
  auto index = QSharedPointer<QgsSpatialIndex>::create( layer->getFeatures() );
  QgsSpatialIndex *raw = index.data();
  m_indexCache.insert( layer, std::move( index ) );
  hookLayerInvalidation( layer );
  return raw;
}

void PreviewIdentifyCore::dropIndex( QgsMapLayer *layer )
{
  auto *vl = qobject_cast<QgsVectorLayer *>( layer );
  if ( vl )
    m_indexCache.remove( vl );
}

int PreviewIdentifyCore::indexCacheSize() const
{
  return m_indexCache.size();
}

void PreviewIdentifyCore::hookLayerInvalidation( QgsVectorLayer *layer )
{
  // 每层只挂一次：索引失效后 ensureIndex 会重建，重建时重复挂会让 layer 上的
  // 连接数随编辑次数无界增长（长期共享工程层尤其明显）。m_watched 记「已挂」
  // 的层，只在层析构时摘除。
  if ( !layer || m_watched.contains( layer ) )
    return;
  m_watched.insert( layer );
  // #235：要素编辑/提交同样让缓存索引陈旧（新要素漏报、已删要素回查落空）。
  // 预览页当前全部传独立新建层，触发面为零；一旦接共享工程层即现症——
  // 建索引时挂失效钩子。信号集对齐 topologicalindex.cpp 的 watchLayer
  // （fid 重排在编辑会话边界收口，宁可多重建不可带脏查询）：任一编辑/提交
  // 信号 → 缓存行即删（重建时按当前数据重扫）。
  auto dropKey = [this, layer] { m_indexCache.remove( layer ); };
  connect( layer, &QgsVectorLayer::geometryChanged, this,
           [dropKey]( QgsFeatureId, const QgsGeometry & ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::featureAdded, this,
           [dropKey]( QgsFeatureId ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::featureDeleted, this,
           [dropKey]( QgsFeatureId ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::committedFeaturesAdded, this,
           [dropKey]( const QString &, const QgsFeatureList & ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::committedFeaturesRemoved, this,
           [dropKey]( const QString &, const QgsFeatureIds & ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::committedGeometriesChanges, this,
           [dropKey]( const QString &, const QgsGeometryMap & ) { dropKey(); } );
  connect( layer, &QgsVectorLayer::afterCommitChanges, this, dropKey );
  connect( layer, &QgsVectorLayer::afterRollBack, this, dropKey );
  connect( layer, &QgsVectorLayer::editingStarted, this, dropKey );
  connect( layer, &QgsVectorLayer::editingStopped, this, dropKey );
  // 层析构 → 缓存行清除（QHash 键悬空是真实风险，这里根治）。
  // 注意 QPointer 在 destroyed 信号到达前已置空——按裸指针值删哈希键
  //（此时对象正在析构，键只作地址比较用）。
  QgsVectorLayer *const key = layer;
  connect( layer, &QObject::destroyed, this, [this, key] {
    m_indexCache.remove( key );
    m_watched.remove( key );
  } );
}

QVector<PreviewIdentifyResult> PreviewIdentifyCore::identifyPoint(
    const QList<QgsMapLayer *> &layers, const QgsPointXY &point, double toleranceMapUnits )
{
  QVector<PreviewIdentifyResult> out;
  const double tol = qMax( 0.0, toleranceMapUnits );
  QgsRectangle filter( point.x() - tol, point.y() - tol, point.x() + tol, point.y() + tol );

  // 层序自顶而下——先命中的在上面（结果面板序 = 视觉序）。
  for ( QgsMapLayer *ml : layers )
  {
    if ( !ml )
      continue;
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( ml ) )
    {
      QgsFeatureRequest req;
      req.setFilterRect( filter );
      req.setLimit( 25 );
      if ( QgsSpatialIndex *index = ensureIndex( vl ) )
      {
        // 空间索引先粗筛（bbox），再按几何精筛（点容差与真实相交）。
        const QList<QgsFeatureId> ids = index->intersects( filter );
        for ( QgsFeatureId fid : ids )
        {
          QgsFeature full;
          if ( !vl->getFeatures( QgsFeatureRequest( fid ) ).nextFeature( full ) )
            continue;
          const QgsGeometry geom = full.geometry();
          if ( !geom.isNull() && !geom.intersects( filter ) )
            continue;
          PreviewIdentifyResult r;
          r.layer = vl;
          r.layerName = vl->name();
          r.featureId = full.id();
          r.attributes = attributesToMap( vl, full );
          r.geometry = geom;
          out.append( r );
          if ( out.size() >= 200 )
            return out; // 防极端密度
        }
      }
      else
      {
        QgsFeatureIterator it = vl->getFeatures( req );
        QgsFeature f;
        while ( it.nextFeature( f ) )
        {
          PreviewIdentifyResult r;
          r.layer = vl;
          r.layerName = vl->name();
          r.featureId = f.id();
          r.attributes = attributesToMap( vl, f );
          r.geometry = f.geometry();
          out.append( r );
        }
      }
    }
    else if ( auto *rl = qobject_cast<QgsRasterLayer *>( ml ) )
    {
      double nearest = 0.0;
      double bilinear = 0.0;
      if ( sampleRaster( rl, point, 1, &nearest, &bilinear ) )
      {
        PreviewIdentifyResult r;
        r.layer = rl;
        r.layerName = rl->name();
        r.isRaster = true;
        r.rasterBand = 1;
        r.rasterValueNearest = nearest;
        r.rasterValueBilinear = bilinear;
        r.rasterValid = true;
        out.append( r );
      }
    }
  }
  return out;
}

QVector<PreviewIdentifyResult> PreviewIdentifyCore::identifyRect(
    const QList<QgsMapLayer *> &layers, const QgsRectangle &rect, int maxPerLayer )
{
  QVector<PreviewIdentifyResult> out;
  if ( rect.isEmpty() )
    return out;
  for ( QgsMapLayer *ml : layers )
  {
    auto *vl = qobject_cast<QgsVectorLayer *>( ml );
    if ( !vl || !vl->isValid() )
      continue;
    int taken = 0;
    if ( QgsSpatialIndex *index = ensureIndex( vl ) )
    {
      const QList<QgsFeatureId> ids = index->intersects( rect );
      for ( QgsFeatureId fid : ids )
      {
        if ( taken >= maxPerLayer )
          break;
        QgsFeature f;
        if ( !vl->getFeatures( QgsFeatureRequest( fid ) ).nextFeature( f ) )
          continue;
        if ( !f.geometry().isNull() && !f.geometry().intersects( rect ) )
          continue;
        PreviewIdentifyResult r;
        r.layer = vl;
        r.layerName = vl->name();
        r.featureId = f.id();
        r.attributes = attributesToMap( vl, f );
        r.geometry = f.geometry();
        out.append( r );
        ++taken;
      }
    }
    else
    {
      QgsFeatureRequest req;
      req.setFilterRect( rect );
      req.setLimit( maxPerLayer );
      QgsFeatureIterator it = vl->getFeatures( req );
      QgsFeature f;
      while ( it.nextFeature( f ) && taken < maxPerLayer )
      {
        PreviewIdentifyResult r;
        r.layer = vl;
        r.layerName = vl->name();
        r.featureId = f.id();
        r.attributes = attributesToMap( vl, f );
        r.geometry = f.geometry();
        out.append( r );
        ++taken;
      }
    }
  }
  return out;
}

bool PreviewIdentifyCore::sampleRaster( QgsRasterLayer *layer, const QgsPointXY &point,
                                        int band, double *nearest, double *bilinear )
{
  if ( !layer || !layer->isValid() || !layer->dataProvider() )
    return false;
  bool ok = false;
  const double v = layer->dataProvider()->sample( point, band, &ok );
  if ( !ok || !std::isfinite( v ) )
    return false;
  if ( nearest )
    *nearest = v;
  if ( !bilinear )
    return true;

  // 双线性插值：取点所在像元 2×2 邻域，按亚像元位置加权。
  *bilinear = v; // 退化初值（边缘像元）
  const QgsRectangle ext = layer->extent();
  const int w = layer->width();
  const int h = layer->height();
  if ( ext.isEmpty() || w < 2 || h < 2 || !ext.contains( point ) )
    return true;
  const double muppX = ext.width() / w;
  const double muppY = ext.height() / h;
  const double fx = ( point.x() - ext.xMinimum() ) / muppX - 0.5;
  const double fy = ( ext.yMaximum() - point.y() ) / muppY - 0.5;
  const int x0 = qBound( 0, int( std::floor( fx ) ), w - 2 );
  const int y0 = qBound( 0, int( std::floor( fy ) ), h - 2 );
  const double tx = qBound( 0.0, fx - x0, 1.0 );
  const double ty = qBound( 0.0, fy - y0, 1.0 );
  QgsRectangle window( ext.xMinimum() + x0 * muppX, ext.yMaximum() - ( y0 + 2 ) * muppY,
                       ext.xMinimum() + ( x0 + 2 ) * muppX, ext.yMaximum() - y0 * muppY );
  std::unique_ptr<QgsRasterBlock> block(
      layer->dataProvider()->block( band, window, 2, 2 ) );
  if ( !block || block->isEmpty() )
    return true;
  double v00 = 0.0, v10 = 0.0, v01 = 0.0, v11 = 0.0;
  bool nd00 = true, nd10 = true, nd01 = true, nd11 = true;
  v00 = block->valueAndNoData( 0, 0, nd00 );
  v10 = block->valueAndNoData( 0, 1, nd10 );
  v01 = block->valueAndNoData( 1, 0, nd01 );
  v11 = block->valueAndNoData( 1, 1, nd11 );
  const bool ok00 = !nd00, ok10 = !nd10, ok01 = !nd01, ok11 = !nd11;
  if ( ok00 && ok10 && ok01 && ok11 )
  {
    const double top = v00 * ( 1.0 - tx ) + v10 * tx;
    const double bottom = v01 * ( 1.0 - tx ) + v11 * tx;
    *bilinear = top * ( 1.0 - ty ) + bottom * ty;
  }
  return true;
}
