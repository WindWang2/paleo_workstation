// 层：数据
#include "corridor.h"
#include "geosutil.h"

#include "support.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

constexpr double kCorridorInf = std::numeric_limits<double>::infinity();

void corridorUnitVector( double dx, double dy, double &ux, double &uy )
{
  const double length = std::hypot( dx, dy );
  if ( length <= 1e-15 )
  {
    ux = 1.0;
    uy = 0.0;
    return;
  }
  ux = dx / length;
  uy = dy / length;
}

std::string normalizedExtendMode( std::string mode )
{
  for ( char &ch : mode )
    if ( ch >= 'A' && ch <= 'Z' )
      ch = static_cast<char>( ch - 'A' + 'a' );
  if ( mode != "auto" && mode != "none" && mode != "tangent" )
    mode = "auto";
  return mode;
}

bool extendModeActive( const std::string &mode )
{
  return mode == "auto" || mode == "tangent";
}

// 上游 dual_angle_blend_tangent：以 2θ 向量混合两条切向，避免 0°/180° 相消。
struct BlendedTangent
{
  double tx = 1;
  double ty = 0;
  double g = 0;
  double ratio = 1;
};
BlendedTangent dualAngleBlendTangent( const PointCurveCoord &a, const PointCurveCoord &b )
{
  const double angA = std::atan2( a.ty, a.tx );
  const double angB = std::atan2( b.ty, b.tx );
  const double wa = std::max( a.g, 1e-9 );
  const double wb = std::max( b.g, 1e-9 );
  const double cx = wa * std::cos( 2.0 * angA ) + wb * std::cos( 2.0 * angB );
  const double cy = wa * std::sin( 2.0 * angA ) + wb * std::sin( 2.0 * angB );
  const double ang = 0.5 * std::atan2( cy, cx );
  BlendedTangent out;
  out.tx = std::cos( ang );
  out.ty = std::sin( ang );
  out.g = std::max( a.g, b.g );
  out.ratio = a.g >= b.g ? a.ratio : b.ratio;
  return out;
}

// 上游 _interp_profile_vectorized：常值端点外推的一维线性内插。
double interpProfile( double s, const std::vector<double> &ps, const std::vector<double> &pz )
{
  if ( ps.empty() )
    return std::numeric_limits<double>::quiet_NaN();
  if ( ps.size() == 1 )
    return pz.front();
  if ( s <= ps.front() )
    return pz.front();
  if ( s >= ps.back() )
    return pz.back();
  for ( std::size_t i = 1; i < ps.size(); ++i )
  {
    if ( ps[i - 1] <= s && s <= ps[i] )
    {
      const double span = ps[i] - ps[i - 1];
      if ( span <= 1e-12 )
        return pz[i - 1];
      const double t = std::clamp( ( s - ps[i - 1] ) / span, 0.0, 1.0 );
      return pz[i - 1] * ( 1.0 - t ) + pz[i] * t;
    }
  }
  double best = pz.front();
  double bestDist = std::abs( ps.front() - s );
  for ( std::size_t i = 1; i < ps.size(); ++i )
  {
    const double dist = std::abs( ps[i] - s );
    if ( dist < bestDist )
    {
      bestDist = dist;
      best = pz[i];
    }
  }
  return best;
}

} // namespace

std::vector<DirectionLineSpec> resolveDirectionParams( std::span<const DirectionLineSpec> specs,
    double searchRadius, double meanWellSpacing, double mapExtent )
{
  const double baseSearch = std::max( searchRadius, 1.0 );
  const double spacing = std::max( { meanWellSpacing, baseSearch * 0.15, 1.0 } );
  const double mapE = std::max( { mapExtent, baseSearch, 1.0 } );
  std::vector<DirectionLineSpec> resolved;
  resolved.reserve( specs.size() );
  for ( const DirectionLineSpec &spec : specs )
  {
    if ( !spec.active || spec.points.size() < 2 )
      continue;
    const double ratio = std::max( spec.ratio, 1.0 );
    const double lineLen = std::max( { polylineLength( spec.points ), spacing, 1.0 } );
    double autoCore = std::max( { baseSearch * 0.65, lineLen * 0.12, spacing * 1.05 } );
    autoCore = std::min( { autoCore, mapE * 0.16, lineLen * 0.20 } );
    double autoInfluence = std::max( { autoCore * 3.2, spacing * 3.2, baseSearch * 1.45, lineLen * 0.30 } );
    autoInfluence = std::min( { autoInfluence, mapE * 0.38, lineLen * 0.48 } );

    const bool influenceExplicit = spec.influenceRadius > 0.0;
    const bool coreExplicit = spec.coreRadius > 0.0;
    double influence = influenceExplicit ? spec.influenceRadius : autoInfluence;
    double core = coreExplicit ? spec.coreRadius : autoCore;
    if ( influenceExplicit && !coreExplicit )
      core = std::min( autoCore, std::max( influence * 0.55, influence * 0.4 ) );
    if ( core > influence )
    {
      if ( influenceExplicit && !coreExplicit )
        core = std::max( influence * 0.5, std::min( core, influence * 0.85 ) );
      else if ( influenceExplicit && coreExplicit )
        core = std::min( core, influence * 0.95 );
      else
        influence = std::max( influence, core * 1.05 );
    }
    core = std::max( std::min( core, influence * 0.99 ), 0.0 );
    influence = std::max( influence, core + 1e-6 );
    double transition = spec.transition;
    if ( transition <= 0.0 )
      transition = std::max( influence - core, std::max( core * 0.2, 1e-6 ) );

    DirectionLineSpec out = spec;
    out.active = true;
    out.ratio = ratio;
    out.influenceRadius = influence;
    out.coreRadius = core;
    out.priority = spec.priority > 0 ? static_cast<int>( spec.priority ) : 1;
    out.zoneId = spec.zoneId;
    out.extendMode = normalizedExtendMode( spec.extendMode );
    out.transition = transition;
    resolved.push_back( std::move( out ) );
  }
  return resolved;
}

PolylineGeometry buildPolylineGeometry( const DirectionLineSpec &spec, int index, double extendDistance )
{
  std::vector<Point2> cleaned;
  cleaned.reserve( spec.points.size() );
  if ( !spec.points.empty() )
    cleaned.push_back( spec.points.front() );
  for ( std::size_t i = 1; i < spec.points.size(); ++i )
  {
    if ( std::hypot( spec.points[i].x - cleaned.back().x, spec.points[i].y - cleaned.back().y ) > 1e-12 )
      cleaned.push_back( spec.points[i] );
  }

  PolylineGeometry geom;
  geom.lineId = spec.lineId;
  geom.ratio = spec.ratio;
  geom.coreRadius = spec.coreRadius;
  geom.influenceRadius = spec.influenceRadius;
  geom.priority = spec.priority;
  geom.zoneId = spec.zoneId;
  geom.extendMode = normalizedExtendMode( spec.extendMode );
  geom.transition = spec.transition;
  geom.index = index;

  if ( cleaned.size() < 2 )
  {
    geom.points = cleaned.empty() ? std::vector<Point2>{ Point2{}, Point2{} } : std::vector<Point2>{ cleaned.front(), cleaned.front() };
    geom.cumlen.assign( geom.points.size(), 0.0 );
    geom.sStart = 0;
    geom.sEnd = 0;
    return geom;
  }

  const std::string mode = normalizedExtendMode( spec.extendMode );
  const double extend = std::max( extendDistance, 0.0 );
  const bool extending = extendModeActive( mode ) && extend > 0.0;
  if ( extending )
  {
    double t0x = 0, t0y = 0, t1x = 0, t1y = 0;
    corridorUnitVector( cleaned[1].x - cleaned[0].x, cleaned[1].y - cleaned[0].y, t0x, t0y );
    corridorUnitVector( cleaned[cleaned.size() - 1].x - cleaned[cleaned.size() - 2].x,
                cleaned[cleaned.size() - 1].y - cleaned[cleaned.size() - 2].y, t1x, t1y );
    std::vector<Point2> chain;
    chain.reserve( cleaned.size() + 2 );
    chain.push_back( Point2{ cleaned[0].x - t0x * extend, cleaned[0].y - t0y * extend } );
    chain.insert( chain.end(), cleaned.begin(), cleaned.end() );
    chain.push_back( Point2{ cleaned.back().x + t1x * extend, cleaned.back().y + t1y * extend } );
    geom.points = std::move( chain );
    geom.sStart = extend;
  }
  else
  {
    geom.points = std::move( cleaned );
    geom.sStart = 0;
  }

  geom.cumlen.assign( geom.points.size(), 0.0 );
  for ( std::size_t i = 1; i < geom.points.size(); ++i )
    geom.cumlen[i] = geom.cumlen[i - 1] +
                     std::hypot( geom.points[i].x - geom.points[i - 1].x, geom.points[i].y - geom.points[i - 1].y );
  geom.totalLength = geom.cumlen.back();
  geom.sEnd = geom.totalLength - ( extending ? extend : 0.0 );
  return geom;
}

std::vector<PolylineGeometry> buildDirectionGeometries( std::span<const DirectionLineSpec> specs,
    double searchRadius, double meanWellSpacing, double mapExtent )
{
  const std::vector<DirectionLineSpec> resolved =
      resolveDirectionParams( specs, searchRadius, meanWellSpacing, mapExtent );
  std::vector<PolylineGeometry> geoms;
  geoms.reserve( resolved.size() );
  for ( std::size_t i = 0; i < resolved.size(); ++i )
  {
    const DirectionLineSpec &spec = resolved[i];
    const double a = std::max( spec.ratio, 1.0 );
    const double rBase = std::max( searchRadius, 1.0 );
    double extend = 0.0;
    if ( extendModeActive( normalizedExtendMode( spec.extendMode ) ) )
      extend = std::min( { a * rBase, spec.influenceRadius, std::max( mapExtent * 0.35, rBase ) } );
    geoms.push_back( buildPolylineGeometry( spec, static_cast<int>( i ), extend ) );
  }
  return geoms;
}

PolylineProjection projectPointToPolyline( Point2 point, const PolylineGeometry &geom )
{
  const double px = point.x;
  const double py = point.y;
  double bestDist = kCorridorInf;
  PolylineProjection best;
  best.distance = kCorridorInf;
  for ( std::size_t i = 0; i + 1 < geom.points.size(); ++i )
  {
    const double ax = geom.points[i].x;
    const double ay = geom.points[i].y;
    const double bx = geom.points[i + 1].x;
    const double by = geom.points[i + 1].y;
    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSq = dx * dx + dy * dy;
    if ( lengthSq <= 1e-24 )
      continue;
    const double t = std::clamp( ( ( px - ax ) * dx + ( py - ay ) * dy ) / lengthSq, 0.0, 1.0 );
    const double cx = ax + t * dx;
    const double cy = ay + t * dy;
    const double dist = std::hypot( px - cx, py - cy );
    if ( dist < bestDist )
    {
      bestDist = dist;
      const double length = std::sqrt( lengthSq );
      const double tx = dx / length;
      const double ty = dy / length;
      best.s = geom.cumlen[i] + t * length;
      best.n = ( px - cx ) * ( -ty ) + ( py - cy ) * tx;
      best.tx = tx;
      best.ty = ty;
      best.distance = dist;
    }
  }
  return best;
}

double influenceStrength( double distToLine, double coreRadius, double influenceRadius,
                          double transition, double expK )
{
  (void )transition;
  const double d = std::abs( distToLine );
  const double core = std::max( coreRadius, 0.0 );
  const double inf = std::max( influenceRadius, core + 1e-9 );
  if ( d <= core )
    return 1.0;
  if ( d >= inf )
    return 0.0;
  const double t = std::clamp( ( d - core ) / std::max( inf - core, 1e-9 ), 0.0, 1.0 );
  const double k = std::max( expK, 0.5 );
  const double eK = std::exp( -k );
  const double g = ( std::exp( -k * t ) - eK ) / std::max( 1.0 - eK, 1e-12 );
  return std::clamp( g, 0.0, 1.0 );
}

double alongTrackEnvelope( double s, double sStart, double sEnd, double tipLength,
                           const std::string &extendMode )
{
  const double s0 = std::min( sStart, sEnd );
  const double s1 = std::max( sStart, sEnd );
  if ( s0 - 1e-9 <= s && s <= s1 + 1e-9 )
    return 1.0;
  std::string mode = extendMode;
  for ( char &ch : mode )
    if ( ch >= 'A' && ch <= 'Z' )
      ch = static_cast<char>( ch - 'A' + 'a' );
  if ( mode.empty() )
    mode = "auto";
  if ( mode == "none" || mode == "off" || mode == "0" || mode == "false" )
    return 0.0;
  double tip = std::max( tipLength, 0.0 );
  if ( tip <= 1e-12 )
    tip = std::max( 0.1 * ( s1 - s0 ), 1e-6 );
  double t = 0;
  if ( s < s0 )
    t = ( s0 - s ) / tip;
  else
    t = ( s - s1 ) / tip;
  if ( t >= 1.0 )
    return 0.0;
  t = std::clamp( t, 0.0, 1.0 );
  return 1.0 - t * t * ( 3.0 - 2.0 * t );
}

double combinedInfluence( double distToLine, double s, const PolylineGeometry &geom, double tipLength )
{
  const double gPerp =
      influenceStrength( distToLine, geom.coreRadius, geom.influenceRadius, geom.transition );
  if ( gPerp <= 1e-12 )
    return 0.0;
  double tip = tipLength;
  if ( tip <= 0.0 )
    tip = std::max( { 0.12 * std::max( geom.sEnd - geom.sStart, 1.0 ), geom.coreRadius * 0.35, 1.0 } );
  const double gAlong = alongTrackEnvelope( s, geom.sStart, geom.sEnd, tip, geom.extendMode );
  return gPerp * gAlong;
}

DirectionFieldCache buildGridDirectionCache( const GridSpec &grid,
    const std::vector<std::uint8_t> &domainMask, const std::vector<PolylineGeometry> &geoms )
{
  const std::size_t cells = static_cast<std::size_t>( grid.cols ) * static_cast<std::size_t>( grid.rows );
  DirectionFieldCache cache;
  cache.cells = static_cast<int>( cells );
  cache.dirIndex.assign( cells, -1 );
  cache.s.assign( cells, 0.0 );
  cache.n.assign( cells, 0.0 );
  cache.tx.assign( cells, 0.0 );
  cache.ty.assign( cells, 0.0 );
  cache.g.assign( cells, 0.0 );
  cache.ratio.assign( cells, 1.0 );
  cache.stretch.assign( cells, 1.0 );
  cache.dirIndex2.assign( cells, -1 );
  cache.g2.assign( cells, 0.0 );
  cache.tx2.assign( cells, 0.0 );
  cache.ty2.assign( cells, 0.0 );
  cache.ratio2.assign( cells, 1.0 );
  cache.s2.assign( cells, 0.0 );
  cache.n2.assign( cells, 0.0 );
  if ( geoms.empty() || domainMask.size() != cells )
    return cache;

  const bool multiDir = geoms.size() >= 2;
  for ( int row = 0; row < grid.rows; ++row )
  {
    for ( int col = 0; col < grid.cols; ++col )
    {
      const std::size_t idx = static_cast<std::size_t>( row ) * static_cast<std::size_t>( grid.cols ) +
                              static_cast<std::size_t>( col );
      if ( !domainMask[idx] )
        continue;
      const Point2 p = cellCenter( grid, col, row );
      double bestScore = -1.0;
      PointCurveCoord best;
      PointCurveCoord second;
      double secondScore = -1.0;
      for ( const PolylineGeometry &geom : geoms )
      {
        const PolylineProjection proj = projectPointToPolyline( p, geom );
        const double g = combinedInfluence( proj.distance, proj.s, geom );
        if ( g <= 1e-9 )
          continue;
        const double prioBoost = 1.0 / std::max( static_cast<double>( geom.priority ), 1.0 );
        const double score =
            g * ( 1.0 + 0.15 * prioBoost ) / ( 1.0 + proj.distance / std::max( geom.influenceRadius, 1.0 ) );
        PointCurveCoord coord;
        coord.dirIndex = geom.index;
        coord.s = proj.s;
        coord.n = proj.n;
        coord.tx = proj.tx;
        coord.ty = proj.ty;
        coord.g = g;
        coord.ratio = geom.ratio;
        coord.distance = proj.distance;
        if ( score > bestScore )
        {
          second = best;
          secondScore = bestScore;
          best = coord;
          bestScore = score;
        }
        else if ( multiDir && score > secondScore )
        {
          second = coord;
          secondScore = score;
        }
      }
      if ( best.dirIndex < 0 )
        continue;
      cache.dirIndex[idx] = best.dirIndex;
      cache.s[idx] = best.s;
      cache.n[idx] = best.n;
      cache.tx[idx] = best.tx;
      cache.ty[idx] = best.ty;
      cache.g[idx] = best.g;
      cache.ratio[idx] = best.ratio;

      if ( multiDir && second.dirIndex >= 0 && second.g > 0.15 && best.g > 0.15 )
      {
        const PolylineGeometry &gA = geoms[static_cast<std::size_t>( best.dirIndex )];
        const PolylineGeometry &gB = geoms[static_cast<std::size_t>( second.dirIndex )];
        const bool zoneOk = gA.zoneId.empty() || gB.zoneId.empty() || gA.zoneId == gB.zoneId;
        const bool close = std::abs( bestScore - secondScore ) <= std::max( 0.15 * bestScore, 1e-6 );
        if ( zoneOk && close )
        {
          const BlendedTangent blended = dualAngleBlendTangent( best, second );
          cache.tx[idx] = blended.tx;
          cache.ty[idx] = blended.ty;
          cache.dirIndex2[idx] = second.dirIndex;
          cache.g2[idx] = second.g;
          cache.tx2[idx] = second.tx;
          cache.ty2[idx] = second.ty;
          cache.ratio2[idx] = second.ratio;
          cache.s2[idx] = second.s;
          cache.n2[idx] = second.n;
          cache.g[idx] = std::max( best.g, second.g * 0.85 );
          cache.ratio[idx] = blended.ratio;
        }
      }
    }
  }
  for ( std::size_t i = 0; i < cells; ++i )
    cache.stretch[i] = 1.0 + ( cache.ratio[i] - 1.0 ) * cache.g[i];
  return cache;
}

std::map<int, WellCurveTable> precomputeWellCurveCoords( std::span<const Point2> wellXy,
    const std::vector<PolylineGeometry> &geoms )
{
  const std::size_t n = wellXy.size();
  std::map<int, WellCurveTable> result;
  for ( const PolylineGeometry &geom : geoms )
  {
    WellCurveTable table;
    table.s.assign( n, 0.0 );
    table.n.assign( n, 0.0 );
    table.g.assign( n, 0.0 );
    table.tx.assign( n, 0.0 );
    table.ty.assign( n, 0.0 );
    table.valid.assign( n, std::uint8_t{ 0 } );
    for ( std::size_t i = 0; i < n; ++i )
    {
      const PolylineProjection proj = projectPointToPolyline( wellXy[i], geom );
      const double g = combinedInfluence( proj.distance, proj.s, geom );
      table.s[i] = proj.s;
      table.n[i] = proj.n;
      table.g[i] = g;
      table.tx[i] = proj.tx;
      table.ty[i] = proj.ty;
      table.valid[i] = g > 1e-9 ? std::uint8_t{ 1 } : std::uint8_t{ 0 };
    }
    table.ratio = geom.ratio;
    table.zoneId = geom.zoneId;
    table.lineLength = std::max( { geom.sEnd - geom.sStart, geom.totalLength, 0.0 } );
    result.emplace( geom.index, std::move( table ) );
  }
  return result;
}

PointCurveCoord pickControllingDirection( std::span<const PointCurveCoord> candidates,
    const std::vector<PolylineGeometry> &geoms )
{
  PointCurveCoord best;
  double bestScore = -1.0;
  for ( const PointCurveCoord &coord : candidates )
  {
    if ( coord.dirIndex < 0 )
      continue;
    if ( coord.dirIndex >= static_cast<int>( geoms.size() ) )
      continue;
    const PolylineGeometry &geom = geoms[static_cast<std::size_t>( coord.dirIndex )];
    const double prioBoost = 1.0 / std::max( static_cast<double>( geom.priority ), 1.0 );
    const double score = coord.g * ( 1.0 + 0.15 * prioBoost ) /
                         ( 1.0 + coord.distance / std::max( geom.influenceRadius, 1.0 ) );
    if ( score > bestScore )
    {
      bestScore = score;
      best = coord;
    }
  }
  return best;
}

double curveDistanceSq( double s0, double n0, double s1, double n1, double ratio, double expAniso )
{
  double a = std::max( ratio, 1.0 );
  if ( expAniso > 1.0 + 1e-9 )
    a = std::pow( a, expAniso );
  const double ds = ( s0 - s1 ) / a;
  const double dn = n0 - n1;
  return ds * ds + dn * dn;
}

double blendEffectiveDistance( double euclidean, double curveDist, double gPair )
{
  double g = std::clamp( gPair, 0.0, 1.0 );
  if ( g >= 0.25 )
    g = std::min( 1.0, 0.70 + 0.30 * g );
  const double de2 = euclidean * euclidean;
  const double dc2 = curveDist * curveDist;
  return std::sqrt( std::max( ( 1.0 - g ) * de2 + g * dc2, 0.0 ) );
}

bool ellipticalSearchAccept( double s0, double n0, double s1, double n1, double ratio,
                             double baseRadius, double gPair, double euclidean, double lineLength )
{
  const double r = std::max( baseRadius, 1e-9 );
  const double g = std::clamp( gPair, 0.0, 1.0 );
  if ( g <= 1e-9 )
    return euclidean <= r;
  const double a = std::max( ratio, 1.0 );
  double rPar = r * ( 1.0 + g * ( a - 1.0 ) * 1.15 );
  if ( lineLength > 0.0 )
    rPar = std::max( { rPar, g * lineLength * 1.20, lineLength * 1.05 * g } );
  rPar = std::max( rPar, r * a * std::max( g, 0.85 ) );
  const double rPerp = r;
  const double ds = std::abs( s0 - s1 );
  const double dn = std::abs( n0 - n1 );
  const double lhs = ( ds / std::max( rPar, 1e-9 ) ) * ( ds / std::max( rPar, 1e-9 ) ) +
                     ( dn / std::max( rPerp, 1e-9 ) ) * ( dn / std::max( rPerp, 1e-9 ) );
  if ( lhs <= 1.0 )
    return true;
  if ( g < 0.35 && euclidean <= r )
    return true;
  return false;
}

PairDistanceResult pairEffectiveDistance( double euclidean, int cellDir, double cellS, double cellN,
    double cellG, double cellRatio, std::size_t wellIndex,
    const std::map<int, WellCurveTable> &wellCoords )
{
  PairDistanceResult out;
  out.dEff = euclidean;
  out.gPair = 0.0;
  if ( cellDir < 0 || cellG <= 1e-9 )
    return out;
  const auto found = wellCoords.find( cellDir );
  if ( found == wellCoords.end() || wellIndex >= found->second.valid.size() ||
       !found->second.valid[wellIndex] )
    return out;
  const WellCurveTable &wc = found->second;
  const double gWell = wc.g[wellIndex];
  const double gPair = std::min( cellG, gWell );
  if ( gPair <= 1e-9 )
    return out;
  const double a = std::max( { cellRatio, wc.ratio, 1.0 } );
  const double dCurve = std::sqrt( std::max(
      curveDistanceSq( cellS, cellN, wc.s[wellIndex], wc.n[wellIndex], a ), 0.0 ) );
  out.dEff = blendEffectiveDistance( euclidean, dCurve, gPair );
  out.gPair = gPair;
  return out;
}

bool pairInSearchNeighborhood( double euclidean, double dEff, double gPair, double cellS, double cellN,
    double cellRatio, std::size_t wellIndex, int cellDir,
    const std::map<int, WellCurveTable> &wellCoords, double baseRadius, bool useExtendedSearch )
{
  const double r = std::max( baseRadius, 1e-9 );
  if ( gPair <= 1e-9 || cellDir < 0 )
    return ( !useExtendedSearch ? euclidean <= r : dEff <= r );
  const auto found = wellCoords.find( cellDir );
  if ( found == wellCoords.end() )
    return euclidean <= r;
  const WellCurveTable &wc = found->second;
  const double a = std::max( { cellRatio, wc.ratio, 1.0 } );
  if ( useExtendedSearch )
    return ellipticalSearchAccept( cellS, cellN, wc.s[wellIndex], wc.n[wellIndex], a, r, gPair,
                                   euclidean, wc.lineLength );
  return euclidean <= r;
}

std::map<int, RidgeProfile> buildAlongTrackWellProfiles( std::span<const Point2> wellXy,
    std::span<const double> wellValues, const std::map<int, WellCurveTable> &wellCoords,
    const std::vector<PolylineGeometry> &geoms, double minG )
{
  std::map<int, RidgeProfile> profiles;
  const std::size_t nWells = wellXy.size();
  for ( const PolylineGeometry &geom : geoms )
  {
    const auto found = wellCoords.find( geom.index );
    if ( found == wellCoords.end() )
      continue;
    const WellCurveTable &wc = found->second;
    std::vector<double> ss;
    std::vector<double> zz;
    for ( std::size_t i = 0; i < nWells; ++i )
    {
      if ( i >= wc.valid.size() || !wc.valid[i] )
        continue;
      if ( wc.g[i] < minG )
        continue;
      const double nLim = std::max(
          { std::min( geom.coreRadius * 0.46, geom.influenceRadius * 0.22 ), geom.coreRadius * 0.24, 1.0 } );
      if ( std::abs( wc.n[i] ) > nLim )
        continue;
      const double val = wellValues[i];
      if ( !std::isfinite( val ) )
        continue;
      ss.push_back( wc.s[i] );
      zz.push_back( val );
    }
    if ( ss.empty() )
      continue;

    std::vector<std::size_t> order( ss.size() );
    std::iota( order.begin(), order.end(), std::size_t{ 0 } );
    std::stable_sort( order.begin(), order.end(),
                      [&]( std::size_t a, std::size_t b ) { return ss[a] < ss[b]; } );
    std::vector<double> sArr( ss.size() );
    std::vector<double> zArr( zz.size() );
    for ( std::size_t k = 0; k < order.size(); ++k )
    {
      sArr[k] = ss[order[k]];
      zArr[k] = zz[order[k]];
    }

    std::vector<double> sMerged;
    std::vector<double> zMerged;
    double bucketS = sArr.front();
    std::vector<double> bucketZ{ zArr.front() };
    const double mergeEps = std::max( geom.coreRadius * 0.05, 1.0 );
    auto flushBucket = [&]()
    {
      std::vector<double> sorted = bucketZ;
      std::sort( sorted.begin(), sorted.end() );
      const double median = sorted.size() % 2 == 1
                                ? sorted[sorted.size() / 2]
                                : 0.5 * ( sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2] );
      sMerged.push_back( bucketS );
      zMerged.push_back( median );
    };
    for ( std::size_t k = 1; k < sArr.size(); ++k )
    {
      if ( std::abs( sArr[k] - bucketS ) <= mergeEps )
      {
        bucketZ.push_back( zArr[k] );
      }
      else
      {
        flushBucket();
        bucketS = sArr[k];
        bucketZ.assign( 1, zArr[k] );
      }
    }
    flushBucket();

    RidgeProfile profile;
    const double sTip0 = std::min( geom.sStart, geom.sEnd );
    const double sTip1 = std::max( geom.sStart, geom.sEnd );
    profile.s.push_back( sTip0 );
    profile.s.insert( profile.s.end(), sMerged.begin(), sMerged.end() );
    profile.s.push_back( sTip1 );
    profile.z.push_back( zMerged.front() );
    profile.z.insert( profile.z.end(), zMerged.begin(), zMerged.end() );
    profile.z.push_back( zMerged.back() );
    profiles.emplace( geom.index, std::move( profile ) );
  }
  return profiles;
}

double sampleAlongTrackValue( double s, double n, const RidgeProfile &profile,
                              const PolylineGeometry &geom )
{
  (void )n;
  (void )geom;
  return interpProfile( s, profile.s, profile.z );
}

std::vector<double> blendCorridorAlongTrack( const std::vector<double> &gridValues,
    const DirectionFieldCache &cache, const std::vector<PolylineGeometry> &geoms,
    const std::map<int, RidgeProfile> &profiles, const std::vector<std::uint8_t> &domainMask,
    AlongTrackStats *stats, double blendStrength, double minCellG, double expK )
{
  std::vector<double> out = gridValues;
  if ( stats )
  {
    stats->alongTrackCells = 0;
    stats->alongTrackProfiles = static_cast<int>( profiles.size() );
  }
  const std::size_t cells = static_cast<std::size_t>( cache.cells );
  if ( profiles.empty() || cells == 0 || out.size() != cells )
    return out;

  const double strength = std::clamp( blendStrength, 0.0, 1.0 );
  const double minG = minCellG;
  const double kExp = std::max( expK, 0.5 );

  std::map<int, const PolylineGeometry *> geomByIdx;
  for ( const PolylineGeometry &geom : geoms )
    geomByIdx.emplace( geom.index, &geom );

  const double edgeStart = std::max( minG * 0.10, 1e-6 );
  const double edgeFull = std::min( 1.0, std::max( minG * 4.0, 0.15 ) );

  for ( const auto &[di, profile] : profiles )
  {
    const auto foundGeom = geomByIdx.find( di );
    if ( foundGeom == geomByIdx.end() )
      continue;
    const PolylineGeometry &geom = *foundGeom->second;

    const double s0 = std::min( geom.sStart, geom.sEnd );
    const double s1 = std::max( geom.sStart, geom.sEnd );
    const double tip = std::max( { geom.coreRadius * 0.65, geom.influenceRadius * 0.20, 1.0 } );
    const double core = std::max( geom.coreRadius, 1e-9 );
    const double inf = std::max( geom.influenceRadius, core + 1e-9 );
    const double kPerp = std::max( kExp * 0.65, 2.0 );
    const double eK = std::exp( -kExp );
    const double eKPerp = std::exp( -kPerp );
    const double coreFadeEnd = std::min( inf, core + std::max( ( inf - core ) * 0.35, core * 0.35 ) );
    const double rawMax = 1.0 - std::exp( -kExp * strength );
    const bool tipActive = geom.extendMode != "none";

    for ( std::size_t i = 0; i < cells; ++i )
    {
      if ( i >= domainMask.size() || !domainMask[i] )
        continue;
      if ( cache.dirIndex[i] != di )
        continue;
      if ( !( cache.g[i] >= edgeStart ) )
        continue;

      const double sCell = cache.s[i];
      const double nCell = cache.n[i];
      const double gCell = cache.g[i];
      const double base = out[i];

      double edgeT = std::clamp( ( gCell - edgeStart ) / std::max( edgeFull - edgeStart, 1e-9 ), 0.0, 1.0 );
      edgeT = edgeT * edgeT * ( 3.0 - 2.0 * edgeT );

      double gAlong = 0.0;
      if ( sCell >= s0 - 1e-9 && sCell <= s1 + 1e-9 )
      {
        gAlong = 1.0;
      }
      else if ( tipActive )
      {
        const bool left = sCell < s0;
        const double t = left ? std::clamp( ( s0 - sCell ) / tip, 0.0, 1.0 )
                              : std::clamp( ( sCell - s1 ) / tip, 0.0, 1.0 );
        if ( t < 1.0 )
          gAlong = std::clamp( ( std::exp( -kExp * t ) - eK ) / std::max( 1.0 - eK, 1e-12 ), 0.0, 1.0 );
        else
          gAlong = 0.0;
      }

      const double nAbs = std::abs( nCell );
      double gPerp = 1.0;
      if ( nAbs >= inf )
      {
        gPerp = 0.0;
      }
      else if ( nAbs > core )
      {
        const double t = std::clamp( ( nAbs - core ) / std::max( inf - core, 1e-9 ), 0.0, 1.0 );
        gPerp = std::clamp( ( std::exp( -kPerp * t ) - eKPerp ) / std::max( 1.0 - eKPerp, 1e-12 ), 0.0, 1.0 );
      }

      const double axisW = std::max( 0.0, 1.0 - ( nAbs / ( core * 1.65 ) ) * ( nAbs / ( core * 1.65 ) ) );
      const bool use = gAlong > 1e-9 && gPerp > 1e-9;
      if ( !use )
        continue;

      const double vAlong = interpProfile( sCell, profile.s, profile.z );

      const double wLin = std::clamp( gCell * gAlong * ( 0.30 * axisW + 0.70 * gPerp ) * edgeT, 0.0, 1.0 );
      double alpha = std::clamp( ( 1.0 - std::exp( -kExp * strength * wLin ) ) /
                                     std::max( rawMax, 1e-12 ) * 0.995,
                                 0.0, 0.995 );
      const bool onSpan = sCell >= s0 - 1e-9 && sCell <= s1 + 1e-9;
      double coreT = std::clamp( ( nAbs - core ) / std::max( coreFadeEnd - core, 1e-9 ), 0.0, 1.0 );
      const double coreWeight = 1.0 - coreT * coreT * ( 3.0 - 2.0 * coreT );
      const double continuousFloor =
          strength * edgeT * gAlong * gPerp * ( 0.78 + 0.215 * coreWeight );
      if ( onSpan )
        alpha = std::max( alpha, continuousFloor );
      alpha = std::clamp( alpha, 0.0, 0.995 );

      if ( !std::isfinite( base ) )
      {
        if ( gPerp > 0.08 )
        {
          out[i] = vAlong;
          if ( stats )
            ++stats->alongTrackCells;
        }
      }
      else if ( alpha > 1e-6 )
      {
        out[i] = ( 1.0 - alpha ) * base + alpha * vAlong;
        if ( stats )
          ++stats->alongTrackCells;
      }
    }
  }
  return out;
}

} // namespace paleo::singlefactor
