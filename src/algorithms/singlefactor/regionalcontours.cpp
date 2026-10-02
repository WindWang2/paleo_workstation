// 层：数据
#include "regionalcontours.h"

#include "localidw.h"
#include "support.h"

#include <algorithm>
#include <cmath>
#include <limits>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

struct Segment
{
  Point2 a;
  Point2 b;
};

// 网格 marching squares：区域内有限值格点提等值线段。北向上、像元中心采样。
void marchingSquaresSegments( const GridSpec &grid, const std::vector<double> &values,
                              const std::vector<std::uint8_t> &regionMask, double level,
                              std::vector<Segment> *out )
{
  const auto valueAt = [&]( int col, int row, double *value ) {
    if ( col < 0 || col >= grid.cols || row < 0 || row >= grid.rows )
      return false;
    const std::size_t idx = static_cast<std::size_t>( row ) * grid.cols + col;
    if ( !regionMask[idx] || !std::isfinite( values[idx] ) )
      return false;
    *value = values[idx];
    return true;
  };
  const auto interp = []( double v0, double v1, double lvl ) {
    const double d = v1 - v0;
    if ( std::abs( d ) <= 1e-15 )
      return 0.5;
    return ( lvl - v0 ) / d;
  };
  for ( int row = 0; row + 1 < grid.rows; ++row )
  {
    for ( int col = 0; col + 1 < grid.cols; ++col )
    {
      double tl = 0, tr = 0, br = 0, bl = 0;
      const bool hasTl = valueAt( col, row, &tl );
      const bool hasTr = valueAt( col + 1, row, &tr );
      const bool hasBr = valueAt( col + 1, row + 1, &br );
      const bool hasBl = valueAt( col, row + 1, &bl );
      if ( !hasTl || !hasTr || !hasBr || !hasBl )
        continue; // 分区边界与缺失像元处截断，不跨区连线
      const double x0 = grid.originX + ( col + 0.5 ) * grid.pixelWidth;
      const double x1 = grid.originX + ( col + 1.5 ) * grid.pixelWidth;
      const double yTop = grid.originY + ( row + 0.5 ) * grid.pixelHeight;
      const double yBot = grid.originY + ( row + 1.5 ) * grid.pixelHeight;
      struct EdgePoint
      {
        Point2 p;
        int code;
      };
      std::vector<EdgePoint> hits;
      // 值恰等于级别也算穿级（对称场不漏检）；整边同值不算。
      const auto crosses = [level]( double v0, double v1 ) {
        return v0 != v1 && ( v0 - level ) * ( v1 - level ) <= 0.0;
      };
      if ( crosses( tl, tr ) )
        hits.push_back( { Point2{ x0 + ( x1 - x0 ) * interp( tl, tr, level ), yTop }, 0 } );
      if ( crosses( tr, br ) )
        hits.push_back( { Point2{ x1, yTop + ( yBot - yTop ) * interp( tr, br, level ) }, 1 } );
      if ( crosses( br, bl ) )
        hits.push_back( { Point2{ x0 + ( x1 - x0 ) * interp( bl, br, level ), yBot }, 2 } );
      if ( crosses( bl, tl ) )
        hits.push_back( { Point2{ x0, yTop + ( yBot - yTop ) * interp( tl, bl, level ) }, 3 } );
      if ( hits.size() == 2 )
      {
        out->push_back( Segment{ hits[0].p, hits[1].p } );
      }
      else if ( hits.size() == 4 )
      {
        // 鞍点：按中心均值消歧，避免交叉连线。
        const double center = 0.25 * ( tl + tr + br + bl );
        if ( center >= level )
        {
          out->push_back( Segment{ hits[0].p, hits[1].p } );
          out->push_back( Segment{ hits[2].p, hits[3].p } );
        }
        else
        {
          out->push_back( Segment{ hits[0].p, hits[3].p } );
          out->push_back( Segment{ hits[1].p, hits[2].p } );
        }
      }
    }
  }
}

double squaredDist( Point2 a, Point2 b )
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return dx * dx + dy * dy;
}

// 线段缝合：端点就近串成折线，剩余孤立段自成一线。
void stitchSegments( const std::vector<Segment> &segments, double toleranceSq,
                     std::vector<std::vector<Point2>> *out )
{
  std::vector<std::vector<Point2>> chains;
  std::vector<unsigned char> used( segments.size(), 0 );
  for ( std::size_t i = 0; i < segments.size(); ++i )
  {
    if ( used[i] )
      continue;
    used[i] = 1;
    std::vector<Point2> chain{ segments[i].a, segments[i].b };
    bool extended = true;
    while ( extended )
    {
      extended = false;
      for ( std::size_t j = 0; j < segments.size(); ++j )
      {
        if ( used[j] )
          continue;
        if ( squaredDist( chain.back(), segments[j].a ) <= toleranceSq )
        {
          chain.push_back( segments[j].b );
          used[j] = 1;
          extended = true;
        }
        else if ( squaredDist( chain.back(), segments[j].b ) <= toleranceSq )
        {
          chain.push_back( segments[j].a );
          used[j] = 1;
          extended = true;
        }
      }
    }
    if ( chain.size() >= 2 )
      chains.push_back( std::move( chain ) );
  }
  for ( auto &chain : chains )
    out->push_back( std::move( chain ) );
}

} // namespace

RegionalContourResult regionalContours( const PreparedInput &input, const GridSpec &grid,
                                        const ResolvedParameters &parameters,
                                        const std::vector<double> &levels,
                                        const PartitionResult &partition,
                                        const Control *control )
{
  RegionalContourResult result;
  const std::size_t cells = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  if ( partition.regionIds.size() != cells )
  {
    result.status = Status::InvalidInput;
    result.message = "分区标签与网格尺寸不一致";
    return result;
  }
  if ( partition.wellRegionIds.size() != input.samples.size() )
  {
    result.status = Status::InvalidInput;
    result.message = "分区井归属与样本数不一致";
    return result;
  }
  if ( levels.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "等值线级别为空";
    return result;
  }
  for ( double level : levels )
  {
    if ( !std::isfinite( level ) )
    {
      result.status = Status::InvalidInput;
      result.message = "等值线级别必须为有限值";
      return result;
    }
  }

  const int regionCount = std::max( partition.regionCount, 0 );
  for ( int rid = 0; rid < regionCount; ++rid )
  {
    if ( control && control->cancelled && control->cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "已取消";
      result.contours.clear();
      result.regionOfContour.clear();
      return result;
    }
    std::vector<Sample> regionSamples;
    for ( std::size_t i = 0; i < input.samples.size(); ++i )
      if ( partition.wellRegionIds[i] == rid )
        regionSamples.push_back( input.samples[i] );
    if ( regionSamples.empty() )
    {
      result.status = Status::InvalidInput;
      result.message = "第 " + std::to_string( rid + 1 ) +
                       " 个提线分区没有井点，不能跨隔断借用其他区域数值。请补充井点或调整分区。";
      return result;
    }

    // 本区独立井控：子样本独立解析参数后逐格心点插值。
    PreparedInput regionInput = input;
    regionInput.samples = std::move( regionSamples );
    regionInput.originalCount = static_cast<int>( regionInput.samples.size() );
    regionInput.validCount = regionInput.originalCount;
    regionInput.constraints.clear();
    regionInput.ignored.clear();
    ResolvedParameters regionParams = parameters;
    const std::string resolveError = resolveParameters( regionInput, grid, &regionParams );
    if ( !resolveError.empty() )
    {
      result.status = Status::InvalidInput;
      result.message = resolveError;
      return result;
    }
    // 子区重解析只更新尺度类参数；调用方的显式插值选项不被井数更少的分区覆盖。
    regionParams.minPoints = parameters.minPoints;
    regionParams.maxPoints = parameters.maxPoints;
    regionParams.coverage = parameters.coverage;
    regionParams.searchRadius = parameters.searchRadius;

    std::vector<std::uint8_t> regionMask( cells, std::uint8_t{ 0 } );
    std::vector<Point2> queries;
    std::vector<std::size_t> queryCells;
    for ( std::size_t i = 0; i < cells; ++i )
    {
      if ( partition.regionIds[i] != rid )
        continue;
      regionMask[i] = 1;
      queries.push_back( cellCenter( grid, static_cast<int>( i % grid.cols ),
                                     static_cast<int>( i / grid.cols ) ) );
      queryCells.push_back( i );
    }
    if ( queries.empty() )
      continue;
    const QueryResult region = evaluateAt( regionInput, queries, regionParams,
                                           control ? *control : Control{} );
    if ( region.status != Status::Ok )
    {
      result.status = region.status;
      result.message = region.message;
      return result;
    }
    std::vector<double> regionValues( cells, std::numeric_limits<double>::quiet_NaN() );
    for ( std::size_t q = 0; q < queries.size(); ++q )
      regionValues[queryCells[q]] = region.values[q];

    for ( double level : levels )
    {
      std::vector<Segment> segments;
      marchingSquaresSegments( grid, regionValues, regionMask, level, &segments );
      const double toleranceSq = 1e-12 *
                                 std::max( std::abs( grid.pixelWidth ), std::abs( grid.pixelHeight ) );
      std::vector<std::vector<Point2>> lines;
      stitchSegments( segments, toleranceSq, &lines );
      for ( auto &line : lines )
      {
        ContourPolyline poly;
        poly.points = std::move( line );
        poly.level = level;
        result.contours.push_back( std::move( poly ) );
        result.regionOfContour.push_back( rid );
      }
    }
  }
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::singlefactor
