// 层：数据
#include "faciesqa.h"

#include "../singlefactor/geosutil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace paleo::faciesmapping
{

using singlefactor::GeosContext;
using singlefactor::GeomPtr;

namespace
{

Point2 pointOnSurface( const GeosContext &ctx, const GEOSGeometry *geometry )
{
  Point2 out{ 0, 0 };
  if ( !geometry )
    return out;
  GeomPtr point( singlefactor::makeGeom( ctx.handle,
                                         GEOSPointOnSurface_r( ctx.handle, geometry ) ) );
  if ( point )
  {
    GEOSGeomGetX_r( ctx.handle, point.get(), &out.x );
    GEOSGeomGetY_r( ctx.handle, point.get(), &out.y );
  }
  return out;
}

double geosArea( const GeosContext &ctx, const GEOSGeometry *geometry )
{
  double area = 0;
  if ( geometry )
    GEOSArea_r( ctx.handle, geometry, &area );
  return area;
}

} // namespace

FaciesQaResult runFaciesQa( const std::vector<FaciesMapUnit> &units,
                            const std::vector<QaWellPoint> &wells,
                            const std::vector<QaConstraintLine> &constraints,
                            const FaciesQaOptions &options,
                            const Control *control )
{
  using singlefactor::makeLineString;
  using singlefactor::makePoint;
  using singlefactor::makePolygon;

  auto cancelled = [control]() {
    return control && control->cancelled && control->cancelled();
  };

  FaciesQaResult result;
  if ( units.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "no map units supplied";
    return result;
  }

  GeosContext ctx;

  // 单元 GEOS 几何：未闭环先显式补闭合（重叠/孤岛按隐式闭合语义检出；
  // 不闭合本身由 UnclosedRing 单独报告——两个检测器职责分离）。
  auto closedCopy = []( const singlefactor::Ring &ring ) {
    singlefactor::Ring closed = ring;
    if ( closed.points.size() >= 3 &&
         ( closed.points.front().x != closed.points.back().x ||
           closed.points.front().y != closed.points.back().y ) )
      closed.points.push_back( closed.points.front() );
    return closed;
  };

  std::vector<GeomPtr> unitGeoms;
  std::vector<GeomPtr> unitBoundaries;
  std::vector<double> unitAreas;
  unitGeoms.reserve( units.size() );
  unitBoundaries.reserve( units.size() );
  unitAreas.reserve( units.size() );
  for ( const FaciesMapUnit &unit : units )
  {
    GeomPtr geom;
    if ( unit.geometry.exterior.points.size() >= 3 )
    {
      singlefactor::Polygon closed;
      closed.exterior = closedCopy( unit.geometry.exterior );
      closed.holes.reserve( unit.geometry.holes.size() );
      for ( const singlefactor::Ring &hole : unit.geometry.holes )
        closed.holes.push_back( closedCopy( hole ) );
      geom = makePolygon( ctx.handle, closed );
    }
    unitGeoms.push_back( std::move( geom ) ); // 退化面：空指针，各检测器跳过
    unitBoundaries.push_back(
        unitGeoms.back()
            ? singlefactor::makeGeom( ctx.handle,
                                      GEOSBoundary_r( ctx.handle,
                                                      unitGeoms.back().get() ) )
            : GeomPtr() );
    unitAreas.push_back( unit.area > 0 ? unit.area
                                       : ( unitGeoms.back() ? geosArea( ctx,
                                                                        unitGeoms.back()
                                                                            .get() )
                                                            : 0.0 ) );
  }

  std::vector<GeomPtr> wellGeoms;
  wellGeoms.reserve( wells.size() );
  for ( const QaWellPoint &well : wells )
    wellGeoms.push_back( makePoint( ctx.handle, well.x, well.y ) );

  std::vector<GeomPtr> constraintGeoms;
  constraintGeoms.reserve( constraints.size() );
  for ( const QaConstraintLine &constraint : constraints )
  {
    if ( constraint.points.size() < 2 )
      continue;
    constraintGeoms.push_back( makeLineString( ctx.handle, constraint.points ) );
  }

  // ---- 检测器 1：环闭合 --------------------------------------------------
  for ( std::size_t i = 0; i < units.size(); ++i )
  {
    if ( cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.issues.clear();
      return result;
    }
    const FaciesMapUnit &unit = units[i];
    std::vector<const singlefactor::Ring *> rings{ &unit.geometry.exterior };
    for ( const singlefactor::Ring &hole : unit.geometry.holes )
      rings.push_back( &hole );
    for ( const singlefactor::Ring *ring : rings )
    {
      const auto &points = ring->points;
      if ( points.size() < 4 )
      {
        FaciesQaIssue issue;
        issue.type = FaciesQaIssueType::UnclosedRing;
        issue.regionIds = { unit.regionId };
        issue.metric = static_cast<double>( points.size() ); // 点数不足（<4）
        if ( !points.empty() )
          issue.location = points.front();
        result.issues.push_back( std::move( issue ) );
        continue;
      }
      const singlefactor::Point2 &first = points.front();
      const singlefactor::Point2 &last = points.back();
      const double gap = std::hypot( last.x - first.x, last.y - first.y );
      if ( gap > options.ringClosureTolerance )
      {
        FaciesQaIssue issue;
        issue.type = FaciesQaIssueType::UnclosedRing;
        issue.regionIds = { unit.regionId };
        issue.metric = gap;
        issue.location = singlefactor::Point2{ ( first.x + last.x ) / 2,
                                               ( first.y + last.y ) / 2 };
        result.issues.push_back( std::move( issue ) );
      }
    }
  }

  // ---- 检测器 2：单元两两重叠 --------------------------------------------
  for ( std::size_t i = 0; i + 1 < units.size(); ++i )
  {
    if ( cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.issues.clear();
      return result;
    }
    if ( !unitGeoms[i] )
      continue;
    for ( std::size_t j = i + 1; j < units.size(); ++j )
    {
      if ( !unitGeoms[j] )
        continue;
      GeomPtr intersection = singlefactor::makeGeom(
          ctx.handle, GEOSIntersection_r( ctx.handle, unitGeoms[i].get(),
                                          unitGeoms[j].get() ) );
      const double overlapArea = geosArea( ctx, intersection.get() );
      if ( overlapArea <= options.overlapTolerance )
        continue;
      FaciesQaIssue issue;
      issue.type = FaciesQaIssueType::Overlap;
      issue.regionIds = { units[i].regionId, units[j].regionId };
      issue.metric = overlapArea;
      issue.location = pointOnSurface( ctx, intersection.get() );
      result.issues.push_back( std::move( issue ) );
    }
  }

  // ---- 检测器 3：孤岛小面（阈值可配，<=0 关闭） --------------------------
  if ( options.minIslandArea > 0 )
  {
    for ( std::size_t i = 0; i < units.size(); ++i )
    {
      if ( cancelled() )
      {
        result.status = Status::Cancelled;
        result.message = "cancelled";
        result.issues.clear();
        return result;
      }
      if ( !unitGeoms[i] || unitAreas[i] >= options.minIslandArea )
        continue;
      FaciesQaIssue issue;
      issue.type = FaciesQaIssueType::SmallIsland;
      issue.regionIds = { units[i].regionId };
      issue.metric = unitAreas[i];
      issue.location = pointOnSurface( ctx, unitGeoms[i].get() );
      result.issues.push_back( std::move( issue ) );
    }
  }

  // ---- 检测器 4：单元边界穿越硬约束线 ------------------------------------
  for ( std::size_t i = 0; i < units.size(); ++i )
  {
    if ( cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.issues.clear();
      return result;
    }
    if ( !unitBoundaries[i] )
      continue;
    for ( std::size_t c = 0; c < constraints.size(); ++c )
    {
      if ( c >= constraintGeoms.size() || !constraintGeoms[c] )
        continue;
      if ( GEOSCrosses_r( ctx.handle, unitBoundaries[i].get(),
                          constraintGeoms[c].get() ) != 1 )
        continue;
      GeomPtr crossing = singlefactor::makeGeom(
          ctx.handle, GEOSIntersection_r( ctx.handle, unitBoundaries[i].get(),
                                          constraintGeoms[c].get() ) );
      FaciesQaIssue issue;
      issue.type = FaciesQaIssueType::ConstraintConflict;
      issue.regionIds = { units[i].regionId };
      issue.relatedIds = { constraints[c].id };
      // 度量 = 交叉点数（线×面边界的交是点集；空则记 1——Crosses 已判真）。
      issue.metric = 1;
      if ( crossing && GEOSisEmpty_r( ctx.handle, crossing.get() ) != 1 )
      {
        issue.metric = std::max< double >(
            1, static_cast<double>( GEOSGetNumGeometries_r( ctx.handle,
                                                            crossing.get() ) ) );
        issue.location = pointOnSurface( ctx, crossing.get() );
      }
      result.issues.push_back( std::move( issue ) );
    }
  }

  // ---- 检测器 5：缺井覆盖 -------------------------------------------------
  for ( std::size_t i = 0; i < units.size(); ++i )
  {
    if ( cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.issues.clear();
      return result;
    }
    if ( !unitGeoms[i] )
      continue;
    bool covered = false;
    double nearest = std::numeric_limits<double>::infinity();
    for ( const GeomPtr &well : wellGeoms )
    {
      if ( GEOSIntersects_r( ctx.handle, unitGeoms[i].get(), well.get() ) == 1 )
      {
        covered = true;
        break;
      }
      nearest = std::min( nearest,
                          singlefactor::geosDistance( ctx.handle, unitGeoms[i].get(),
                                                      well.get() ) );
    }
    if ( covered )
      continue;
    if ( options.wellCoverageRadius > 0 && nearest <= options.wellCoverageRadius )
      continue; // 缓冲半径内算覆盖
    FaciesQaIssue issue;
    issue.type = FaciesQaIssueType::NoWellCoverage;
    issue.regionIds = { units[i].regionId };
    issue.metric = wellGeoms.empty() ? -1.0 : nearest; // 无井系统 → -1（如实）
    issue.location = pointOnSurface( ctx, unitGeoms[i].get() );
    result.issues.push_back( std::move( issue ) );
  }

  // ---- 诊断汇总（各类型计数 + 开关状态，报告可复现） ----------------------
  std::map<std::string, int> counts; // 无 Qt 依赖的临时计数
  for ( const FaciesQaIssue &issue : result.issues )
    counts[faciesQaIssueName( issue.type )]++;
  for ( const auto &[name, count] : counts )
    result.diagnostics.insert( QString::fromStdString( "issue_" + name ), count );
  result.diagnostics.insert( QStringLiteral( "unit_count" ),
                             static_cast<int>( units.size() ) );
  result.diagnostics.insert( QStringLiteral( "well_count" ),
                             static_cast<int>( wells.size() ) );
  result.diagnostics.insert( QStringLiteral( "constraint_count" ),
                             static_cast<int>( constraints.size() ) );
  result.diagnostics.insert( QStringLiteral( "island_detector_on" ),
                             options.minIslandArea > 0 );
  result.diagnostics.insert( QStringLiteral( "well_coverage_radius" ),
                             options.wellCoverageRadius );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::faciesmapping
