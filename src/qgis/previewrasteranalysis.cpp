// 层：QGIS 封装
#include "previewrasteranalysis.h"

#include <QFileInfo>

#include <qgscolorramp.h>
#include <qgscolorrampimpl.h>
#include <qgscolorrampshader.h>
#include <qgsrasterbandstats.h>
#include <qgsrasterblock.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgssinglebandpseudocolorrenderer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace PreviewRasterAnalysis
{

qint64 Histogram::totalCount() const
{
  qint64 total = 0;
  for ( qint64 c : counts )
    total += c;
  return total;
}

double Histogram::maximumCount() const
{
  qint64 m = 0;
  for ( qint64 c : counts )
    m = std::max( m, c );
  return double( m );
}

BandSummary summarize( QgsRasterLayer *layer, int band )
{
  BandSummary out;
  if ( !layer || !layer->isValid() || !layer->dataProvider() )
    return out;
  const QgsRasterBandStats st =
      layer->dataProvider()->bandStatistics( band, Qgis::RasterBandStatistic::All );
  if ( st.minimumValue > st.maximumValue )
    return out; // 无有效像元
  out.valid = true;
  out.min = st.minimumValue;
  out.max = st.maximumValue;
  out.mean = st.mean;
  out.stdDev = st.stdDev;
  out.totalPixels = qint64( layer->width() ) * layer->height();
  // 有效像元占比走直方图 nonNullCount（bandStatistics 不带 per-pixel 计数）。
  const QgsRasterHistogram rh = layer->dataProvider()->histogram( band, 2, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN() );
  out.validPixels = rh.nonNullCount > 0 ? rh.nonNullCount : out.totalPixels;
  return out;
}

Histogram histogram( QgsRasterLayer *layer, int bins, int band )
{
  Histogram out;
  if ( !layer || !layer->isValid() || !layer->dataProvider() || bins <= 0 )
    return out;
  const QgsRasterHistogram rh = layer->dataProvider()->histogram( band, bins );
  if ( rh.binCount <= 0 || rh.histogramVector.empty() )
    return out;
  out.valid = true;
  out.bins = rh.binCount;
  out.counts.reserve( rh.binCount );
  for ( int i = 0; i < rh.binCount; ++i )
    out.counts.append( rh.histogramVector[std::size_t( i )] );
  // QgsRasterHistogram 不带 lo/hi——界由 bandStatistics 定（与直方图同一缓存口径）。
  const QgsRasterBandStats st =
      layer->dataProvider()->bandStatistics( band, Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max );
  out.lo = st.minimumValue;
  out.hi = st.maximumValue;
  if ( !( out.lo < out.hi ) )
  {
    out.lo = out.lo - 0.5;
    out.hi = out.hi + 0.5;
  }
  return out;
}

QPair<double, double> stretchBounds( const Histogram &h, Stretch mode,
                                     double manualLo, double manualHi )
{
  switch ( mode )
  {
    case Stretch::Manual:
      if ( manualLo < manualHi )
        return { manualLo, manualHi };
      return { 0.0, 1.0 };
    case Stretch::Percent2To98:
    {
      if ( !h.valid || h.counts.isEmpty() )
        return { h.lo, h.hi };
      const qint64 total = h.totalCount();
      if ( total <= 0 )
        return { h.lo, h.hi };
      const double binWidth = ( h.hi - h.lo ) / h.bins;
      qint64 seen = 0;
      double lo = h.lo;
      double hi = h.hi;
      bool loSet = false;
      for ( int i = 0; i < h.bins; ++i )
      {
        seen += h.counts.at( i );
        if ( !loSet && double( seen ) / total >= 0.02 )
        {
          lo = h.lo + binWidth * i;
          loSet = true;
        }
        if ( double( seen ) / total >= 0.98 )
        {
          hi = h.lo + binWidth * ( i + 1 );
          break;
        }
      }
      if ( !( lo < hi ) )
        return { h.lo, h.hi };
      return { lo, hi };
    }
    case Stretch::MinMax:
    case Stretch::HistEq:
    default:
      if ( h.valid && h.lo < h.hi )
        return { h.lo, h.hi };
      return { 0.0, 1.0 };
  }
}

const QVector<RampPreset> &rampPresets()
{
  static const QVector<RampPreset> presets = {
      { QStringLiteral( "depthBlues" ), QStringLiteral( "等深蓝（浅→深）" ),
        QColor( QStringLiteral( "#CDE7F6" ) ), QColor( QStringLiteral( "#14507F" ) ),
        QColor(), false },
      { QStringLiteral( "terrain" ), QStringLiteral( "地形（绿→黄→棕）" ),
        QColor( QStringLiteral( "#A5D6A7" ) ), QColor( QStringLiteral( "#8D6E63" ) ),
        QColor( QStringLiteral( "#FFF59D" ) ), false },
      { QStringLiteral( "gray" ), QStringLiteral( "灰阶" ),
        QColor( QStringLiteral( "#F5F5F5" ) ), QColor( QStringLiteral( "#424242" ) ),
        QColor(), false },
      { QStringLiteral( "bipolar" ), QStringLiteral( "红白蓝（双极）" ),
        QColor( QStringLiteral( "#1976D2" ) ), QColor( QStringLiteral( "#DC2626" ) ),
        QColor( QStringLiteral( "#FFFFFF" ) ), true },
      { QStringLiteral( "magma" ), QStringLiteral( "岩浆（紫→橙）" ),
        QColor( QStringLiteral( "#3B0F70" ) ), QColor( QStringLiteral( "#F98E09" ) ),
        QColor( QStringLiteral( "#8C2981" ) ), false },
  };
  return presets;
}

const RampPreset *rampPreset( const QString &id )
{
  for ( const RampPreset &p : rampPresets() )
    if ( p.id == id )
      return &p;
  return nullptr;
}

bool applyPseudoColorRenderer( QgsRasterLayer *layer, int band, double lo, double hi,
                               const RampPreset &preset, bool invert,
                               Classification cls, int quantileClasses )
{
  if ( !layer || !layer->isValid() || !layer->dataProvider() )
    return false;
  if ( !( lo < hi ) )
    return false;
  const int clampedClasses = qBound( 2, quantileClasses, 256 );

  // 先造裸 shader 占位（字段在 ctor 里填；下面按 ramp 重建）。
  QgsColorRampShader *rampShader = new QgsColorRampShader();
  rampShader->setMinimumValue( lo );
  rampShader->setMaximumValue( hi );
  rampShader->setClassificationMode( Qgis::ShaderClassificationMethod::Continuous );

  QColor c1 = preset.c1;
  QColor c2 = preset.c2;
  if ( invert )
    std::swap( c1, c2 );

  // 双极色带跨零时在中点放中间色停（蓝-白-红三停）。
  QgsGradientStopsList stops;
  if ( preset.bipolar && preset.mid.isValid() && lo < 0.0 && hi > 0.0 )
    stops.append( QgsGradientStop( ( 0.0 - lo ) / ( hi - lo ), preset.mid ) );
  QgsGradientColorRamp *ramp = new QgsGradientColorRamp( c1, c2, false, stops );
  // ctor 直接接管 ramp 所有权（三参内 min/max/type/mode 全钉）。
  delete rampShader;
  rampShader = new QgsColorRampShader( lo, hi, ramp, Qgis::ShaderInterpolationMethod::Linear,
                                       Qgis::ShaderClassificationMethod::Continuous );

  if ( cls == Classification::Continuous )
  {
    // 地雷防护：Continuous 也必须先分类——itemList 为空时 shade() 全 false，
    // 像元全透明（画布白板）。classifyColorRamp 按当前 ramp 填 itemList。
    rampShader->classifyColorRamp( band, layer->extent(), layer->dataProvider() );
  }
  else
  {
    // HistEq：直方图分位数断点 → 离散 itemList（均衡语义的渲染落地）。
    const Histogram h = histogram( layer, qMax( 64, clampedClasses * 4 ), band );
    if ( !h.valid || h.totalCount() <= 0 )
      rampShader->classifyColorRamp( band, layer->extent(), layer->dataProvider() );
    else
    {
      QList<QgsColorRampShader::ColorRampItem> items;
      const qint64 total = h.totalCount();
      const double binWidth = ( h.hi - h.lo ) / h.bins;
      qint64 seen = 0;
      int nextClass = 1;
      items.append( { lo, ramp->color( 0.0 ) } );
      for ( int i = 0; i < h.bins && nextClass < clampedClasses; ++i )
      {
        seen += h.counts.at( i );
        const double q = double( seen ) / total;
        if ( q >= double( nextClass ) / clampedClasses )
        {
          const double v = h.lo + binWidth * ( i + 1 );
          const double t = ( v - lo ) / ( hi - lo );
          items.append( { qBound( lo, v, hi ), ramp->color( qBound( 0.0, t, 1.0 ) ) } );
          ++nextClass;
        }
      }
      items.append( { hi, ramp->color( 1.0 ) } );
      rampShader->setColorRampItemList( items );
    }
  }

  auto *shader = new QgsRasterShader();
  shader->setRasterShaderFunction( rampShader ); // shader 接管 ownership
  layer->setRenderer( new QgsSingleBandPseudoColorRenderer( layer->dataProvider(), band, shader ) );
  layer->triggerRepaint();
  return true;
}

QVector<ProfileSample> sampleProfile( QgsRasterLayer *layer, const QgsPointXY &p1,
                                      const QgsPointXY &p2, int count )
{
  QVector<ProfileSample> out;
  if ( !layer || !layer->isValid() || !layer->dataProvider() || count < 2 )
    return out;
  const double total = p1.distance( p2 );
  if ( !( total > 0.0 ) )
    return out;
  const QgsRectangle ext = layer->extent();
  out.reserve( count );
  for ( int i = 0; i < count; ++i )
  {
    const double t = double( i ) / ( count - 1 );
    const QgsPointXY p( p1.x() + ( p2.x() - p1.x() ) * t,
                        p1.y() + ( p2.y() - p1.y() ) * t );
    ProfileSample s;
    s.distance = total * t;
    if ( ext.contains( p ) )
    {
      bool ok = false;
      const double v = layer->dataProvider()->sample( p, 1, &ok );
      s.valid = ok && std::isfinite( v );
      s.value = s.valid ? v : 0.0;
    }
    out.append( s );
  }
  return out;
}

QVector<Extremum> localExtrema( QgsRasterLayer *layer, int band, int window,
                                double prominence, int maxCount )
{
  QVector<Extremum> out;
  if ( !layer || !layer->isValid() || !layer->dataProvider() || window < 3 )
    return out;
  const int w = layer->width();
  const int h = layer->height();
  if ( w <= 0 || h <= 0 )
    return out;
  const int half = window / 2;

  // 读整块（预览栅格是小网格；大图走 summarize 的缓存口径不受此影响）。
  std::unique_ptr<QgsRasterBlock> block(
      layer->dataProvider()->block( band, layer->extent(), w, h ) );
  if ( !block )
    return out;

  struct Cand
  {
    int x = 0;
    int y = 0;
    double v = 0.0;
    double prom = 0.0;
    bool peak = false;
  };
  QVector<Cand> cands;
  for ( int y = half; y < h - half; y += 1 )
  {
    for ( int x = half; x < w - half; x += 1 )
    {
      const double v = block->value( y, x );
      if ( !std::isfinite( v ) )
        continue;
      double mean = 0.0;
      int n = 0;
      bool isMax = true;
      bool isMin = true;
      for ( int dy = -half; dy <= half && ( isMax || isMin ); ++dy )
      {
        for ( int dx = -half; dx <= half; ++dx )
        {
          if ( dx == 0 && dy == 0 )
            continue;
          const double nv = block->value( y + dy, x + dx );
          if ( !std::isfinite( nv ) )
            continue;
          mean += nv;
          ++n;
          if ( nv >= v )
            isMax = false;
          if ( nv <= v )
            isMin = false;
        }
      }
      if ( n == 0 || ( !isMax && !isMin ) )
        continue;
      if ( n > 0 )
        mean /= n;
      const double prom = std::abs( v - mean );
      if ( prom < prominence )
        continue;
      cands.append( { x, y, v, prom, isMax } );
    }
  }
  // 按显著性降序取前 maxCount。
  std::sort( cands.begin(), cands.end(),
             []( const Cand &a, const Cand &b ) { return a.prom > b.prom; } );
  const QgsRectangle ext = layer->extent();
  const double muppX = ext.width() / w;
  const double muppY = ext.height() / h;
  for ( int i = 0; i < cands.size() && out.size() < maxCount; ++i )
  {
    const Cand &c = cands.at( i );
    Extremum e;
    e.pos = QgsPointXY( ext.xMinimum() + ( c.x + 0.5 ) * muppX,
                        ext.yMaximum() - ( c.y + 0.5 ) * muppY );
    e.value = c.v;
    e.peak = c.peak;
    out.append( e );
  }
  return out;
}

bool hasPyramids( QgsRasterLayer *layer )
{
  if ( !layer || !layer->dataProvider() )
    return false;
  const QList<QgsRasterPyramid> pyramids = layer->dataProvider()->buildPyramidList();
  for ( const QgsRasterPyramid &p : pyramids )
    if ( p.getExists() )
      return true;
  return false;
}

qint64 fileSizeBytes( const QString &path )
{
  if ( path.isEmpty() )
    return 0;
  const QFileInfo fi( path );
  return fi.exists() ? qint64( fi.size() ) : qint64( 0 );
}

QString bigRasterHint( QgsRasterLayer *layer )
{
  if ( !layer )
    return QString();
  const QString src = layer->source();
  // source 可能带 provider 前缀（url:file:///…）——按首个真实路径探测。
  QString path = src;
  const int schemeIdx = path.indexOf( QStringLiteral( "file://" ) );
  if ( schemeIdx >= 0 )
    path = path.mid( schemeIdx + 7 );
  if ( fileSizeBytes( path ) <= 50LL * 1024 * 1024 ) // ≤50MB 不提示
    return QString();
  if ( hasPyramids( layer ) )
    return QString();
  return QStringLiteral( "大图（%1 MB）且无金字塔——首次全图渲染较慢，可继续交互；"
                         "建议对该栅格构建金字塔加速" )
      .arg( fileSizeBytes( path ) / ( 1024.0 * 1024.0 ), 0, 'f', 0 );
}

} // namespace PreviewRasterAnalysis
