// 层：数据
#include "fieldcontours.h"

#include "cartographicsmooth.h"
#include "geosutil.h"
#include "structural.h"

#include <QVariantList>

#include <geos_c.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>

// 逐项移植 field_contours.py / contour_work_field.py（冻结 @27fdb99）。
// 只覆盖两条上游路径：geometry_policy == "field_only"（公共提取尾部）与
// "local_interpretive_detour"（先 field_only 取初始线，再对真正被穿过的
// 停线建工作场并重提取）。其余策略 Paleo 不写、本模块拒绝。

namespace paleo::singlefactor
{
namespace
{

constexpr double kEps64 = std::numeric_limits<double>::epsilon();

// ---- StoredTrendField（field_contours.py L21-63）----
struct TrendField
{
  std::vector<double> xs;
  std::vector<double> ys;
  std::vector<double> grid; // 行主序 ny*nx（行=升序 y），掩膜外 NaN
  GeomPtr domain;
  GeomPtr domainBoundary;
  std::vector<std::uint8_t> inside; // 节点 intersects(domain)
  double tolerance = 1e-9;
  std::size_t nx = 0, ny = 0;

  double valueAt( std::size_t r, std::size_t c ) const { return grid[r * nx + c]; }
};

// np.searchsorted(axis, x, 'right') - 1 → clip [0, n-2]
std::size_t cellIndex( const std::vector<double> &axis, double x )
{
  const std::size_t n = axis.size();
  const auto it = std::upper_bound( axis.begin(), axis.end(), x );
  std::ptrdiff_t c = static_cast<std::ptrdiff_t>( it - axis.begin() ) - 1;
  if ( c < 0 )
    c = 0;
  if ( c > static_cast<std::ptrdiff_t>( n ) - 2 )
    c = static_cast<std::ptrdiff_t>( n ) - 2;
  return static_cast<std::size_t>( c );
}

// StoredTrendField.evaluate：双线性 + 域内/缺角规则（向量化逐项等价）。
std::vector<double> evaluate( GEOSContextHandle_t handle, const TrendField &field,
                              const GEOSPreparedGeometry *prepared,
                              const std::vector<Point2> &points )
{
  std::vector<double> out( points.size() );
  for ( std::size_t i = 0; i < points.size(); ++i )
  {
    const double x = points[i].x;
    const double y = points[i].y;
    const std::size_t c = cellIndex( field.xs, x );
    const std::size_t r = cellIndex( field.ys, y );
    const double dx = field.xs[c + 1] - field.xs[c];
    const double dy = field.ys[r + 1] - field.ys[r];
    const double u = std::min( 1.0, std::max( 0.0, ( x - field.xs[c] ) / dx ) );
    const double v = std::min( 1.0, std::max( 0.0, ( y - field.ys[r] ) / dy ) );
    double weights[4] = { ( 1.0 - u ) * ( 1.0 - v ), u * ( 1.0 - v ), u * v,
                          ( 1.0 - u ) * v };
    const double values[4] = { field.valueAt( r, c ), field.valueAt( r, c + 1 ),
                               field.valueAt( r + 1, c + 1 ),
                               field.valueAt( r + 1, c ) };
    const bool cornerInside[4] = {
        field.inside[r * field.nx + c] != 0, field.inside[r * field.nx + c + 1] != 0,
        field.inside[( r + 1 ) * field.nx + c + 1] != 0,
        field.inside[( r + 1 ) * field.nx + c] != 0 };
    bool missing = false;
    double total = 0;
    double z = 0;
    for ( int k = 0; k < 4; ++k )
    {
      const bool finite = std::isfinite( values[k] );
      if ( cornerInside[k] && !finite && weights[k] > 1e-12 )
        missing = true;
      if ( !finite )
        weights[k] = 0;
      total += weights[k];
      z += weights[k] * ( finite ? values[k] : 0.0 );
    }
    z /= std::max( total, 1e-30 );
    const bool inDomain =
        prepared &&
        GEOSPreparedIntersectsXY_r( handle, prepared, x, y ) == 1;
    out[i] = ( missing || total <= 0 || !inDomain )
                 ? std::numeric_limits<double>::quiet_NaN()
                 : z;
  }
  return out;
}

// ---- np.unique(axis=0, return_inverse)：按 (x,y) 字典序 ----
struct UniquePoints
{
  std::vector<Point2> values;
  std::vector<int> inverse;
};

UniquePoints uniquePoints( const std::vector<Point2> &rows )
{
  std::vector<int> order( rows.size() );
  for ( std::size_t i = 0; i < rows.size(); ++i )
    order[i] = static_cast<int>( i );
  std::sort( order.begin(), order.end(), [&]( int a, int b ) {
    if ( rows[a].x != rows[b].x )
      return rows[a].x < rows[b].x;
    return rows[a].y < rows[b].y;
  } );
  UniquePoints out;
  out.inverse.assign( rows.size(), 0 );
  for ( int idx : order )
  {
    if ( out.values.empty() ||
         !( out.values.back().x == rows[idx].x && out.values.back().y == rows[idx].y ) )
      out.values.push_back( rows[idx] );
    out.inverse[static_cast<std::size_t>( idx )] =
        static_cast<int>( out.values.size() ) - 1;
  }
  return out;
}

// ---- ContourMesh（field_contours.py L66-165，field_only 分支）----
struct ContourMesh
{
  std::vector<std::array<Point2, 3>> xy;
  std::vector<std::array<double, 3>> values;
  double step = 0;
  std::vector<int> regionIds;
  std::vector<const GEOSGeometry *> regions; // domain 的多边形成员指针
};

GeomPtr makeCollectionOf( GEOSContextHandle_t handle,
                          const std::vector<const GEOSGeometry *> &items, int type )
{
  std::vector<GEOSGeometry *> owned;
  owned.reserve( items.size() );
  for ( const GEOSGeometry *item : items )
    if ( item )
      owned.push_back( GEOSGeom_clone_r( handle, item ) );
  return makeGeom( handle,
                   GEOSGeom_createCollection_r( handle, type, owned.data(),
                                                static_cast<unsigned int>( owned.size() ) ) );
}

// field_mesh：无墙 → 网格线 ∪ domain.boundary → polygonize → CDT。
// 返回 false 时 message 已写。
bool buildMesh( GEOSContextHandle_t handle, TrendField *field, ContourMesh *mesh,
                std::string *message )
{
  const std::vector<double> &xs = field->xs;
  const std::vector<double> &ys = field->ys;
  const double step = std::min( std::abs( xs[1] - xs[0] ), std::abs( ys[1] - ys[0] ) );
  const double eps = step * 1e-7;
  mesh->step = step;

  // field_only → original=[] → interpretation_domains 短路：
  // regions = _parts(domain, 'Polygon')，无 partition_walls、无 extension。
  collectPolygons( handle, field->domain.get(), &mesh->regions );

  // grid lines：先全部竖线（xs 序），再全部横线（ys 序）。
  std::vector<GeomPtr> gridLines;
  for ( const double x : xs )
    gridLines.push_back(
        makeLineString( handle, { Point2{ x, ys.front() }, Point2{ x, ys.back() } } ) );
  for ( const double y : ys )
    gridLines.push_back(
        makeLineString( handle, { Point2{ xs.front(), y }, Point2{ xs.back(), y } } ) );

  // 逐单元格加密：mixed 差分误差 / 容差 → divisions。
  const std::size_t nx = xs.size(), ny = ys.size();
  for ( std::size_t r = 0; r + 1 < ny; ++r )
  {
    for ( std::size_t c = 0; c + 1 < nx; ++c )
    {
      const double corners[4] = { field->valueAt( r, c ), field->valueAt( r, c + 1 ),
                                  field->valueAt( r + 1, c + 1 ),
                                  field->valueAt( r + 1, c ) };
      int finiteCount = 0;
      double high = -std::numeric_limits<double>::infinity();
      double low = std::numeric_limits<double>::infinity();
      for ( const double v : corners )
      {
        if ( std::isfinite( v ) )
        {
          ++finiteCount;
          high = std::max( high, v );
          low = std::min( low, v );
        }
      }
      double error = 0;
      if ( finiteCount == 4 )
        error = std::abs( corners[0] - corners[1] + corners[2] - corners[3] ) * 0.25;
      else if ( finiteCount > 0 )
        error = ( high - low ) * 0.25;
      const int divisions = static_cast<int>(
          std::min( 16.0,
                    std::max( 1.0, std::ceil( std::sqrt( error / field->tolerance ) ) ) ) );
      if ( divisions <= 1 )
        continue;
      for ( int k = 1; k < divisions; ++k )
      {
        const double x = xs[c] + ( xs[c + 1] - xs[c] ) * k / divisions;
        gridLines.push_back(
            makeLineString( handle, { Point2{ x, ys[r] }, Point2{ x, ys[r + 1] } } ) );
      }
      for ( int k = 1; k < divisions; ++k )
      {
        const double y = ys[r] + ( ys[r + 1] - ys[r] ) * k / divisions;
        gridLines.push_back(
            makeLineString( handle, { Point2{ xs[c], y }, Point2{ xs[c + 1], y } } ) );
      }
    }
  }

  std::vector<const GEOSGeometry *> wireItems;
  wireItems.reserve( gridLines.size() + 1 );
  for ( const GeomPtr &line : gridLines )
    wireItems.push_back( line.get() );
  wireItems.push_back( field->domainBoundary.get() );
  GeomPtr gridWire = unaryUnionOf( handle, wireItems );
  // walls/links 为空 → network = unary_union([grid_wire])
  GeomPtr network = unaryUnionOf( handle, { gridWire.get() } );

  // faces = [f for f in polygonize(network) if domain.covers(point_on_surface(f))]
  const std::vector<const GEOSGeometry *> obs = directParts( handle, network.get() );
  GeomPtr polygonized = polygonizeOf( handle, obs );
  if ( !polygonized )
  {
    *message = "GEOS polygonize 失败";
    return false;
  }
  std::vector<const GEOSGeometry *> faces;
  const GEOSPreparedGeometry *preparedDomain = GEOSPrepare_r( handle, field->domain.get() );
  if ( !preparedDomain )
  {
    *message = "GEOS 域索引失败";
    return false;
  }
  for ( const GEOSGeometry *face : directParts( handle, polygonized.get() ) )
  {
    if ( GEOSGeomTypeId_r( handle, face ) != GEOS_POLYGON )
      continue;
    GeomPtr probe = makeGeom( handle, GEOSPointOnSurface_r( handle, face ) );
    if ( probe && GEOSPreparedCovers_r( handle, preparedDomain, probe.get() ) == 1 )
      faces.push_back( face );
  }

  // triangles = get_parts(constrained_delaunay_triangles(faces))
  GeomPtr faceCollection = makeCollectionOf( handle, faces, GEOS_GEOMETRYCOLLECTION );
  GeomPtr triangles =
      makeGeom( handle,
                GEOSConstrainedDelaunayTriangulation_r( handle, faceCollection.get() ) );
  if ( !triangles )
  {
    *message = "GEOS 约束三角化失败";
    return false;
  }
  std::vector<Point2> flat;
  for ( const GEOSGeometry *tri : directParts( handle, triangles.get() ) )
  {
    if ( GEOSGeomTypeId_r( handle, tri ) != GEOS_POLYGON )
      continue;
    const GEOSGeometry *shell = GEOSGetExteriorRing_r( handle, tri );
    const std::vector<Point2> ring = lineCoords( handle, shell );
    if ( ring.size() < 3 )
      continue;
    mesh->xy.push_back( { ring[0], ring[1], ring[2] } );
    flat.push_back( ring[0] );
    flat.push_back( ring[1] );
    flat.push_back( ring[2] );
  }

  const UniquePoints unique = uniquePoints( flat );
  const std::vector<double> vertexValues =
      evaluate( handle, *field, preparedDomain, unique.values );

  mesh->values.resize( mesh->xy.size() );
  for ( std::size_t t = 0; t < mesh->xy.size(); ++t )
    for ( int k = 0; k < 3; ++k )
      mesh->values[t][k] = vertexValues[static_cast<std::size_t>( unique.inverse[t * 3 + k] )];

  // 边界顶点 NaN 修正（field_contours.py L154-161）。
  for ( std::size_t index = 0; index < unique.values.size(); ++index )
  {
    if ( std::isfinite( vertexValues[index] ) )
      continue;
    GeomPtr point = makePoint( handle, unique.values[index].x, unique.values[index].y );
    if ( geosDistance( handle, point.get(), field->domainBoundary.get() ) >= eps )
      continue;
    std::vector<int> positions;
    for ( std::size_t i = 0; i < unique.inverse.size(); ++i )
      if ( static_cast<std::size_t>( unique.inverse[i] ) == index )
        positions.push_back( static_cast<int>( i ) );
    if ( positions.empty() )
      continue;
    const auto &tri = mesh->xy[static_cast<std::size_t>( positions[0] ) / 3];
    Point2 center{ ( tri[0].x + tri[1].x + tri[2].x ) / 3.0,
                   ( tri[0].y + tri[1].y + tri[2].y ) / 3.0 };
    const double dx = center.x - unique.values[index].x;
    const double dy = center.y - unique.values[index].y;
    const double norm = std::max( std::hypot( dx, dy ), eps );
    const Point2 q{ unique.values[index].x + dx / norm * eps,
                    unique.values[index].y + dy / norm * eps };
    const std::vector<double> z = evaluate( handle, *field, preparedDomain, { q } );
    for ( const int pos : positions )
      mesh->values[static_cast<std::size_t>( pos ) / 3][pos % 3] = z[0];
  }

  // _region_ids：三角形重心 → 首个 intersects 的区域。
  mesh->regionIds.assign( mesh->xy.size(), -1 );
  std::vector<const GEOSPreparedGeometry *> preparedRegions;
  for ( const GEOSGeometry *region : mesh->regions )
    preparedRegions.push_back( GEOSPrepare_r( handle, region ) );
  for ( std::size_t t = 0; t < mesh->xy.size(); ++t )
  {
    const auto &tri = mesh->xy[t];
    const double cx = ( tri[0].x + tri[1].x + tri[2].x ) / 3.0;
    const double cy = ( tri[0].y + tri[1].y + tri[2].y ) / 3.0;
    for ( std::size_t rid = 0; rid < preparedRegions.size(); ++rid )
    {
      if ( GEOSPreparedIntersectsXY_r( handle, preparedRegions[rid], cx, cy ) == 1 )
      {
        mesh->regionIds[t] = static_cast<int>( rid );
        break;
      }
    }
  }
  for ( const GEOSPreparedGeometry *prep : preparedRegions )
    GEOSPreparedGeom_destroy_r( handle, prep );
  if ( std::any_of( mesh->regionIds.begin(), mesh->regionIds.end(),
                    []( int id ) { return id < 0; } ) )
  {
    *message = "打断线分区未完整覆盖提线网格，请检查边界与打断线几何";
    return false;
  }
  GEOSPreparedGeom_destroy_r( handle, preparedDomain );
  return true;
}

// ---- _merge_shared_endpoints（L168-173）----
std::vector<std::vector<Point2>> mergeSharedEndpoints(
    GEOSContextHandle_t handle, const std::vector<std::vector<Point2>> &lines )
{
  std::vector<std::vector<Point2>> out;
  if ( lines.empty() )
    return out;
  std::vector<GeomPtr> parts;
  parts.reserve( lines.size() );
  for ( const auto &line : lines )
    parts.push_back( makeLineString( handle, line ) );
  GeomPtr merged = lineMergeOf( handle, parts );
  if ( !merged )
    return out;
  std::vector<std::vector<Point2>> pieces;
  collectCoordParts( handle, merged.get(), GEOS_LINESTRING, &pieces );
  for ( auto &piece : pieces )
  {
    if ( polylineLength( piece ) > 1e-9 )
      out.push_back( std::move( piece ) );
  }
  return out;
}

// ---- _triangle_contours（L176-207）----
std::vector<std::vector<Point2>> triangleContours(
    GEOSContextHandle_t handle, const std::vector<std::array<Point2, 3>> &xyAll,
    const std::vector<std::array<double, 3>> &valuesAll, double level )
{
  // mask = np.isfinite(values).all(axis=1) → 先过滤行。
  std::vector<std::array<Point2, 3>> xy;
  std::vector<std::array<double, 3>> values;
  xy.reserve( xyAll.size() );
  values.reserve( valuesAll.size() );
  for ( std::size_t t = 0; t < xyAll.size(); ++t )
  {
    if ( std::isfinite( valuesAll[t][0] ) && std::isfinite( valuesAll[t][1] ) &&
         std::isfinite( valuesAll[t][2] ) )
    {
      xy.push_back( xyAll[t] );
      values.push_back( valuesAll[t] );
    }
  }
  double maxAbs = 1.0;
  for ( const auto &v : values )
    for ( const double z : v )
      maxAbs = std::max( maxAbs, std::abs( z ) );
  const double tolerance = kEps64 * std::max( 1.0, std::max( std::abs( level ), maxAbs ) ) * 32.0;

  std::vector<std::array<Point2, 2>> pairs;
  for ( std::size_t t = 0; t < xy.size(); ++t )
  {
    bool below[3], hit[3];
    int hits = 0;
    for ( int e = 0; e < 3; ++e )
      below[e] = values[t][e] < level - tolerance;
    for ( int e = 0; e < 3; ++e )
    {
      hit[e] = below[e] != below[( e + 1 ) % 3];
      if ( hit[e] )
        ++hits;
    }
    if ( hits != 2 )
      continue;
    std::array<Point2, 2> pair;
    int slot = 0;
    for ( int e = 0; e < 3; ++e )
    {
      if ( !hit[e] )
        continue;
      Point2 a = xy[t][e];
      Point2 b = xy[t][( e + 1 ) % 3];
      double z0 = values[t][e];
      double z1 = values[t][( e + 1 ) % 3];
      // 规范化边方向：共享交点逐位一致（swap 端点与值）。
      if ( a.x > b.x || ( a.x == b.x && a.y > b.y ) )
      {
        std::swap( a, b );
        std::swap( z0, z1 );
      }
      const double tt =
          std::min( 1.0, std::max( 0.0, ( level - z0 ) / ( z1 - z0 ) ) );
      Point2 p{ a.x + tt * ( b.x - a.x ), a.y + tt * ( b.y - a.y ) };
      if ( tt == 0.0 )
        p = a;
      else if ( tt == 1.0 )
        p = b;
      pair[static_cast<std::size_t>( slot++ )] = p;
    }
    if ( !( pair[0].x == pair[1].x && pair[0].y == pair[1].y ) )
      pairs.push_back( pair );
  }
  std::vector<std::vector<Point2>> segments;
  segments.reserve( pairs.size() );
  for ( const auto &pair : pairs )
    segments.push_back( { pair[0], pair[1] } );
  return mergeSharedEndpoints( handle, segments );
}

// ---- _partition_contours（L235-244）----
std::vector<ContourLevelLines> partitionContours( GEOSContextHandle_t handle,
                                                  const ContourMesh &mesh,
                                                  const std::vector<double> &levels )
{
  std::vector<std::vector<std::vector<Point2>>> pieces( levels.size() );
  for ( std::size_t rid = 0; rid < mesh.regions.size(); ++rid )
  {
    std::vector<std::array<Point2, 3>> xy;
    std::vector<std::array<double, 3>> values;
    for ( std::size_t t = 0; t < mesh.xy.size(); ++t )
    {
      if ( mesh.regionIds[t] != static_cast<int>( rid ) )
        continue;
      xy.push_back( mesh.xy[t] );
      values.push_back( mesh.values[t] );
    }
    for ( std::size_t li = 0; li < levels.size(); ++li )
    {
      // 区域可能无三角形——上游 _triangle_contours 对空输入给出空列表。
      std::vector<std::vector<Point2>> lines;
      if ( !xy.empty() )
        lines = triangleContours( handle, xy, values, levels[li] );
      for ( auto &line : lines )
        pieces[li].push_back( std::move( line ) );
    }
  }
  std::vector<ContourLevelLines> out;
  for ( std::size_t li = 0; li < levels.size(); ++li )
  {
    ContourLevelLines entry;
    entry.level = levels[li];
    entry.lines = mergeSharedEndpoints( handle, pieces[li] );
    out.push_back( std::move( entry ) );
  }
  return out;
}

// ---- _clip（L247-258，wall 空 → 仅 domain 相交）----
std::vector<ContourLevelLines> clipToDomain( GEOSContextHandle_t handle,
                                           const std::vector<ContourLevelLines> &contours,
                                           const GEOSGeometry *domain )
{
  std::vector<ContourLevelLines> out;
  for ( const ContourLevelLines &entry : contours )
  {
    ContourLevelLines clipped;
    clipped.level = entry.level;
    for ( const auto &line : entry.lines )
    {
      GeomPtr geometry = makeLineString( handle, line );
      GeomPtr intersection = geometry
                                 ? makeGeom( handle, GEOSIntersection_r( handle, geometry.get(),
                                                                       domain ) )
                                 : nullptr;
      std::vector<std::vector<Point2>> pieces;
      if ( intersection )
        collectCoordParts( handle, intersection.get(), GEOS_LINESTRING, &pieces );
      for ( auto &piece : pieces )
      {
        if ( polylineLength( piece ) > 1e-9 )
          clipped.lines.push_back( std::move( piece ) );
      }
    }
    out.push_back( std::move( clipped ) );
  }
  return out;
}

// ---- _smooth（L261-339，wall 恒空分支）----
std::vector<ContourLevelLines> smoothContours( GEOSContextHandle_t handle,
                                               const std::vector<ContourLevelLines> &contours,
                                               const TrendField &field,
                                               const GEOSPreparedGeometry *preparedDomain,
                                               double step )
{
  // cartographic_smooth_contours(contours, step, iterations=1)
  ContourLineMap rawMap;
  for ( const ContourLevelLines &entry : contours )
    rawMap.emplace_back( entry.level, entry.lines );
  const ContourLineMap smoothedMap = cartographicSmoothContours( rawMap, step, 1 );

  std::vector<std::pair<double, Polyline>> raw;
  for ( const ContourLevelLines &entry : contours )
    for ( const auto &line : entry.lines )
      raw.emplace_back( entry.level, line );
  std::vector<std::pair<double, Polyline>> candidate;
  for ( const auto &[level, lines] : smoothedMap )
    for ( const auto &line : lines )
      candidate.emplace_back( level, line );
  // 线数或级别序不匹配 → 保留源（上游契约）。
  if ( candidate.size() != raw.size() )
    return contours;
  for ( std::size_t i = 0; i < raw.size(); ++i )
    if ( candidate[i].first != raw[i].first )
      return contours;

  const double coverWidth = step * 1e-8;
  GeomPtr allowedBuffer =
      makeGeom( handle, GEOSBuffer_r( handle, field.domain.get(), coverWidth, 8 ) );
  const GEOSPreparedGeometry *preparedAllowed =
      allowedBuffer ? GEOSPrepare_r( handle, allowedBuffer.get() ) : nullptr;

  std::vector<std::pair<double, Polyline>> result( raw.size() );
  for ( std::size_t i = 0; i < raw.size(); ++i )
  {
    const double level = raw[i].first;
    Polyline points = candidate[i].second;
    const Polyline &original = raw[i].second;
    const bool closed =
        std::hypot( points.front().x - points.back().x,
                    points.front().y - points.back().y ) < step * 1e-7;
    std::vector<int> indices;
    if ( closed )
    {
      for ( std::size_t k = 0; k + 1 < points.size(); ++k )
        indices.push_back( static_cast<int>( k ) );
    }
    else
    {
      for ( std::size_t k = 1; k + 1 < points.size(); ++k )
        indices.push_back( static_cast<int>( k ) );
    }
    if ( !indices.empty() )
    {
      std::vector<Point2> q( indices.size() );
      std::vector<double> h( indices.size(), step * 0.025 );
      for ( std::size_t k = 0; k < indices.size(); ++k )
        q[k] = points[static_cast<std::size_t>( indices[k] )];
      for ( std::size_t k = 0; k < q.size(); ++k )
      {
        GeomPtr probe = makePoint( handle, q[k].x, q[k].y );
        h[k] = std::min( h[k],
                         geosDistance( handle, probe.get(), field.domainBoundary.get() ) * 0.2 );
        h[k] = std::max( h[k], step * 1e-8 );
      }
      for ( int iter = 0; iter < 2; ++iter )
      {
        std::vector<Point2> batch;
        batch.reserve( q.size() * 5 );
        for ( const Point2 &p : q )
          batch.push_back( p );
        for ( std::size_t k = 0; k < q.size(); ++k )
          batch.push_back( Point2{ q[k].x + h[k], q[k].y } );
        for ( std::size_t k = 0; k < q.size(); ++k )
          batch.push_back( Point2{ q[k].x - h[k], q[k].y } );
        for ( std::size_t k = 0; k < q.size(); ++k )
          batch.push_back( Point2{ q[k].x, q[k].y + h[k] } );
        for ( std::size_t k = 0; k < q.size(); ++k )
          batch.push_back( Point2{ q[k].x, q[k].y - h[k] } );
        const std::vector<double> z = evaluate( handle, field, preparedDomain, batch );
        const std::size_t n = q.size();
        for ( std::size_t k = 0; k < n; ++k )
        {
          const double gx = ( z[n + k] - z[2 * n + k] ) / ( 2 * h[k] );
          const double gy = ( z[3 * n + k] - z[4 * n + k] ) / ( 2 * h[k] );
          const double norm = gx * gx + gy * gy;
          double mx = ( level - z[k] ) * gx / std::max( norm, 1e-20 );
          double my = ( level - z[k] ) * gy / std::max( norm, 1e-20 );
          const double length = std::hypot( mx, my );
          const double scale = std::min( 1.0, step * 0.35 / std::max( length, 1e-20 ) );
          mx *= scale;
          my *= scale;
          if ( !std::isfinite( mx ) || !std::isfinite( my ) )
          {
            mx = 0;
            my = 0;
          }
          q[k].x += mx;
          q[k].y += my;
        }
      }
      for ( std::size_t k = 0; k < indices.size(); ++k )
        points[static_cast<std::size_t>( indices[k] )] = q[k];
      if ( closed )
        points.back() = points.front();
    }

    GeomPtr geometry = makeLineString( handle, points );
    bool valid = geometry &&
                 GEOSisSimple_r( handle, geometry.get() ) == 1 &&
                 preparedAllowed &&
                 GEOSPreparedCovers_r( handle, preparedAllowed, geometry.get() ) == 1;

    const auto valueError = [&]( const Polyline &coords ) {
      std::vector<Point2> probes = coords;
      for ( std::size_t k = 0; k + 1 < coords.size(); ++k )
        probes.push_back( Point2{ ( coords[k].x + coords[k + 1].x ) * 0.5,
                                  ( coords[k].y + coords[k + 1].y ) * 0.5 } );
      const std::vector<double> z = evaluate( handle, field, preparedDomain, probes );
      double worst = 0;
      for ( const double v : z )
      {
        if ( !std::isfinite( v ) )
          return std::numeric_limits<double>::infinity();
        worst = std::max( worst, std::abs( v - level ) );
      }
      return worst;
    };
    const double candidateError = valueError( points );
    valid = valid && std::isfinite( candidateError ) &&
            candidateError <= std::max( field.tolerance * 4.0, valueError( original ) + 1e-9 );
    result[i] = { level, valid ? points : original };
  }
  if ( preparedAllowed )
    GEOSPreparedGeom_destroy_r( handle, preparedAllowed );

  // 穿线回滚：平滑引入交叉的线整组回退（上游 STRtree+predicate 同集合语义）。
  std::unordered_set<int> changed;
  for ( std::size_t i = 0; i < result.size(); ++i )
    changed.insert( static_cast<int>( i ) );
  while ( !changed.empty() )
  {
    std::vector<GeomPtr> geometries;
    geometries.reserve( result.size() );
    for ( const auto &entry : result )
      geometries.push_back( makeLineString( handle, entry.second ) );
    std::unordered_set<int> bad;
    for ( std::size_t i = 0; i < geometries.size(); ++i )
    {
      for ( std::size_t j = 0; j < geometries.size(); ++j )
      {
        if ( i == j || !geometries[i] || !geometries[j] )
          continue;
        if ( GEOSCrosses_r( handle, geometries[i].get(), geometries[j].get() ) == 1 )
        {
          if ( changed.count( static_cast<int>( i ) ) )
            bad.insert( static_cast<int>( i ) );
          if ( changed.count( static_cast<int>( j ) ) )
            bad.insert( static_cast<int>( j ) );
        }
      }
    }
    if ( bad.empty() )
      break;
    for ( const int i : bad )
      result[static_cast<std::size_t>( i )] = raw[static_cast<std::size_t>( i )];
    for ( const int i : bad )
      changed.erase( i );
  }

  std::vector<ContourLevelLines> out;
  for ( const ContourLevelLines &entry : contours )
    out.push_back( ContourLevelLines{ entry.level, {} } );
  for ( const auto &[level, line] : result )
  {
    for ( ContourLevelLines &entry : out )
    {
      if ( entry.level == level )
      {
        entry.lines.push_back( line );
        break;
      }
    }
  }
  return out;
}

// ---- filter_contour_fragments（L210-232）----
std::vector<ContourLevelLines> filterContourFragments(
    GEOSContextHandle_t handle, const std::vector<ContourLevelLines> &contours,
    double step )
{
  std::vector<ContourLevelLines> out;
  for ( const ContourLevelLines &entry : contours )
  {
    ContourLevelLines kept;
    kept.level = entry.level;
    for ( const auto &line : entry.lines )
    {
      if ( line.size() < 2 )
        continue;
      double xmin = line[0].x, xmax = line[0].x;
      double ymin = line[0].y, ymax = line[0].y;
      for ( const Point2 &p : line )
      {
        xmin = std::min( xmin, p.x );
        xmax = std::max( xmax, p.x );
        ymin = std::min( ymin, p.y );
        ymax = std::max( ymax, p.y );
      }
      const double extent = std::max( xmax - xmin, ymax - ymin );
      GeomPtr geometry = makeLineString( handle, line );
      const bool ring = geometry && GEOSisRing_r( handle, geometry.get() ) == 1;
      const double length = polylineLength( line );
      const bool tiny =
          ring ? ( extent <= step * 2.0 && length <= step * 8.0 )
               : ( extent <= step * 1.25 && length <= step * 1.5 );
      if ( !tiny )
        kept.lines.push_back( line );
    }
    out.push_back( std::move( kept ) );
  }
  return out;
}

// ---- StoredTrendField 构造 ----
bool buildField( GEOSContextHandle_t handle, const FieldContourSurface &surface,
                 TrendField *field, std::string *message )
{
  field->xs = surface.xs;
  field->ys = surface.ys;
  field->grid = surface.grid;
  field->nx = surface.xs.size();
  field->ny = surface.ys.size();
  if ( field->nx < 2 || field->ny < 2 || field->grid.size() != field->nx * field->ny )
  {
    *message = "趋势面栅格尺寸不合法";
    return false;
  }
  if ( !surface.validMask.empty() )
  {
    if ( surface.validMask.size() != field->grid.size() )
    {
      *message = "趋势面有效范围与栅格尺寸不一致";
      return false;
    }
    for ( std::size_t i = 0; i < field->grid.size(); ++i )
      if ( !surface.validMask[i] )
        field->grid[i] = std::numeric_limits<double>::quiet_NaN();
  }
  // xs/ys 升序保证（上游翻转分支；本实现要求调用方给升序）。
  for ( std::size_t i = 1; i < field->xs.size(); ++i )
    if ( !( field->xs[i] > field->xs[i - 1] ) )
    {
      *message = "grid_x 必须严格升序";
      return false;
    }
  for ( std::size_t i = 1; i < field->ys.size(); ++i )
    if ( !( field->ys[i] > field->ys[i - 1] ) )
    {
      *message = "grid_y 必须严格升序";
      return false;
    }

  if ( !surface.boundaries.empty() )
  {
    // shapely unary_union([Polygon(exterior, holes)]) —— GEOMETRYCOLLECTION。
    field->domain = unionBoundaryDomain( handle, surface.boundaries );
  }
  else
  {
    // box(xs[0], ys[0], xs[-1], ys[-1]) —— shapely box 逆时针外环。
    Polygon box;
    box.exterior.points = { Point2{ field->xs.front(), field->ys.front() },
                            Point2{ field->xs.back(), field->ys.front() },
                            Point2{ field->xs.back(), field->ys.back() },
                            Point2{ field->xs.front(), field->ys.back() },
                            Point2{ field->xs.front(), field->ys.front() } };
    field->domain = makePolygon( handle, box );
  }
  if ( !field->domain )
  {
    *message = "成图边界几何无效，请先修正自交或孔洞";
    return false;
  }
  // regions = surface-stage domain（上游 field_model.regions 恒为
  // [union(boundaries) ∩ union(areas)]）→ domain ∩ unary_union(regions)。
  GeomPtr stageDomain = unionBoundaryDomain( handle, surface.boundaries );
  if ( !surface.interpolationAreas.empty() )
  {
    GeomPtr areas = unionBoundaryDomain( handle, surface.interpolationAreas );
    stageDomain = makeGeom( handle, GEOSIntersection_r( handle, stageDomain.get(),
                                                      areas.get() ) );
  }
  GeomPtr regionUnion = unaryUnionOf( handle, { stageDomain.get() } );
  field->domain = makeGeom( handle, GEOSIntersection_r( handle, field->domain.get(),
                                                      regionUnion.get() ) );
  if ( !field->domain )
  {
    *message = "成图域求交失败";
    return false;
  }
  field->domainBoundary = makeGeom( handle, GEOSBoundary_r( handle, field->domain.get() ) );

  // inside 节点掩膜 + tolerance。
  const GEOSPreparedGeometry *preparedDomain =
      GEOSPrepare_r( handle, field->domain.get() );
  if ( !preparedDomain )
  {
    *message = "GEOS 域索引失败";
    return false;
  }
  field->inside.assign( field->nx * field->ny, 0 );
  double minValue = std::numeric_limits<double>::infinity();
  double maxValue = -std::numeric_limits<double>::infinity();
  bool anyFinite = false;
  for ( std::size_t r = 0; r < field->ny; ++r )
  {
    for ( std::size_t c = 0; c < field->nx; ++c )
    {
      const std::size_t idx = r * field->nx + c;
      if ( GEOSPreparedIntersectsXY_r( handle, preparedDomain, field->xs[c],
                                     field->ys[r] ) == 1 )
        field->inside[idx] = 1;
      const double v = field->grid[idx];
      if ( std::isfinite( v ) )
      {
        anyFinite = true;
        minValue = std::min( minValue, v );
        maxValue = std::max( maxValue, v );
      }
    }
  }
  field->tolerance =
      anyFinite ? std::max( ( maxValue - minValue ) * 2e-4, 1e-9 ) : 1e-9;
  GEOSPreparedGeom_destroy_r( handle, preparedDomain );
  return true;
}

// field_only 提取尾部：mesh → partition → clip → smooth → filter。
bool extractFieldOnly( GEOSContextHandle_t handle, const FieldContourSurface &surface,
                       const std::vector<double> &levels, bool smooth,
                       std::vector<ContourLevelLines> *out, double *meshStep,
                       std::string *message )
{
  TrendField field;
  if ( !buildField( handle, surface, &field, message ) )
    return false;
  const GEOSPreparedGeometry *preparedDomain =
      GEOSPrepare_r( handle, field.domain.get() );
  if ( !preparedDomain )
  {
    *message = "GEOS 域索引失败";
    return false;
  }
  ContourMesh mesh;
  const bool meshed = buildMesh( handle, &field, &mesh, message );
  if ( !meshed )
  {
    GEOSPreparedGeom_destroy_r( handle, preparedDomain );
    return false;
  }
  std::vector<ContourLevelLines> raw = partitionContours( handle, mesh, levels );
  raw = clipToDomain( handle, raw, field.domain.get() );
  if ( smooth )
    raw = smoothContours( handle, raw, field, preparedDomain, mesh.step );
  GEOSPreparedGeom_destroy_r( handle, preparedDomain );
  *out = filterContourFragments( handle, raw, mesh.step );
  *meshStep = mesh.step;
  return true;
}

// ---- build_contour_work_surface（contour_work_field.py L10-59）----
// 就地改写 surface.grid；*wroteInfo = 是否写入了 contour_work_info。
bool buildContourWorkSurface( GEOSContextHandle_t handle, FieldContourSurface *surface,
                              const std::vector<const FieldContourBarrier *> &walls,
                              const std::vector<double> &levels, double shoulderOverride,
                              ContourWorkInfo *info, bool *wroteInfo,
                              std::string *message )
{
  *wroteInfo = false;
  const std::size_t cells = surface->grid.size();
  std::vector<std::uint8_t> finite( cells );
  bool anyFinite = false;
  for ( std::size_t i = 0; i < cells; ++i )
  {
    finite[i] = static_cast<std::uint8_t>( std::isfinite( surface->grid[i] ) );
    if ( !surface->validMask.empty() )
      finite[i] &= surface->validMask[i];
    anyFinite |= finite[i] != 0;
  }
  if ( walls.empty() || levels.empty() || !anyFinite )
    return true; // work = 原样（contour_work_info 不写入）
  for ( const double level : levels )
    if ( !std::isfinite( level ) )
    {
      *message = "等值级别必须为有限数值";
      return false;
    }

  const std::vector<double> &xs = surface->xs;
  const std::vector<double> &ys = surface->ys;
  double dx = 0, dy = 0;
  for ( std::size_t i = 1; i < xs.size(); ++i )
    dx = std::max( dx, std::abs( xs[i] - xs[i - 1] ) );
  for ( std::size_t i = 1; i < ys.size(); ++i )
    dy = std::max( dy, std::abs( ys[i] - ys[i - 1] ) );
  const double guard = 2.0 * std::hypot( dx, dy );
  const double width = std::max( surface->barrierBufferDistance,
                                surface->contourStopBufferDistance );
  const double span = std::max( xs.back() - xs.front(), ys.back() - ys.front() );
  double shoulder = shoulderOverride;
  if ( shoulder <= 0 )
    shoulder = std::max( width * 8.0, std::max( guard * 4.0, span * 0.025 ) );
  double gridPtp = 0, levelPtp = 0;
  {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for ( std::size_t i = 0; i < cells; ++i )
      if ( finite[i] )
      {
        lo = std::min( lo, surface->grid[i] );
        hi = std::max( hi, surface->grid[i] );
      }
    gridPtp = hi - lo;
    double llo = *std::min_element( levels.begin(), levels.end() );
    double lhi = *std::max_element( levels.begin(), levels.end() );
    levelPtp = lhi - llo;
  }
  const double scale = std::max( gridPtp, std::max( levelPtp, 1e-9 ) );

  // distance = 节点 → union_all(walls)。
  std::vector<GeomPtr> wallGeoms;
  wallGeoms.reserve( walls.size() );
  for ( const FieldContourBarrier *wall : walls )
    wallGeoms.push_back( makeLineString( handle, wall->points ) );
  std::vector<const GEOSGeometry *> wallItems;
  for ( const GeomPtr &g : wallGeoms )
    wallItems.push_back( g.get() );
  GeomPtr wallUnion = unaryUnionOf( handle, wallItems );
  std::vector<double> distance( cells );
  for ( std::size_t i = 0; i < cells; ++i )
  {
    const double x = xs[i % xs.size()];
    const double y = ys[i / xs.size()];
    GeomPtr point = makePoint( handle, x, y );
    distance[i] = geosDistance( handle, point.get(), wallUnion.get() );
  }

  // representative = 局部有限值中位数（width+guard 邻域），缺省全体中位数。
  std::vector<double> local;
  std::vector<double> allFinite;
  for ( std::size_t i = 0; i < cells; ++i )
  {
    if ( !finite[i] )
      continue;
    allFinite.push_back( surface->grid[i] );
    if ( distance[i] <= width + guard )
      local.push_back( surface->grid[i] );
  }
  const auto median = []( std::vector<double> values ) {
    std::sort( values.begin(), values.end() );
    const std::size_t n = values.size();
    return n % 2 ? values[n / 2] : ( values[n / 2 - 1] + values[n / 2] ) * 0.5;
  };
  const double representative =
      local.empty() ? median( allFinite ) : median( local );

  std::vector<double> ordered = levels;
  std::sort( ordered.begin(), ordered.end() );
  ordered.erase( std::unique( ordered.begin(), ordered.end() ), ordered.end() );
  std::vector<double> gaps;
  gaps.push_back( ordered.front() - scale * 0.02 );
  for ( std::size_t i = 1; i < ordered.size(); ++i )
    gaps.push_back( ( ordered[i - 1] + ordered[i] ) * 0.5 );
  gaps.push_back( ordered.back() + scale * 0.02 );
  double core = gaps.front();
  double best = std::abs( gaps.front() - representative );
  for ( const double gap : gaps )
  {
    if ( std::abs( gap - representative ) < best )
    {
      best = std::abs( gap - representative );
      core = gap;
    }
  }

  int modified = 0;
  for ( std::size_t i = 0; i < cells; ++i )
  {
    const double t =
        std::min( 1.0, std::max( 0.0, ( distance[i] - width - guard ) / shoulder ) );
    if ( !( finite[i] && t < 1.0 ) )
      continue;
    const double blend = t * t * t * ( 10.0 - 15.0 * t + 6.0 * t * t );
    surface->grid[i] = core + blend * ( surface->grid[i] - core );
    ++modified;
  }
  info->coreValue = core;
  info->bufferHalfWidth = width;
  info->numericalGuard = guard;
  info->transitionDistance = shoulder;
  info->modifiedCells = modified;
  *wroteInfo = true;
  return true;
}

} // namespace

FieldContourResult extractFieldContours( const FieldContourSurface &surface,
                                         const std::vector<double> &levels,
                                         bool smooth )
{
  FieldContourResult result;
  GeosContext geos;
  if ( !geos.handle )
  {
    result.status = Status::NumericalFailure;
    result.message = "GEOS 初始化失败";
    return result;
  }
  const std::string &policy = surface.partition.geometryPolicy;
  if ( policy != "field_only" && policy != "local_interpretive_detour" )
  {
    result.status = Status::InvalidInput;
    result.message = "不支持的 contour_partition.geometry_policy：" + policy;
    return result;
  }

  // base：geometry_policy → field_only。
  FieldContourSurface base = surface;
  base.partition.geometryPolicy = "field_only";

  std::string message;
  double meshStep = 0;
  if ( !extractFieldOnly( geos.handle, base, levels, smooth, &result.initial,
                          &meshStep, &message ) )
  {
    result.status = Status::InvalidInput;
    result.message = message;
    return result;
  }
  result.meshStep = meshStep;

  if ( policy == "field_only" )
  {
    result.contours = result.initial;
    result.status = Status::Ok;
    return result;
  }

  // local_detour_surface：被初始等值线穿过的停线才进工作场。
  std::vector<GeomPtr> lines;
  for ( const ContourLevelLines &entry : result.initial )
    for ( const auto &line : entry.lines )
      lines.push_back( makeLineString( geos.handle, line ) );

  std::vector<const FieldContourBarrier *> selected;
  for ( std::size_t i = 0; i < base.barriers.size(); ++i )
  {
    const FieldContourBarrier &barrier = base.barriers[i];
    if ( !barrier.active || !isContourStopMode( barrier.blockMode ) )
      continue;
    GeomPtr wall = makeLineString( geos.handle, barrier.points );
    bool crossed = false;
    for ( const GeomPtr &line : lines )
    {
      if ( line && wall &&
           GEOSCrosses_r( geos.handle, line.get(), wall.get() ) == 1 )
      {
        crossed = true;
        break;
      }
    }
    if ( crossed )
    {
      selected.push_back( &barrier );
      result.crossedBarrierIndices.push_back( static_cast<int>( i ) );
    }
  }
  result.detourApplied = !selected.empty();
  if ( !result.detourApplied )
  {
    result.contours = result.initial;
    result.status = Status::Ok;
    return result;
  }

  const double width = std::max( base.barrierBufferDistance,
                                 base.contourStopBufferDistance );
  // policy['work_transition_distance'] = policy.get('shape_radius',0.) or width*6.
  const double shoulder =
      base.partition.shapeRadius != 0.0 ? base.partition.shapeRadius : width * 6.0;

  // work = dict(base, barriers=selected, partition=field_only+过渡距离)。
  FieldContourSurface work = base;
  work.barriers.clear();
  work.barriers.reserve( selected.size() );
  for ( const FieldContourBarrier *barrier : selected )
    work.barriers.push_back( *barrier );
  ContourWorkInfo info;
  bool wroteInfo = false;
  if ( !buildContourWorkSurface( geos.handle, &work,
                                 std::vector<const FieldContourBarrier *>(
                                     selected.begin(), selected.end() ),
                                 levels, shoulder, &info, &wroteInfo, &message ) )
  {
    result.status = Status::InvalidInput;
    result.message = message;
    return result;
  }
  result.workBuilt = wroteInfo;
  result.workInfo = info;

  double workStep = 0;
  if ( !extractFieldOnly( geos.handle, work, levels, smooth, &result.contours,
                          &workStep, &message ) )
  {
    result.status = Status::InvalidInput;
    result.message = message;
    return result;
  }
  result.status = Status::Ok;
  return result;
}

namespace
{

std::vector<Point2> jsonPoints( const QVariant &value )
{
  std::vector<Point2> points;
  for ( const QVariant &item : value.toList() )
  {
    const QVariantList pair = item.toList();
    if ( pair.size() >= 2 )
      points.push_back( Point2{ pair[0].toDouble(), pair[1].toDouble() } );
  }
  return points;
}

Polygon jsonPolygon( const QVariant &value )
{
  const QVariantMap map = value.toMap();
  Polygon polygon;
  polygon.exterior.points = jsonPoints( map.value( QStringLiteral( "exterior" ) ) );
  for ( const QVariant &hole : map.value( QStringLiteral( "holes" ) ).toList() )
  {
    Ring ring;
    ring.points = jsonPoints( hole );
    polygon.holes.push_back( std::move( ring ) );
  }
  return polygon;
}

} // namespace

bool loadFieldContourSurface( const QVariantMap &model, FieldContourSurface *surface,
                              QString *error )
{
  const QVariantMap grid = model.value( QStringLiteral( "grid" ) ).toMap();
  const QVariantList xs = grid.value( QStringLiteral( "x" ) ).toList();
  const QVariantList ys = grid.value( QStringLiteral( "y" ) ).toList();
  const QVariantList zRows = grid.value( QStringLiteral( "z" ) ).toList();
  const int nx = xs.size(), ny = ys.size();
  if ( nx < 2 || ny < 2 || zRows.size() != ny )
  {
    if ( error )
      *error = QStringLiteral( "structural 侧卡栅格尺寸不合法" );
    return false;
  }
  surface->xs.clear();
  surface->ys.clear();
  for ( const QVariant &v : xs )
    surface->xs.push_back( v.toDouble() );
  for ( const QVariant &v : ys )
    surface->ys.push_back( v.toDouble() );
  surface->grid.assign( static_cast<std::size_t>( nx ) * static_cast<std::size_t>( ny ),
                        std::numeric_limits<double>::quiet_NaN() );
  for ( int r = 0; r < ny; ++r )
  {
    const QVariantList row = zRows[r].toList();
    if ( row.size() != nx )
    {
      if ( error )
        *error = QStringLiteral( "structural 侧卡栅格行宽不一致" );
      return false;
    }
    for ( int c = 0; c < nx; ++c )
    {
      const QVariant &v = row[c];
      if ( v.isValid() && !v.isNull() )
        surface->grid[static_cast<std::size_t>( r ) * static_cast<std::size_t>( nx ) +
                      static_cast<std::size_t>( c )] = v.toDouble();
    }
  }
  const QVariantList maskRows = model.value( QStringLiteral( "valid_mask" ) ).toList();
  if ( maskRows.size() == ny )
  {
    surface->validMask.assign(
        static_cast<std::size_t>( nx ) * static_cast<std::size_t>( ny ), 0 );
    for ( int r = 0; r < ny; ++r )
    {
      const QVariantList row = maskRows[r].toList();
      if ( row.size() != nx )
      {
        if ( error )
          *error = QStringLiteral( "structural 侧卡 valid_mask 行宽不一致" );
        return false;
      }
      for ( int c = 0; c < nx; ++c )
        surface->validMask[static_cast<std::size_t>( r ) * static_cast<std::size_t>( nx ) +
                           static_cast<std::size_t>( c )] = row[c].toBool() ? 1 : 0;
    }
  }
  else
  {
    surface->validMask.clear();
  }
  surface->boundaries.clear();
  for ( const QVariant &item : model.value( QStringLiteral( "boundaries" ) ).toList() )
    surface->boundaries.push_back( jsonPolygon( item ) );
  surface->interpolationAreas.clear();
  for ( const QVariant &item :
        model.value( QStringLiteral( "interpolation_areas" ) ).toList() )
    surface->interpolationAreas.push_back( jsonPolygon( item ) );
  surface->barriers.clear();
  for ( const QVariant &item : model.value( QStringLiteral( "barriers" ) ).toList() )
  {
    const QVariantMap map = item.toMap();
    FieldContourBarrier barrier;
    barrier.points = jsonPoints( map.value( QStringLiteral( "points" ) ) );
    barrier.active = map.value( QStringLiteral( "active" ), true ).toBool();
    barrier.blockMode =
        map.value( QStringLiteral( "blockMode" ), QStringLiteral( "full_block" ) )
            .toString()
            .toStdString();
    surface->barriers.push_back( std::move( barrier ) );
  }
  surface->barrierBufferDistance =
      model.value( QStringLiteral( "barrier_buffer_distance" ), 0.0 ).toDouble();
  surface->contourStopBufferDistance =
      model.value( QStringLiteral( "contour_stop_buffer_distance" ), 0.0 ).toDouble();
  const QVariantMap partition =
      model.value( QStringLiteral( "contour_partition" ) ).toMap();
  surface->partition.geometryPolicy =
      partition.value( QStringLiteral( "geometry_policy" ), QStringLiteral( "field_only" ) )
          .toString()
          .toStdString();
  surface->partition.bufferPolicy =
      partition.value( QStringLiteral( "buffer_policy" ) ).toString().toStdString();
  surface->partition.shapeRadius =
      partition.value( QStringLiteral( "shape_radius" ), 0.0 ).toDouble();
  surface->partition.shapeStrength =
      partition.value( QStringLiteral( "shape_strength" ), 1.0 ).toDouble();
  surface->partition.workTransitionDistance =
      partition.value( QStringLiteral( "work_transition_distance" ), 0.0 ).toDouble();
  surface->partition.preserveClosed =
      partition.value( QStringLiteral( "preserve_closed" ), false ).toBool();
  return true;
}

} // namespace paleo::singlefactor
