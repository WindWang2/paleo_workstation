// 层：数据
#include "compare.h"

#include "../singlefactor/geosutil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace paleo::evolution
{

namespace
{

// geosutil 薄封装住在 singlefactor 命名空间——这里按名引入（只取用到者）。
using paleo::singlefactor::GeomPtr;
using paleo::singlefactor::GeosContext;
using paleo::singlefactor::collectLineParts;
using paleo::singlefactor::makeCoordSeq;
using paleo::singlefactor::makeGeom;
using paleo::singlefactor::makePoint;
using paleo::singlefactor::makePolygon;
using paleo::singlefactor::unaryUnionOf;

// GEOSNearestPoints 返回的坐标序列 RAII（GeomPtr 是几何模板，序列要单列）。
struct CoordSeqDeleter
{
  GEOSContextHandle_t handle = nullptr;
  void operator()( GEOSCoordSequence *seq ) const
  {
    if ( seq )
      GEOSCoordSeq_destroy_r( handle, seq );
  }
};
using CoordSeqPtr = std::unique_ptr<GEOSCoordSequence, CoordSeqDeleter>;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// 相对容差：同格网判定用（浮点往返误差量级），非数值近似余地。
bool nearlyEqual( double a, double b )
{
  return std::fabs( a - b ) <= 1e-9 * std::max( { 1.0, std::fabs( a ), std::fabs( b ) } );
}

std::string gridLabel( const GridSpec &g )
{
  return std::to_string( g.cols ) + "×" + std::to_string( g.rows ) + " 像元 " +
         std::to_string( g.pixelWidth ) + "×" + std::to_string( g.pixelHeight ) +
         " @(" + std::to_string( g.originX ) + "," + std::to_string( g.originY ) + ")";
}

bool gridUsable( const GridSpec &g )
{
  return g.cols > 0 && g.rows > 0 && g.pixelWidth != 0 && g.pixelHeight != 0;
}

// 每相面片 → GEOS 并集，按 faciesCode 分组。返回码表（升序在调用侧收口）。
std::map<int, GeomPtr> unionsByCode( GEOSContextHandle_t handle, const FaciesCoverage &coverage )
{
  std::map<int, std::vector<const GEOSGeometry *>> parts;
  std::vector<GeomPtr> owned; // 保持面片几何存活到 union 完成
  owned.reserve( coverage.patches.size() );
  for ( const FaciesPatch &patch : coverage.patches )
  {
    if ( patch.geometry.exterior.points.size() < 4 )
      continue;
    GeomPtr geom = makePolygon( handle, patch.geometry );
    if ( !geom )
      continue;
    parts[patch.faciesCode].push_back( geom.get() );
    owned.push_back( std::move( geom ) );
  }
  std::map<int, GeomPtr> unions;
  for ( auto &[code, geoms] : parts )
  {
    // 面片几何已是 GEOS 形态，直接走 unaryUnionOf（往返回 DTO 会重解析）。
    unions[code] = unaryUnionOf( handle, geoms );
  }
  return unions;
}

double geomArea( GEOSContextHandle_t handle, const GEOSGeometry *geometry )
{
  double area = 0;
  if ( geometry )
    GEOSArea_r( handle, geometry, &area );
  return area;
}

bool centroidOf( GEOSContextHandle_t handle, const GEOSGeometry *geometry, Point2 *out )
{
  if ( !geometry )
    return false;
  GeomPtr centroid = makeGeom( handle, GEOSGetCentroid_r( handle, geometry ) );
  if ( !centroid )
    return false;
  return GEOSGeomGetX_r( handle, centroid.get(), &out->x ) &&
         GEOSGeomGetY_r( handle, centroid.get(), &out->y );
}

double medianOf( std::vector<double> &values )
{
  if ( values.empty() )
    return kNaN;
  const std::size_t n = values.size();
  std::nth_element( values.begin(), values.begin() + n / 2, values.end() );
  const double mid = values[n / 2];
  if ( n % 2 == 1 )
    return mid;
  const double lower = *std::max_element( values.begin(), values.begin() + n / 2 );
  return 0.5 * ( lower + mid );
}

// 沿闭合环按弧长等间距采样（首点计入，末点=首点不重复计）。
void sampleRing( const std::vector<Point2> &ring, double spacing, std::vector<Point2> *out )
{
  if ( ring.size() < 2 || !( spacing > 0 ) )
    return;
  double carried = 0;
  for ( std::size_t i = 1; i < ring.size(); ++i )
  {
    const Point2 &a = ring[i - 1];
    const Point2 &b = ring[i];
    const double segLen = std::hypot( b.x - a.x, b.y - a.y );
    if ( !( segLen > 0 ) )
      continue;
    double along = spacing - carried;
    while ( along <= segLen )
    {
      const double t = along / segLen;
      out->push_back( Point2{ a.x + t * ( b.x - a.x ), a.y + t * ( b.y - a.y ) } );
      along += spacing;
    }
    carried = segLen - ( along - spacing );
  }
}

// 几何 → 边界环采样点（外环与内环都采——都是相带边界的一部分）。
std::vector<Point2> boundarySamples( GEOSContextHandle_t handle, const GEOSGeometry *unionGeom,
                                     double spacing )
{
  std::vector<Point2> samples;
  if ( !unionGeom )
    return samples;
  GeomPtr boundary = makeGeom( handle, GEOSBoundary_r( handle, unionGeom ) );
  if ( !boundary )
    return samples;
  std::vector<std::vector<Point2>> lines;
  collectLineParts( handle, boundary.get(), &lines );
  for ( const std::vector<Point2> &line : lines )
    sampleRing( line, spacing, &samples );
  return samples;
}

} // namespace

DomainCheck checkSameDomain( const FaciesCoverage &earlier, const FaciesCoverage &later )
{
  DomainCheck check;
  const auto refuse = [&check]( const std::string &reason ) {
    check.same = false;
    check.reason = reason;
    return check;
  };

  if ( !gridUsable( earlier.grid ) )
    return refuse( "早期相覆盖的格网口径不可用（" + gridLabel( earlier.grid ) + "）" );
  if ( !gridUsable( later.grid ) )
    return refuse( "晚期相覆盖的格网口径不可用（" + gridLabel( later.grid ) + "）" );

  if ( earlier.grid.crs != later.grid.crs )
  {
    return refuse( "坐标参考系不同，跨期对比只在与格网同口径下进行：早期 [" +
                   earlier.grid.crs + "] vs 晚期 [" + later.grid.crs + "]" );
  }
  if ( earlier.grid.cols != later.grid.cols || earlier.grid.rows != later.grid.rows )
  {
    return refuse( "格网尺寸不同：早期 " + gridLabel( earlier.grid ) + " vs 晚期 " +
                   gridLabel( later.grid ) );
  }
  if ( !nearlyEqual( earlier.grid.pixelWidth, later.grid.pixelWidth ) ||
       !nearlyEqual( earlier.grid.pixelHeight, later.grid.pixelHeight ) )
  {
    return refuse( "像元尺寸不同：早期 " + gridLabel( earlier.grid ) + " vs 晚期 " +
                   gridLabel( later.grid ) );
  }
  if ( !nearlyEqual( earlier.grid.originX, later.grid.originX ) ||
       !nearlyEqual( earlier.grid.originY, later.grid.originY ) )
  {
    return refuse( "格网原点（坐标域）不同：早期 " + gridLabel( earlier.grid ) + " vs 晚期 " +
                   gridLabel( later.grid ) );
  }
  return check;
}

EvolutionResult compareFacies( const FaciesCoverage &earlier, const FaciesCoverage &later,
                               const CompareOptions &options, const Control &control )
{
  EvolutionResult result;
  result.earlierHorizon = earlier.horizon;
  result.laterHorizon = later.horizon;
  result.methodNote =
      "几何近似口径：面积/重叠为 GEOS 多边形求交（非像素计数）；边界进退 = 早期边界"
      "等距采样点至对期同相边界的最近点，进/退以采样点是否落入对期同相多边形内判定；"
      "图幅边框（两期共有的格网外缘）不是相带前缘，其采样不计入位移统计；"
      "位移方向为采样单位矢圆均值（合长度示方向一致性）；未做井控密度加权。";

  const auto cancelled = [&control]() {
    return control.cancelled && control.cancelled();
  };
  const auto report = [&control]( double progress ) {
    if ( control.progress )
      control.progress( progress );
  };

  if ( earlier.patches.empty() || later.patches.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = earlier.patches.empty() ? "早期相覆盖没有相面片，无法对比"
                                             : "晚期相覆盖没有相面片，无法对比";
    return result;
  }

  const DomainCheck domain = checkSameDomain( earlier, later );
  if ( !domain.same )
  {
    result.status = Status::InvalidInput;
    result.message = "拒算（不同源不近似）：" + domain.reason;
    return result;
  }

  GeosContext geos;
  GEOSContextHandle_t handle = geos.handle;
  if ( !handle )
  {
    result.status = Status::NumericalFailure;
    result.message = "GEOS 上下文初始化失败";
    return result;
  }

  const std::map<int, GeomPtr> earlierUnions = unionsByCode( handle, earlier );
  const std::map<int, GeomPtr> laterUnions = unionsByCode( handle, later );
  if ( earlierUnions.empty() || laterUnions.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "相面片几何无效（外环顶点不足），无法构建相并集";
    return result;
  }

  // 码表 = 两期并集，升序（map 已按码排序）。
  std::set<int> codes;
  for ( const auto &entry : earlierUnions )
    codes.insert( entry.first );
  for ( const auto &entry : laterUnions )
    codes.insert( entry.first );
  result.faciesCodes.assign( codes.begin(), codes.end() );

  // ---- 面积 + 重叠矩阵 --------------------------------------------------
  double totalEarlier = 0;
  double totalLater = 0;
  std::map<std::pair<int, int>, double> overlap;
  int overlapSteps = 0;
  const int totalOverlapSteps = static_cast<int>( earlierUnions.size() * laterUnions.size() );
  for ( const auto &[earlierCode, earlierGeom] : earlierUnions )
  {
    const double areaEarlier = geomArea( handle, earlierGeom.get() );
    totalEarlier += areaEarlier;
    for ( const auto &[laterCode, laterGeom] : laterUnions )
    {
      if ( cancelled() )
      {
        result.status = Status::Cancelled;
        result.message = "已取消";
        return result;
      }
      GeomPtr inter = makeGeom(
          handle, GEOSIntersection_r( handle, earlierGeom.get(), laterGeom.get() ) );
      const double area = geomArea( handle, inter.get() );
      if ( area > 0 )
        overlap[{ earlierCode, laterCode }] = area;
      report( 0.3 + 0.3 * ( ++overlapSteps ) / std::max( 1, totalOverlapSteps ) );
    }
  }
  for ( const auto &[laterCode, laterGeom] : laterUnions )
    totalLater += geomArea( handle, laterGeom.get() );

  if ( !( totalEarlier > 0 ) || !( totalLater > 0 ) )
  {
    result.status = Status::InvalidInput;
    result.message = "相覆盖总面积为零，无法对比";
    return result;
  }

  double unchanged = 0;
  for ( const auto &[pair, area] : overlap )
  {
    result.overlap.push_back( OverlapCell{ pair.first, pair.second, area } );
    if ( pair.first == pair.second )
      unchanged += area;
  }
  result.totalAreaEarlier = totalEarlier;
  result.totalAreaLater = totalLater;
  result.unchangedArea = unchanged;
  result.faciesTurnoverRatio = 1.0 - unchanged / totalEarlier;

  // ---- 逐相指标 ----------------------------------------------------------
  double spacing = options.boundarySampleSpacing;
  if ( !( spacing > 0 ) )
    spacing = std::max( std::fabs( earlier.grid.pixelWidth ), std::fabs( earlier.grid.pixelHeight ) );

  const auto findUnion = []( const std::map<int, GeomPtr> &unions, int code ) -> const GEOSGeometry * {
    const auto it = unions.find( code );
    return it == unions.end() ? nullptr : it->second.get();
  };

  int codeSteps = 0;
  // 图幅外缘矩形（两期共有的格网框）：其上的采样不是相带前缘，剔除。
  const double frameMinX = earlier.grid.originX;
  const double frameMaxX = earlier.grid.originX + earlier.grid.cols * earlier.grid.pixelWidth;
  const double frameMaxY = earlier.grid.originY;
  const double frameMinY = earlier.grid.originY + earlier.grid.rows * earlier.grid.pixelHeight;
  const double frameEps = 1e-6 * std::max( std::fabs( earlier.grid.pixelWidth ),
                                           std::fabs( earlier.grid.pixelHeight ) );
  const auto onFrame = [&]( const Point2 &p ) {
    return ( std::fabs( p.x - frameMinX ) <= frameEps || std::fabs( p.x - frameMaxX ) <= frameEps ) &&
           p.y >= frameMinY - frameEps && p.y <= frameMaxY + frameEps ||
           ( std::fabs( p.y - frameMinY ) <= frameEps || std::fabs( p.y - frameMaxY ) <= frameEps ) &&
               p.x >= frameMinX - frameEps && p.x <= frameMaxX + frameEps;
  };
  for ( const int code : result.faciesCodes )
  {
    if ( cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      return result;
    }
    FaciesChange change;
    change.faciesCode = code;
    const GEOSGeometry *geomEarlier = findUnion( earlierUnions, code );
    const GEOSGeometry *geomLater = findUnion( laterUnions, code );
    change.areaEarlier = geomEarlier ? geomArea( handle, geomEarlier ) : 0;
    change.areaLater = geomLater ? geomArea( handle, geomLater ) : 0;
    change.areaChange = change.areaLater - change.areaEarlier;
    change.areaChangeRatio =
        change.areaEarlier > 0 ? change.areaChange / change.areaEarlier : kNaN;

    const bool centroidEarlierOk = geomEarlier && centroidOf( handle, geomEarlier, &change.centroidEarlier );
    const bool centroidLaterOk = geomLater && centroidOf( handle, geomLater, &change.centroidLater );
    change.centroidsValid = centroidEarlierOk && centroidLaterOk;
    change.centroidDisplacement = change.centroidsValid
                                      ? std::hypot( change.centroidLater.x - change.centroidEarlier.x,
                                                    change.centroidLater.y - change.centroidEarlier.y )
                                      : kNaN;

    if ( geomEarlier && geomLater )
    {
      std::vector<Point2> samples = boundarySamples( handle, geomEarlier, spacing );
      if ( static_cast<int>( samples.size() ) > options.maxBoundarySamples &&
           options.maxBoundarySamples > 0 )
      {
        // 均匀抽稀：保首尾、等步长跳采，不聚簇。
        std::vector<Point2> thinned;
        thinned.reserve( options.maxBoundarySamples );
        const double step = static_cast<double>( samples.size() ) / options.maxBoundarySamples;
        for ( int i = 0; i < options.maxBoundarySamples; ++i )
          thinned.push_back( samples[static_cast<std::size_t>( i * step )] );
        samples = std::move( thinned );
      }
      // 晚期同相边界的「前缘」目标：剔除贴图幅框的段（框边不是两期间的
      // 相带前缘，留着会把边缘采样吸到图幅框上），余段聚合为最近点目标。
      GeomPtr laterFront;
      {
        GeomPtr boundary = makeGeom( handle, GEOSBoundary_r( handle, geomLater ) );
        std::vector<std::vector<Point2>> lines;
        collectLineParts( handle, boundary.get(), &lines );
        std::vector<GEOSGeometry *> segments;
        for ( const std::vector<Point2> &line : lines )
        {
          for ( std::size_t i = 1; i < line.size(); ++i )
          {
            const Point2 mid{ 0.5 * ( line[i - 1].x + line[i].x ),
                              0.5 * ( line[i - 1].y + line[i].y ) };
            if ( onFrame( mid ) )
              continue;
            GEOSGeometry *segment = GEOSGeom_createLineString_r(
              handle, makeCoordSeq( handle, { line[i - 1], line[i] } ) );
            if ( segment )
              segments.push_back( segment );
          }
        }
        if ( !segments.empty() )
        {
          GEOSGeometry *collection =
            GEOSGeom_createCollection_r( handle, GEOS_MULTILINESTRING, segments.data(),
                                         static_cast<unsigned int>( segments.size() ) );
          if ( collection )
            laterFront = makeGeom( handle, collection );
          else
            for ( GEOSGeometry *segment : segments ) // 聚合失败时手动回收，防漏
              GEOSGeom_destroy_r( handle, segment );
        }
      }
      std::vector<double> magnitudes;
      double sumDx = 0;
      double sumDy = 0;
      int advances = 0;
      for ( const Point2 &sample : samples )
      {
        if ( onFrame( sample ) )
          continue;
        GeomPtr samplePoint = makePoint( handle, sample.x, sample.y );
        if ( !samplePoint || !laterFront )
          continue;
        CoordSeqPtr nearestSeq( GEOSNearestPoints_r( handle, samplePoint.get(), laterFront.get() ),
                                CoordSeqDeleter{ handle } );
        if ( !nearestSeq )
          continue;
        Point2 target{};
        unsigned int size = 0;
        if ( !GEOSCoordSeq_getSize_r( handle, nearestSeq.get(), &size ) || size < 2 )
          continue;
        // 序列 [0]=sample 自身，[1]=最近点。
        if ( !GEOSCoordSeq_getXY_r( handle, nearestSeq.get(), 1, &target.x, &target.y ) )
          continue;
        const double dx = target.x - sample.x;
        const double dy = target.y - sample.y;
        const double magnitude = std::hypot( dx, dy );
        if ( !std::isfinite( magnitude ) )
          continue;
        magnitudes.push_back( magnitude );
        sumDx += magnitude > 0 ? dx / magnitude : 0;
        sumDy += magnitude > 0 ? dy / magnitude : 0;
        const char inside = GEOSIntersects_r( handle, samplePoint.get(), geomLater );
        const bool advance = inside == 1; // GEOS 谓词：1=true 0=false 2=异常
        if ( advance )
          ++advances;
        result.boundaryField.push_back( BoundaryVector{ sample, target, advance, code } );
      }
      change.boundarySamples = static_cast<int>( magnitudes.size() );
      change.boundaryMedianShift = medianOf( magnitudes );
      change.boundaryAdvanceRatio =
          magnitudes.empty() ? kNaN : static_cast<double>( advances ) / magnitudes.size();
      if ( !magnitudes.empty() )
      {
        double azimuth = std::atan2( sumDx, sumDy ) * 180.0 / M_PI;
        if ( azimuth < 0 )
          azimuth += 360.0;
        change.boundaryMedianAzimuthDeg = azimuth;
        change.boundaryResultant = std::hypot( sumDx, sumDy ) / magnitudes.size();
      }
      else
      {
        change.boundaryMedianAzimuthDeg = kNaN;
        change.boundaryResultant = kNaN;
      }
    }
    result.changes.push_back( change );
    report( 0.6 + 0.4 * ( ++codeSteps ) / std::max<std::size_t>( 1, result.faciesCodes.size() ) );
  }

  result.status = Status::Ok;
  result.message = "ok";
  return result;
}

} // namespace paleo::evolution
