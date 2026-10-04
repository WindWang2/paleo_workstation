// 层：数据
#include "candidateboundaries.h"

#include "../singlefactor/geosutil.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace paleo::faciesmapping
{

using singlefactor::GeomPtr;
using singlefactor::GeosContext;

namespace
{

// 域环 → LineString 列表（polygonize/unaryUnion 输入要线，环按坐标重建）。
std::vector<GeomPtr> domainRingLines( const GeosContext &ctx, const GEOSGeometry *domain )
{
  std::vector<GeomPtr> out;
  if ( !domain )
    return out;
  std::vector<std::vector<Point2>> parts;
  singlefactor::collectLineParts( ctx.handle, domain, &parts );
  for ( const std::vector<Point2> &points : parts )
  {
    if ( points.size() < 2 )
      continue;
    GeomPtr line = singlefactor::makeLineString( ctx.handle, points );
    if ( line )
      out.push_back( std::move( line ) );
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

CandidateBoundaryResult extractCandidateRegions( const CandidateBoundaryRequest &request,
                                                 const Control *control )
{
  using singlefactor::makeGeom;
  using singlefactor::makeLineString;
  using singlefactor::polygonizeOf;
  using singlefactor::unaryUnionOf;

  CandidateBoundaryResult result;
  GeosContext ctx;

  // ---- 1) 域并集 + 等值线裁剪 -------------------------------------------
  GeomPtr domainUnion;
  if ( !request.domain.empty() )
    domainUnion = singlefactor::unionPolygons( ctx.handle, request.domain );
  const bool hasDomain = domainUnion != nullptr;

  // 裁剪后的等值线（GeomPtr 持有；directParts 拆段只借指针）。
  struct ClippedContour
  {
    std::string tag;
    GeomPtr geometry;
  };
  std::vector<ClippedContour> clippedContours;
  for ( const ContourLevelLines &level : request.contours )
  {
    for ( std::size_t segment = 0; segment < level.lines.size(); ++segment )
    {
      const std::vector<Point2> &points = level.lines[segment];
      if ( points.size() < 2 )
        continue;
      GeomPtr line = makeLineString( ctx.handle, points );
      if ( !line )
        continue;
      if ( hasDomain )
        line = makeGeom( ctx.handle,
                         GEOSIntersection_r( ctx.handle, line.get(), domainUnion.get() ) );
      if ( !line || GEOSisEmpty_r( ctx.handle, line.get() ) == 1 )
        continue;
      ClippedContour clipped;
      // 级别格式紧凑化（"5" 而非 "5.000000"）：证据标记进属性表，人读友好。
      std::ostringstream levelText;
      levelText << level.level;
      clipped.tag = "level:" + levelText.str() + "#" + std::to_string( segment );
      clipped.geometry = std::move( line );
      clippedContours.push_back( std::move( clipped ) );
    }
  }

  // ---- 2) 节点化 + 面化：域环 + 裁剪后等值线 ----------------------------
  std::vector<const GEOSGeometry *> arrangementInputs;
  GeomPtr domainBoundary; // 边界接触判定用（域环的 boundary）
  std::vector<GeomPtr> domainRings; // 具名持有：指针进 arrangementInputs 后须存活
  if ( hasDomain )
  {
    domainBoundary = makeGeom( ctx.handle, GEOSBoundary_r( ctx.handle, domainUnion.get() ) );
    domainRings = domainRingLines( ctx, domainBoundary.get() );
    for ( const GeomPtr &ring : domainRings )
      arrangementInputs.push_back( ring.get() );
  }
  for ( const ClippedContour &contour : clippedContours )
    for ( const GEOSGeometry *part : singlefactor::directParts( ctx.handle,
                                                                contour.geometry.get() ) )
      arrangementInputs.push_back( part );

  // 面化结果必须具名持有到函数尾：faceGeoms 借的是 faces 内部指针。
  std::vector<const GEOSGeometry *> faceGeoms;
  GeomPtr noded;
  GeomPtr faces;
  if ( !arrangementInputs.empty() )
  {
    noded = unaryUnionOf( ctx.handle, arrangementInputs );
    faces = noded ? polygonizeOf( ctx.handle,
                                  singlefactor::directParts( ctx.handle, noded.get() ) )
                  : GeomPtr();
    if ( faces )
      singlefactor::collectPolygons( ctx.handle, faces.get(), &faceGeoms );
  }

  // ---- 3) 约束区并集（一次构建，逐面求交） ------------------------------
  struct ZoneUnion
  {
    const FaciesZone *zone = nullptr;
    GeomPtr geometry;
  };
  std::vector<ZoneUnion> zoneUnions;
  for ( const FaciesZone &zone : request.zones )
  {
    if ( zone.polygons.empty() )
      continue;
    ZoneUnion entry;
    entry.zone = &zone;
    entry.geometry = singlefactor::unionPolygons( ctx.handle, zone.polygons );
    if ( entry.geometry )
      zoneUnions.push_back( std::move( entry ) );
  }

  // ---- 4) 面 → 候选单元 --------------------------------------------------
  int droppedSmall = 0;
  int zoneConflictFaces = 0;
  int regionIndex = 0;
  const double areaEpsilon = std::max( request.snapTolerance, 0.0 );
  for ( const GEOSGeometry *face : faceGeoms )
  {
    if ( control && control->cancelled && control->cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.regions.clear();
      return result;
    }
    const double faceArea = geosArea( ctx, face );
    if ( request.minArea > 0 && faceArea < request.minArea )
    {
      droppedSmall++;
      continue;
    }

    GeomPtr faceBoundary = makeGeom( ctx.handle, GEOSBoundary_r( ctx.handle, face ) );

    // 等值线证据：裁剪后线段与本面边界相交（内部悬空段不计入）。
    std::vector<std::string> contourEvidence;
    for ( const ClippedContour &contour : clippedContours )
    {
      for ( const GEOSGeometry *part : singlefactor::directParts( ctx.handle,
                                                                  contour.geometry.get() ) )
      {
        if ( faceBoundary &&
             GEOSIntersects_r( ctx.handle, faceBoundary.get(), part ) == 1 )
        {
          if ( std::find( contourEvidence.begin(), contourEvidence.end(), contour.tag ) ==
           contourEvidence.end() )
            contourEvidence.push_back( contour.tag );
          break; // 同一条线拆成的段共享一个 tag，命中即止
        }
      }
    }
    const bool touchesDomainEdge =
        hasDomain && domainBoundary &&
        GEOSIntersects_r( ctx.handle, faceBoundary.get(), domainBoundary.get() ) == 1;

    // 约束区归属：交集面积 > 0 的每个区各出一批单元（区重叠 → 双出，
    // 如实计数交 QA 检测器裁决，不在提取阶段静默合并）。
    int coveringZones = 0;
    std::vector<const GEOSGeometry *> coveringGeoms;
    for ( const ZoneUnion &zone : zoneUnions )
    {
      GeomPtr intersection =
          makeGeom( ctx.handle, GEOSIntersection_r( ctx.handle, face, zone.geometry.get() ) );
      if ( !intersection || geosArea( ctx, intersection.get() ) <= areaEpsilon )
        continue;
      coveringZones++;
      coveringGeoms.push_back( zone.geometry.get() );
      std::vector<const GEOSGeometry *> pieces;
      singlefactor::collectPolygons( ctx.handle, intersection.get(), &pieces );
      for ( const GEOSGeometry *piece : pieces )
      {
        const double pieceArea = geosArea( ctx, piece );
        if ( pieceArea <= areaEpsilon )
          continue;
        CandidateRegion region;
        region.regionId = "r" + std::to_string( regionIndex++ );
        region.faciesCode = zone.zone->faciesCode;
        region.geometry = singlefactor::polygonCoords( ctx.handle, piece );
        region.area = pieceArea;
        region.contourEvidence = contourEvidence;
        region.constraintEvidence = { zone.zone->id };
        region.touchesDomainEdge = touchesDomainEdge;
        result.regions.push_back( std::move( region ) );
      }
    }
    if ( coveringZones > 1 )
      zoneConflictFaces++;

    // 未覆盖残量：面减去全部覆盖区后仍 > 0 → 未定相单元（面积守恒，
    // 约束区之间的空隙不静默丢弃）。
    if ( coveringZones == 0 )
    {
      CandidateRegion region;
      region.regionId = "r" + std::to_string( regionIndex++ );
      region.faciesCode = -1; // 无约束证据 → 未定相，不造归属
      region.geometry = singlefactor::polygonCoords( ctx.handle, face );
      region.area = faceArea;
      region.contourEvidence = std::move( contourEvidence );
      region.touchesDomainEdge = touchesDomainEdge;
      result.regions.push_back( std::move( region ) );
    }
    else
    {
      GeomPtr coveringUnion = unaryUnionOf( ctx.handle, coveringGeoms );
      GeomPtr residual =
          coveringUnion ? makeGeom( ctx.handle,
                                    GEOSDifference_r( ctx.handle, face,
                                                      coveringUnion.get() ) )
                        : GeomPtr();
      if ( residual && geosArea( ctx, residual.get() ) > areaEpsilon )
      {
        std::vector<const GEOSGeometry *> pieces;
        singlefactor::collectPolygons( ctx.handle, residual.get(), &pieces );
        for ( const GEOSGeometry *piece : pieces )
        {
          const double pieceArea = geosArea( ctx, piece );
          if ( pieceArea <= areaEpsilon )
            continue;
          CandidateRegion region;
          region.regionId = "r" + std::to_string( regionIndex++ );
          region.faciesCode = -1;
          region.geometry = singlefactor::polygonCoords( ctx.handle, piece );
          region.area = pieceArea;
          region.contourEvidence = contourEvidence;
          region.touchesDomainEdge = touchesDomainEdge;
          result.regions.push_back( std::move( region ) );
        }
      }
    }
  }

  result.diagnostics.insert( QStringLiteral( "face_count" ),
                             static_cast<int>( faceGeoms.size() ) );
  result.diagnostics.insert( QStringLiteral( "region_count" ),
                             static_cast<int>( result.regions.size() ) );
  result.diagnostics.insert( QStringLiteral( "dropped_small_faces" ), droppedSmall );
  result.diagnostics.insert( QStringLiteral( "zone_conflict_faces" ), zoneConflictFaces );
  result.diagnostics.insert( QStringLiteral( "clipped_contour_lines" ),
                             static_cast<int>( clippedContours.size() ) );
  result.diagnostics.insert( QStringLiteral( "domain_present" ), hasDomain );
  if ( faceGeoms.empty() && !request.contours.empty() )
    result.message = "no closed faces from given contour lines and domain";
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::faciesmapping
