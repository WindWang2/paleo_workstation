// 层：QGIS 封装
#include "previewrasteranalysis.h"
#include "../services/paleotaskservice.h"
#include <QCache>
#include <QCoreApplication>
#include <QFileInfo>
#include <memory>

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

namespace {
QString rasterKey(const QString &uri) {
  const QFileInfo file(uri.section('|', 0, 0));
  return uri + QString::number(file.size()) + ":" +
         QString::number(file.lastModified().toMSecsSinceEpoch());
}
} // namespace
void requestSummary(const QString &uri, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const BandSummary &)> ready) {
  // Cache is owner-thread only and bounded; immutable versions share statistics
  // across preview tabs and repeated TOC selection.
  static QCache<QString, BandSummary> cache(64);
  const auto key = rasterKey(uri);
  if (const auto *hit = cache.object(key)) {
    ready(*hit);
    return;
  }
  if (!tasks) {
    QgsRasterLayer layer(uri, "statistics", "gdal");
    ready(summarize(&layer));
    return;
  }
  auto summary = std::make_shared<BandSummary>();
  auto *task = tasks->start(
      QObject::tr("统计预览栅格"),
      [uri, summary](PaleoTask *running) {
        QgsRasterLayer layer(uri, "statistics", "gdal");
        if (!layer.isValid())
          return QCoreApplication::translate("PreviewRasterAnalysis", "无法读取预览栅格");
        running->reportStage("scan", 5);
        *summary = summarize(&layer);
        return QString();
      },
      {}, true);
  QObject::connect(receiver, &QObject::destroyed, task,
                   &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, receiver,
                   [task, summary, ready, key, uri] {
                     if (task->state() == PaleoTask::State::Succeeded &&
                         rasterKey(uri) == key) {
                       cache.insert(key, new BandSummary(*summary));
                       ready(*summary);
                     }
                   });
}
void requestHistogram(const QString &uri, int bins, PaleoTaskService *tasks,
                      QObject *receiver,
                      std::function<void(const Histogram &)> ready) {
  static QCache<QString, Histogram> cache(64);
  const auto identity = rasterKey(uri);
  const auto key = identity + ":" + QString::number(bins);
  if (const auto *hit = cache.object(key)) {
    ready(*hit);
    return;
  }
  if (!tasks) {
    QgsRasterLayer layer(uri, "histogram", "gdal");
    ready(histogram(&layer, bins));
    return;
  }
  auto result = std::make_shared<Histogram>();
  auto *task = tasks->start(
      QObject::tr("计算预览直方图"),
      [uri, bins, result](PaleoTask *running) {
        QgsRasterLayer layer(uri, "histogram", "gdal");
        if (!layer.isValid())
          return QCoreApplication::translate("PreviewRasterAnalysis", "无法读取预览栅格");
        running->reportStage("scan", 5);
        *result = histogram(&layer, bins);
        return QString();
      },
      {}, true);
  QObject::connect(receiver, &QObject::destroyed, task,
                   &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, receiver,
                   [task, result, ready, key, identity, uri] {
                     if (task->state() == PaleoTask::State::Succeeded &&
                         rasterKey(uri) == identity) {
                       cache.insert(key, new Histogram(*result));
                       ready(*result);
                     }
                   });
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
      auto *provider = layer->dataProvider();
      const double v = provider->sample( p, 1, &ok );
      // ok 只承诺「越界/band 非法」（qgsrasterdataprovider.h sample 文档）——
      // nodata 哨兵仍按真实值返回，需按源 nodata 自行屏蔽（与
      // composeattribution/mappingartifactwriter 的 isNoData 口径一致）。
      const bool noData = ok && provider->sourceHasNoDataValue( 1 ) &&
                          v == provider->sourceNoDataValue( 1 );
      s.valid = ok && !noData && std::isfinite( v );
      s.value = s.valid ? v : 0.0;
    }
    out.append( s );
  }
  return out;
}

namespace {
// 邻域统计：中心 (x,y) 的 window 内有效邻居均值与严格极值判定。
struct Neighbourhood
{
  double mean = 0.0;
  int n = 0;
  bool isMax = true;
  bool isMin = true;
};
Neighbourhood neighbourhood(const ExtremaGrid &g, int x, int y, int half,
                            double v, bool needExtremum)
{
  Neighbourhood nb;
  for (int dy = -half; dy <= half; ++dy)
    for (int dx = -half; dx <= half; ++dx)
    {
      if (dx == 0 && dy == 0)
        continue;
      if (!g.ok(x + dx, y + dy))
        continue;
      const double nv = g.at(x + dx, y + dy);
      nb.mean += nv;
      ++nb.n;
      if (needExtremum)
      {
        if (nv >= v)
          nb.isMax = false;
        if (nv <= v)
          nb.isMin = false;
      }
    }
  if (nb.n > 0)
    nb.mean /= nb.n;
  return nb;
}

double medianInPlace(std::vector<double> &v)
{
  const std::size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(mid), v.end());
  double m = v[mid];
  if (v.size() % 2 == 0)
  {
    const double lo = *std::max_element(v.begin(), v.begin() + std::ptrdiff_t(mid));
    m = 0.5 * (m + lo);
  }
  return m;
}

ExtremaGrid readGrid(QgsRasterLayer *layer, int band)
{
  ExtremaGrid g;
  if (!layer || !layer->isValid() || !layer->dataProvider())
    return g;
  const int w = layer->width();
  const int h = layer->height();
  if (w <= 0 || h <= 0)
    return g;
  // 读整块（预览栅格是小网格；大图走 summarize 的缓存口径不受此影响）。
  std::unique_ptr<QgsRasterBlock> block(
      layer->dataProvider()->block(band, layer->extent(), w, h));
  if (!block)
    return g;
  g.width = w;
  g.height = h;
  g.values.assign(std::size_t(w) * h, 0.0);
  g.valid.assign(std::size_t(w) * h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      // nodata 哨兵不是真实数据：块读取必须先 isNoData 再 value（#589 C.14）。
      if (block->isNoData(y, x))
        continue;
      const double v = block->value(y, x);
      if (!std::isfinite(v))
        continue;
      g.values[std::size_t(y) * w + x] = v;
      g.valid[std::size_t(y) * w + x] = 1;
    }
  return g;
}

QVector<Extremum> toMapExtrema(QgsRasterLayer *layer, const ExtremaGrid &g,
                               const std::vector<GridExtremum> &cells)
{
  QVector<Extremum> out;
  const QgsRectangle ext = layer->extent();
  const double muppX = ext.width() / g.width;
  const double muppY = ext.height() / g.height;
  for (const auto &c : cells)
  {
    Extremum e;
    e.pos = QgsPointXY(ext.xMinimum() + (c.x + 0.5) * muppX,
                       ext.yMaximum() - (c.y + 0.5) * muppY);
    e.value = c.value;
    e.peak = c.peak;
    out.append(e);
  }
  return out;
}
} // namespace

double robustResidualScale(const ExtremaGrid &g, int window,
                           const std::function<bool()> &cancelled)
{
  if (window < 3 || g.width <= 0 || g.height <= 0)
    return 0.0;
  const int half = window / 2;
  std::vector<double> residuals;
  for (int y = half; y < g.height - half; ++y)
  {
    if (cancelled && cancelled())
      return 0.0;
    for (int x = half; x < g.width - half; ++x)
    {
      if (!g.ok(x, y))
        continue;
      const double v = g.at(x, y);
      const Neighbourhood nb = neighbourhood(g, x, y, half, v, false);
      if (nb.n == 0)
        continue;
      residuals.push_back(v - nb.mean);
    }
  }
  if (residuals.empty())
    return 0.0;
  std::vector<double> work = residuals;
  const double med = medianInPlace(work);
  for (std::size_t i = 0; i < residuals.size(); ++i)
    work[i] = std::abs(residuals[i] - med);
  double meanAbs = 0.0;
  for (double d : work)
    meanAbs += d;
  meanAbs /= double(work.size());
  const double mad = medianInPlace(work);
  if (mad > 0.0)
    return 1.4826 * mad;
  return 1.2533 * meanAbs; // MAD=0 回落；完全平坦时为 0
}

std::vector<GridExtremum> gridExtrema(const ExtremaGrid &g, int window,
                                      double minProminence, int maxCount,
                                      const std::function<bool()> &cancelled)
{
  std::vector<GridExtremum> cands;
  if (window < 3 || g.width <= 0 || g.height <= 0)
    return cands;
  const int half = window / 2;
  for (int y = half; y < g.height - half; ++y)
  {
    if (cancelled && cancelled())
      return {};
    for (int x = half; x < g.width - half; ++x)
    {
      if (!g.ok(x, y))
        continue;
      const double v = g.at(x, y);
      const Neighbourhood nb = neighbourhood(g, x, y, half, v, true);
      if (nb.n == 0 || (!nb.isMax && !nb.isMin))
        continue;
      const double prom = std::abs(v - nb.mean);
      if (prom < minProminence)
        continue;
      cands.push_back({x, y, v, prom, nb.isMax});
    }
  }
  // 按显著性降序取前 maxCount；扫描序天然按 (y,x) 升序，stable_sort 保证并列确定。
  std::stable_sort(cands.begin(), cands.end(),
                   [](const GridExtremum &a, const GridExtremum &b) {
                     return a.prominence > b.prominence;
                   });
  if (maxCount >= 0 && cands.size() > std::size_t(maxCount))
    cands.resize(std::size_t(maxCount));
  return cands;
}

std::vector<GridExtremum> robustGridExtrema(const ExtremaGrid &g, int window,
                                            double k, int maxCount,
                                            const std::function<bool()> &cancelled)
{
  const double scale = robustResidualScale(g, window, cancelled);
  if (cancelled && cancelled())
    return {};
  return gridExtrema(g, window, k * scale, maxCount, cancelled);
}

QVector<Extremum> localExtrema(QgsRasterLayer *layer, int band, int window,
                               double prominence, int maxCount,
                               const std::function<bool()> &cancelled) {
  if (window < 3)
    return {};
  const ExtremaGrid g = readGrid(layer, band);
  if (g.width <= 0)
    return {};
  return toMapExtrema(layer, g, gridExtrema(g, window, prominence, maxCount, cancelled));
}

QVector<Extremum> robustLocalExtrema(QgsRasterLayer *layer, int band, int window,
                                     double k, int maxCount,
                                     const std::function<bool()> &cancelled) {
  if (window < 3)
    return {};
  const ExtremaGrid g = readGrid(layer, band);
  if (g.width <= 0)
    return {};
  return toMapExtrema(layer, g, robustGridExtrema(g, window, k, maxCount, cancelled));
}

void requestExtrema(const QString &uri, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const QVector<Extremum> &)> ready) {
  if (!tasks) {
    QgsRasterLayer layer(uri, "extrema", "gdal");
    // 显著性阈值 = k·局部残差稳健尺度（见头文件 robustResidualScale）；
    // 旧口径「全局 σ×0.5」把平滑层位面上的真实极值全部滤掉。
    ready(robustLocalExtrema(&layer, 1, 5, kRobustProminenceK, 100));
    return;
  }
  auto result = std::make_shared<QVector<Extremum>>();
  auto *task = tasks->start(
      QObject::tr("计算地层栅格极值"), [uri, result](PaleoTask *running) {
        QgsRasterLayer layer(uri, "extrema", "gdal");
        if (!layer.isValid())
          return QCoreApplication::translate("PreviewRasterAnalysis", "无法读取地层栅格");
        running->reportStage("scan", 5);
        *result = robustLocalExtrema(&layer, 1, 5, kRobustProminenceK, 100,
                                     [running] { return running->cancelRequested(); });
        return QString();
      });
  QObject::connect(receiver, &QObject::destroyed, task,
                   &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, receiver, [task, result, ready] {
    if (task->state() == PaleoTask::State::Succeeded)
      ready(*result);
  });
}
void requestProfile(const QString &uri, const QgsPointXY &p1,
                    const QgsPointXY &p2, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const QVector<ProfileSample> &)> ready) {
  if (!tasks) {
    QgsRasterLayer layer(uri, "profile", "gdal");
    ready(sampleProfile(&layer, p1, p2, 200));
    return;
  }
  auto result = std::make_shared<QVector<ProfileSample>>();
  auto *task = tasks->start(
      QObject::tr("采样栅格剖面"),
      [uri, p1, p2, result](PaleoTask *running) {
        QgsRasterLayer layer(uri, "profile", "gdal");
        if (!layer.isValid())
          return QCoreApplication::translate("PreviewRasterAnalysis", "无法读取剖面栅格");
        running->reportStage("scan", 5);
        *result = sampleProfile(&layer, p1, p2, 200);
        return QString();
      },
      {}, true);
  QObject::connect(receiver, &QObject::destroyed, task,
                   &PaleoTask::requestCancel);
  QObject::connect(task, &PaleoTask::finished, receiver, [task, result, ready] {
    if (task->state() == PaleoTask::State::Succeeded)
      ready(*result);
  });
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
  return QCoreApplication::translate("PreviewRasterAnalysis", "大图（%1 MB）且无金字塔——首次全图渲染较慢，可继续交互；"
                         "建议对该栅格构建金字塔加速" )
      .arg( fileSizeBytes( path ) / ( 1024.0 * 1024.0 ), 0, 'f', 0 );
}

} // namespace PreviewRasterAnalysis
