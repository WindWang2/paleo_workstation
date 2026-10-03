// 层：数据
#pragma once

#include "types.h"

#include <geos_c.h>

#include <cmath>
#include <memory>
#include <vector>

// 单因素算法层共享的 GEOS C API 薄封装（reentrant `_r` 句柄）。
// 语义目标：按 shapely 2.1.2 的调用顺序逐项等价——unary_union =
// GEOMETRYCOLLECTION(list 顺序) + GEOSUnaryUnion，polygonize =
// collection + GEOSPolygonize，line_merge = GEOSLineMerge。

namespace paleo::singlefactor
{

struct GeosContext
{
  GEOSContextHandle_t handle = nullptr;
  GeosContext() { handle = GEOS_init_r(); }
  ~GeosContext()
  {
    if ( handle )
      GEOS_finish_r( handle );
  }
  GeosContext( const GeosContext & ) = delete;
  GeosContext &operator=( const GeosContext & ) = delete;
};

struct GeomDeleter
{
  GEOSContextHandle_t handle = nullptr;
  void operator()( GEOSGeometry *geometry ) const
  {
    if ( geometry )
      GEOSGeom_destroy_r( handle, geometry );
  }
};
using GeomPtr = std::unique_ptr<GEOSGeometry, GeomDeleter>;

inline GeomPtr makeGeom( GEOSContextHandle_t handle, GEOSGeometry *geometry )
{
  return GeomPtr( geometry, GeomDeleter{ handle } );
}

inline GEOSCoordSequence *makeCoordSeq( GEOSContextHandle_t handle,
                                        const std::vector<Point2> &points )
{
  GEOSCoordSequence *seq =
      GEOSCoordSeq_create_r( handle, static_cast<unsigned int>( points.size() ), 2 );
  if ( !seq )
    return nullptr;
  for ( unsigned int i = 0; i < points.size(); ++i )
    GEOSCoordSeq_setXY_r( handle, seq, i, points[i].x, points[i].y );
  return seq;
}

// 外环 + 孔洞 → GEOS Polygon；外环自动闭合（GEOS 接受未闭合输入）
inline GeomPtr makePolygon( GEOSContextHandle_t handle, const Polygon &polygon )
{
  if ( polygon.exterior.points.empty() )
    return makeGeom( handle, GEOSGeom_createEmptyPolygon_r( handle ) );
  GEOSGeometry *shell = GEOSGeom_createLinearRing_r(
      handle, makeCoordSeq( handle, polygon.exterior.points ) );
  if ( !shell )
    return nullptr;
  std::vector<GEOSGeometry *> holes;
  holes.reserve( polygon.holes.size() );
  for ( const Ring &hole : polygon.holes )
  {
    if ( hole.points.empty() )
      continue;
    GEOSGeometry *holeRing =
        GEOSGeom_createLinearRing_r( handle, makeCoordSeq( handle, hole.points ) );
    if ( holeRing )
      holes.push_back( holeRing );
  }
  return makeGeom( handle, GEOSGeom_createPolygon_r( handle, shell, holes.data(),
                                                   static_cast<unsigned int>( holes.size() ) ) );
}

inline GeomPtr makeLineString( GEOSContextHandle_t handle,
                               const std::vector<Point2> &points )
{
  return makeGeom( handle, GEOSGeom_createLineString_r(
                             handle, makeCoordSeq( handle, points ) ) );
}

inline GeomPtr makePoint( GEOSContextHandle_t handle, double x, double y )
{
  GEOSCoordSequence *seq = GEOSCoordSeq_create_r( handle, 1, 2 );
  if ( !seq )
    return nullptr;
  GEOSCoordSeq_setXY_r( handle, seq, 0, x, y );
  return makeGeom( handle, GEOSGeom_createPoint_r( handle, seq ) );
}

// 按 shapely `getattr(lines, "geoms", None) or lines` 展开一个几何的
// 直接成员（非集合 → 单元素列表）。
inline std::vector<const GEOSGeometry *> directParts( GEOSContextHandle_t handle,
                                                      const GEOSGeometry *geometry )
{
  std::vector<const GEOSGeometry *> out;
  if ( !geometry )
    return out;
  const int type = GEOSGeomTypeId_r( handle, geometry );
  if ( type == GEOS_MULTIPOINT || type == GEOS_MULTILINESTRING ||
       type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION )
  {
    const int count = GEOSGetNumGeometries_r( handle, geometry );
    for ( int i = 0; i < count; ++i )
      out.push_back( GEOSGetGeometryN_r( handle, geometry, i ) );
    return out;
  }
  out.push_back( geometry );
  return out;
}

// shapely unary_union / union_all：collection(list 顺序) → GEOSUnaryUnion。
// 输入指针不转移所有权（克隆进 collection）。
inline GeomPtr unaryUnionOf( GEOSContextHandle_t handle,
                             std::vector<const GEOSGeometry *> items )
{
  std::vector<GEOSGeometry *> owned;
  owned.reserve( items.size() );
  for ( const GEOSGeometry *item : items )
  {
    if ( item )
      owned.push_back( GEOSGeom_clone_r( handle, item ) );
  }
  GeomPtr collection = makeGeom(
      handle,
      GEOSGeom_createCollection_r( handle, GEOS_GEOMETRYCOLLECTION, owned.data(),
                                   static_cast<unsigned int>( owned.size() ) ) );
  return makeGeom( handle, GEOSUnaryUnion_r( handle, collection.get() ) );
}

inline GeomPtr unaryUnionOf( GEOSContextHandle_t handle,
                             std::initializer_list<const GEOSGeometry *> items )
{
  return unaryUnionOf( handle, std::vector<const GEOSGeometry *>( items ) );
}

// shapely ops.polygonize：GEOMETRYCOLLECTION(obs) → GEOSPolygonize。
inline GeomPtr polygonizeOf( GEOSContextHandle_t handle,
                             const std::vector<const GEOSGeometry *> &items )
{
  std::vector<GEOSGeometry *> owned;
  owned.reserve( items.size() );
  for ( const GEOSGeometry *item : items )
  {
    if ( item )
      owned.push_back( GEOSGeom_clone_r( handle, item ) );
  }
  GeomPtr collection = makeGeom(
      handle,
      GEOSGeom_createCollection_r( handle, GEOS_GEOMETRYCOLLECTION, owned.data(),
                                   static_cast<unsigned int>( owned.size() ) ) );
  // shapely polygonize(collection) → GEOSPolygonize(&collection, 1)。
  const GEOSGeometry *input = collection.get();
  return makeGeom( handle, GEOSPolygonize_r( handle, &input, 1 ) );
}

// shapely line_merge(MultiLineString)：GEOSLineMerge。
inline GeomPtr lineMergeOf( GEOSContextHandle_t handle,
                            const std::vector<GeomPtr> &lines )
{
  std::vector<GEOSGeometry *> owned;
  owned.reserve( lines.size() );
  for ( const GeomPtr &line : lines )
  {
    if ( line )
      owned.push_back( GEOSGeom_clone_r( handle, line.get() ) );
  }
  GeomPtr collection = makeGeom(
      handle,
      GEOSGeom_createCollection_r( handle, GEOS_MULTILINESTRING, owned.data(),
                                   static_cast<unsigned int>( owned.size() ) ) );
  return makeGeom( handle, GEOSLineMerge_r( handle, collection.get() ) );
}

// 递归收集指定几何类型的坐标段（上游 _parts(geom, kind)）。
// kind = GEOS_LINESTRING / GEOS_POLYGON 等 GEOS 类型 id。
inline void collectCoordParts( GEOSContextHandle_t handle, const GEOSGeometry *geometry,
                               int kind, std::vector<std::vector<Point2>> *out )
{
  if ( !geometry )
    return;
  const int type = GEOSGeomTypeId_r( handle, geometry );
  if ( type == kind )
  {
    if ( kind == GEOS_LINESTRING || kind == GEOS_LINEARRING )
    {
      const GEOSCoordSequence *seq = GEOSGeom_getCoordSeq_r( handle, geometry );
      unsigned int size = 0;
      if ( seq && GEOSCoordSeq_getSize_r( handle, seq, &size ) && size > 0 )
      {
        std::vector<Point2> points( size );
        for ( unsigned int i = 0; i < size; ++i )
          GEOSCoordSeq_getXY_r( handle, seq, i, &points[i].x, &points[i].y );
        out->push_back( std::move( points ) );
      }
    }
    else if ( kind == GEOS_POINT )
    {
      double x = 0, y = 0;
      if ( GEOSGeomGetX_r( handle, geometry, &x ) &&
           GEOSGeomGetY_r( handle, geometry, &y ) )
        out->push_back( { Point2{ x, y } } );
    }
    return;
  }
  if ( type == GEOS_GEOMETRYCOLLECTION || type == GEOS_MULTILINESTRING ||
       type == GEOS_MULTIPOINT || type == GEOS_MULTIPOLYGON )
  {
    const int count = GEOSGetNumGeometries_r( handle, geometry );
    for ( int i = 0; i < count; ++i )
      collectCoordParts( handle, GEOSGetGeometryN_r( handle, geometry, i ), kind, out );
  }
}

// 递归收集 LineString 坐标段，含 LINEARRING（structural 方向裁剪用）。
inline void collectLineParts( GEOSContextHandle_t handle, const GEOSGeometry *geometry,
                              std::vector<std::vector<Point2>> *out )
{
  if ( !geometry )
    return;
  const int type = GEOSGeomTypeId_r( handle, geometry );
  if ( type == GEOS_LINESTRING || type == GEOS_LINEARRING )
  {
    collectCoordParts( handle, geometry, type, out );
    return;
  }
  if ( type == GEOS_MULTILINESTRING || type == GEOS_GEOMETRYCOLLECTION )
  {
    const int count = GEOSGetNumGeometries_r( handle, geometry );
    for ( int i = 0; i < count; ++i )
      collectLineParts( handle, GEOSGetGeometryN_r( handle, geometry, i ), out );
  }
}

// 递归收集 Polygon 几何指针（上游 _parts(domain, 'Polygon')）。
inline void collectPolygons( GEOSContextHandle_t handle, const GEOSGeometry *geometry,
                             std::vector<const GEOSGeometry *> *out )
{
  if ( !geometry )
    return;
  const int type = GEOSGeomTypeId_r( handle, geometry );
  if ( type == GEOS_POLYGON )
  {
    out->push_back( geometry );
    return;
  }
  if ( type == GEOS_MULTIPOLYGON || type == GEOS_GEOMETRYCOLLECTION )
  {
    const int count = GEOSGetNumGeometries_r( handle, geometry );
    for ( int i = 0; i < count; ++i )
      collectPolygons( handle, GEOSGetGeometryN_r( handle, geometry, i ), out );
  }
}

// 多边形列表并集：MULTIPOLYGON collection + unary_union（上游 unary_union
// 边界路径沿用历史顺序，不改既有 surface 语义）。
inline GeomPtr unionPolygons( GEOSContextHandle_t handle,
                              const std::vector<Polygon> &polygons )
{
  std::vector<GEOSGeometry *> parts;
  parts.reserve( polygons.size() );
  for ( const Polygon &polygon : polygons )
  {
    GeomPtr part = makePolygon( handle, polygon );
    if ( part )
      parts.push_back( part.release() );
  }
  if ( parts.empty() )
    return makeGeom( handle, GEOSGeom_createEmptyCollection_r( handle, GEOS_MULTIPOLYGON ) );
  GeomPtr collection = makeGeom(
      handle, GEOSGeom_createCollection_r( handle, GEOS_MULTIPOLYGON, parts.data(),
                                           static_cast<unsigned int>( parts.size() ) ) );
  return makeGeom( handle, GEOSUnaryUnion_r( handle, collection.get() ) );
}

// StoredTrendField 域：shapely unary_union([Polygon(...)]) 用
// GEOMETRYCOLLECTION（上游按 shape/mapping 语义，不是 MULTIPOLYGON）。
inline GeomPtr unionBoundaryDomain( GEOSContextHandle_t handle,
                                    const std::vector<Polygon> &polygons )
{
  std::vector<GEOSGeometry *> parts;
  parts.reserve( polygons.size() );
  for ( const Polygon &polygon : polygons )
  {
    GeomPtr part = makePolygon( handle, polygon );
    if ( part )
      parts.push_back( part.release() );
  }
  GeomPtr collection = makeGeom(
      handle, GEOSGeom_createCollection_r( handle, GEOS_GEOMETRYCOLLECTION,
                                           parts.data(),
                                           static_cast<unsigned int>( parts.size() ) ) );
  return makeGeom( handle, GEOSUnaryUnion_r( handle, collection.get() ) );
}

inline std::vector<Point2> lineCoords( GEOSContextHandle_t handle,
                                       const GEOSGeometry *line )
{
  std::vector<Point2> out;
  if ( !line )
    return out;
  const GEOSCoordSequence *seq = GEOSGeom_getCoordSeq_r( handle, line );
  unsigned int size = 0;
  if ( !seq || !GEOSCoordSeq_getSize_r( handle, seq, &size ) )
    return out;
  out.resize( size );
  for ( unsigned int i = 0; i < size; ++i )
    GEOSCoordSeq_getXY_r( handle, seq, i, &out[i].x, &out[i].y );
  return out;
}

// 多边形外环/内环坐标（上游 Polygon.exterior.coords / interiors）。
inline Polygon polygonCoords( GEOSContextHandle_t handle, const GEOSGeometry *polygon )
{
  Polygon out;
  if ( !polygon || GEOSGeomTypeId_r( handle, polygon ) != GEOS_POLYGON )
    return out;
  const GEOSGeometry *shell = GEOSGetExteriorRing_r( handle, polygon );
  out.exterior.points = lineCoords( handle, shell );
  const int holes = GEOSGetNumInteriorRings_r( handle, polygon );
  for ( int i = 0; i < holes; ++i )
  {
    Ring hole;
    hole.points = lineCoords( handle, GEOSGetInteriorRingN_r( handle, polygon, i ) );
    out.holes.push_back( std::move( hole ) );
  }
  return out;
}

inline double polylineLength( const std::vector<Point2> &points )
{
  double length = 0;
  for ( std::size_t i = 1; i < points.size(); ++i )
    length += std::hypot( points[i].x - points[i - 1].x, points[i].y - points[i - 1].y );
  return length;
}

inline double geosLength( GEOSContextHandle_t handle, const GEOSGeometry *geometry )
{
  double length = 0;
  if ( geometry )
    GEOSLength_r( handle, geometry, &length );
  return length;
}

inline double geosDistance( GEOSContextHandle_t handle, const GEOSGeometry *a,
                            const GEOSGeometry *b )
{
  double distance = std::numeric_limits<double>::infinity();
  if ( a && b )
    GEOSDistance_r( handle, a, b, &distance );
  return distance;
}

} // namespace paleo::singlefactor
