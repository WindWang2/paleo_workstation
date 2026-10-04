// 层：数据
#include "wellsiting.h"

#include "../singlefactor/support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace paleo::wellsiting
{

namespace
{

constexpr double kInf = std::numeric_limits<double>::infinity();

struct BBox
{
  double minX = kInf, minY = kInf, maxX = -kInf, maxY = -kInf;
  bool valid() const { return minX <= maxX && minY <= maxY; }
  void add( double x, double y )
  {
    minX = std::min( minX, x );
    minY = std::min( minY, y );
    maxX = std::max( maxX, x );
    maxY = std::max( maxY, y );
  }
};

// 均匀桶格点索引：环形扩张最近邻查询。井数典型在百级，桶距取
// bbox 对角线 / sqrt(n) 的量级，使每桶平均约 1 点。
class PointIndex
{
public:
  void build( const std::vector<Point2> &points )
  {
    m_pts = points;
    m_first.clear();
    m_next.assign( m_pts.size(), -1 );
    m_cols = m_rows = 0;
    m_h = 1;
    if ( m_pts.empty() )
      return;
    BBox box;
    for ( const Point2 &p : m_pts )
      box.add( p.x, p.y );
    const double diag = std::hypot( box.maxX - box.minX, box.maxY - box.minY );
    const double suggest = diag / std::sqrt( double( m_pts.size() ) );
    m_h = std::max( suggest, 1.0 );
    m_cols = int( ( box.maxX - box.minX ) / m_h ) + 1;
    m_rows = int( ( box.maxY - box.minY ) / m_h ) + 1;
    m_minX = box.minX;
    m_minY = box.minY;
    m_first.assign( std::size_t( m_cols ) * m_rows, -1 );
    for ( int i = int( m_pts.size() ) - 1; i >= 0; --i )
    {
      const int c = colOf( m_pts[std::size_t( i )].x );
      const int r = rowOf( m_pts[std::size_t( i )].y );
      const std::size_t slot = std::size_t( r ) * m_cols + c;
      m_next[std::size_t( i )] = m_first[slot];
      m_first[slot] = i;
    }
  }

  bool empty() const { return m_pts.empty(); }

  // 距最近点距离；exclude >= 0 时跳过该下标（最近邻统计用）。
  double nearestDistance( double x, double y, int exclude = -1 ) const
  {
    if ( m_pts.empty() )
      return kInf;
    const int cc = colOf( x ), cr = rowOf( y );
    double best = kInf;
    int r = 0;
    const int maxRing = m_cols + m_rows + 2; // 数值护栏：扩张不可能超过对角格数
    while ( true )
    {
      const int c0 = std::max( cc - r, 0 ), c1 = std::min( cc + r, m_cols - 1 );
      const int r0 = std::max( cr - r, 0 ), r1 = std::min( cr + r, m_rows - 1 );
      const bool offGrid = c0 == 0 && c1 == m_cols - 1 && r0 == 0 && r1 == m_rows - 1;
      for ( int row = r0; row <= r1; ++row )
        for ( int col = c0; col <= c1; ++col )
        {
          // 只扫环带（|Δrow| == r 或 |Δcol| == r 的格），内部环先前已扫。
          if ( r > 0 && std::abs( row - cr ) != r && std::abs( col - cc ) != r )
            continue;
          for ( int i = m_first[std::size_t( row ) * m_cols + col]; i >= 0;
                i = m_next[std::size_t( i )] )
          {
            if ( i == exclude )
              continue;
            const Point2 &p = m_pts[std::size_t( i )];
            best = std::min( best, std::hypot( p.x - x, p.y - y ) );
          }
        }
      // 扫完环 r 后，未扫的环（≥ r+1）任何点到查询点 ≥ r*h——best 不超
      // 过该下界才可停。注意必须先扫完环 r 再判：环 1 的点可以任意近
      //（查询点贴格边时下界是 0，不是 1*h）。
      if ( best <= r * m_h || offGrid || r >= maxRing )
        break;
      ++r;
    }
    return best;
  }

private:
  int colOf( double x ) const
  {
    return std::clamp( int( ( x - m_minX ) / m_h ), 0, m_cols - 1 );
  }
  int rowOf( double y ) const
  {
    return std::clamp( int( ( y - m_minY ) / m_h ), 0, m_rows - 1 );
  }

  std::vector<Point2> m_pts;
  std::vector<int> m_first, m_next;
  double m_minX = 0, m_minY = 0, m_h = 1;
  int m_cols = 0, m_rows = 0;
};

BBox domainBBox( const std::vector<Polygon> &domain )
{
  BBox box;
  for ( const Polygon &polygon : domain )
    for ( const Point2 &p : polygon.exterior.points )
      box.add( p.x, p.y );
  return box;
}

std::string buildNote( double cellSize )
{
  char buf[320];
  std::snprintf( buf, sizeof( buf ),
                 "欧氏距离近似（局部米制网格）；空洞面积/覆盖率/域面积为采样格口径"
                 "（cellSize=%.6gm，格中心采样）；边界多边形为格并集阶梯轮廓，仅用于"
                 "显示；本评估只衡量几何覆盖，不代表地质最优。",
                 cellSize );
  return buf;
}

void appendScenarioNote( std::string *note )
{
  if ( note )
    *note += " 候选井按「控制全部层位」假设计入层位井控密度。";
}

} // namespace

NearestNeighborStats nearestNeighborStats( const std::vector<Point2> &wells )
{
  NearestNeighborStats stats;
  stats.count = wells.size();
  if ( wells.size() < 2 )
    return stats;
  PointIndex index;
  index.build( wells );
  std::vector<double> dists;
  dists.reserve( wells.size() );
  double sum = 0;
  stats.min = kInf;
  stats.max = 0;
  for ( std::size_t i = 0; i < wells.size(); ++i )
  {
    const double d = index.nearestDistance( wells[i].x, wells[i].y, int( i ) );
    if ( !std::isfinite( d ) )
      continue; // 防御：不有限的不进分布
    dists.push_back( d );
    sum += d;
    stats.min = std::min( stats.min, d );
    stats.max = std::max( stats.max, d );
  }
  if ( dists.empty() )
    return stats;
  std::sort( dists.begin(), dists.end() );
  const std::size_t n = dists.size();
  stats.mean = sum / double( n );
  stats.median = n % 2 ? dists[n / 2] : ( dists[n / 2 - 1] + dists[n / 2] ) / 2;
  stats.p90 = dists[std::min( n - 1, std::size_t( std::ceil( 0.9 * double( n ) ) ) - 1 )];
  stats.valid = true;
  return stats;
}

SitingField sampleField( const std::vector<Polygon> &domain,
                         const std::vector<Point2> &wells, const SitingOptions &options )
{
  SitingField field;
  field.domain = domain;
  field.wells = wells;
  if ( domain.empty() )
  {
    field.message = "工区域为空";
    return field;
  }
  bool anyRing = false;
  for ( const Polygon &polygon : domain )
    if ( polygon.exterior.points.size() >= 3 )
      anyRing = true;
  if ( !anyRing )
  {
    field.message = "工区域多边形没有有效外环（至少 3 个点）";
    return field;
  }
  if ( !( options.controlRadius > 0 ) || !( options.cellSize > 0 ) )
  {
    field.message = "controlRadius 与 cellSize 必须为正";
    return field;
  }
  const BBox box = domainBBox( domain );
  const double w = box.maxX - box.minX, h = box.maxY - box.minY;
  if ( !( w > 0 ) || !( h > 0 ) )
  {
    field.message = "工区域包围盒退化";
    return field;
  }
  const int cols = int( std::ceil( w / options.cellSize ) );
  const int rows = int( std::ceil( h / options.cellSize ) );
  if ( std::uint64_t( cols ) * std::uint64_t( rows ) > options.maxCells )
  {
    char buf[192];
    std::snprintf( buf, sizeof( buf ), "采样预算超限：%dx%d 格 > %llu，请增大 cellSize",
                   cols, rows, static_cast<unsigned long long>( options.maxCells ) );
    field.status = Status::BudgetExceeded;
    field.message = buf;
    return field;
  }

  field.grid.cols = cols;
  field.grid.rows = rows;
  field.grid.originX = box.minX; // 左上像元边：north-up，行向南
  field.grid.originY = box.maxY;
  field.grid.pixelWidth = options.cellSize;
  field.grid.pixelHeight = -options.cellSize;
  field.cellArea = options.cellSize * options.cellSize;
  field.maxBoundaryCells = options.maxBoundaryCells;
  field.note = buildNote( options.cellSize );

  PointIndex index;
  index.build( wells );

  const std::size_t cellCount = std::size_t( cols ) * std::size_t( rows );
  field.dist.assign( cellCount, -1 );
  field.inDomain.assign( cellCount, 0 );
  std::size_t inDomainCount = 0;
  for ( int row = 0; row < rows; ++row )
    for ( int col = 0; col < cols; ++col )
    {
      const std::size_t cell = std::size_t( row ) * cols + col;
      const Point2 center = paleo::singlefactor::cellCenter( field.grid, col, row );
      if ( !paleo::singlefactor::pointInDomain( domain, center, 1e-9 ) )
        continue;
      field.inDomain[cell] = 1;
      ++inDomainCount;
      field.dist[cell] = index.nearestDistance( center.x, center.y );
    }
  field.domainArea = double( inDomainCount ) * field.cellArea;

  // 空洞连通域：域内且距最近井 > controlRadius；8 连通（角点相接视同
  // 一片空洞——更贴近「连续未控区」的直觉口径）。
  std::vector<std::uint8_t> visited( cellCount, 0 );
  std::vector<int> stack;
  for ( std::size_t seed = 0; seed < cellCount; ++seed )
  {
    if ( visited[seed] || !field.inDomain[seed] )
      continue;
    if ( !( field.dist[seed] > options.controlRadius ) )
      continue;
    visited[seed] = 1;
    stack.clear();
    stack.push_back( int( seed ) );
    std::vector<int> cells;
    while ( !stack.empty() )
    {
      const int cur = stack.back();
      stack.pop_back();
      cells.push_back( cur );
      const int cc = cur % cols, cr = cur / cols;
      for ( int dr = -1; dr <= 1; ++dr )
        for ( int dc = -1; dc <= 1; ++dc )
        {
          if ( dr == 0 && dc == 0 )
            continue;
          const int nr = cr + dr, nc = cc + dc;
          if ( nr < 0 || nr >= rows || nc < 0 || nc >= cols )
            continue;
          const std::size_t nb = std::size_t( nr ) * cols + nc;
          if ( visited[nb] || !field.inDomain[nb] )
            continue;
          if ( !( field.dist[nb] > options.controlRadius ) )
            continue;
          visited[nb] = 1;
          stack.push_back( int( nb ) );
        }
    }
    field.holes.push_back( std::move( cells ) );
  }
  std::sort( field.holes.begin(), field.holes.end(),
             []( const std::vector<int> &a, const std::vector<int> &b ) {
               return a.size() > b.size();
             } );
  field.status = Status::Ok;
  return field;
}

CoverageReport describeField( const SitingField &field, bool withBoundaries )
{
  CoverageReport report;
  report.status = field.status;
  report.message = field.message;
  report.note = field.note;
  if ( field.status != Status::Ok )
    return report;
  report.spacing = nearestNeighborStats( field.wells );

  // GEOS 上下文可选：量算全部来自格计数，边界只服务显示；评估路径
  // （withBoundaries=false）直接跳过提取。
  paleo::singlefactor::GeosContext geos;
  const bool haveGeos = withBoundaries && geos.handle != nullptr;
  std::size_t overBudgetRegions = 0;

  double domainSum = 0;
  bool allFinite = true;
  std::size_t domainCells = 0;
  for ( std::size_t i = 0; i < field.dist.size(); ++i )
  {
    if ( !field.inDomain[i] )
      continue;
    ++domainCells;
    const double d = field.dist[i];
    if ( std::isfinite( d ) )
      domainSum += d;
    else
      allFinite = false;
  }
  report.domainMeanDistance =
      allFinite && domainCells ? domainSum / double( domainCells ) : kInf;

  double holeArea = 0;
  report.regions.reserve( field.holes.size() );
  for ( std::size_t h = 0; h < field.holes.size(); ++h )
  {
    const std::vector<int> &cells = field.holes[h];
    HoleRegion region;
    region.id = int( h ) + 1;
    region.cellCount = int( cells.size() );
    region.area = double( cells.size() ) * field.cellArea;
    holeArea += region.area;
    double deepest = -1;
    for ( int cell : cells )
    {
      const double d = field.dist[std::size_t( cell )];
      if ( d > deepest )
      {
        deepest = d;
        region.maxDistance = d;
        region.deepest = paleo::singlefactor::cellCenter(
            field.grid, cell % field.grid.cols, cell / field.grid.cols );
      }
    }
    if ( haveGeos && field.maxBoundaryCells > 0 &&
         std::uint64_t( cells.size() ) > field.maxBoundaryCells )
    {
      ++overBudgetRegions; // 超预算不画轮廓——量算不受影响，note 如实注记
    }
    else if ( haveGeos )
    {
      // 边界 = 空洞格矩形并集的外环集合（阶梯轮廓）。8 连通域在角点
      // 相接时并集可为 MultiPolygon——如实逐成员返回外环。
      std::vector<const GEOSGeometry *> parts;
      std::vector<paleo::singlefactor::GeomPtr> keepAlive;
      parts.reserve( cells.size() );
      for ( int cell : cells )
      {
        const int col = cell % field.grid.cols, row = cell / field.grid.cols;
        const double x0 = field.grid.originX + col * field.grid.pixelWidth;
        const double y0 = field.grid.originY + row * field.grid.pixelHeight;
        const double cw = field.grid.pixelWidth, ch = -field.grid.pixelHeight;
        paleo::singlefactor::Polygon rect;
        // GEOS 线性环要求闭合（首尾同点）——矩形 4 角 + 回到起点。
        rect.exterior.points = { Point2{ x0, y0 }, Point2{ x0 + cw, y0 },
                                 Point2{ x0 + cw, y0 - ch }, Point2{ x0, y0 - ch },
                                 Point2{ x0, y0 } };
        keepAlive.push_back( paleo::singlefactor::makePolygon( geos.handle, rect ) );
        if ( keepAlive.back() )
          parts.push_back( keepAlive.back().get() );
      }
      if ( !parts.empty() )
      {
        paleo::singlefactor::GeomPtr merged =
            paleo::singlefactor::unaryUnionOf( geos.handle, parts );
        std::vector<const GEOSGeometry *> polys;
        paleo::singlefactor::collectPolygons( geos.handle, merged.get(), &polys );
        for ( const GEOSGeometry *poly : polys )
        {
          paleo::singlefactor::Polygon coords =
              paleo::singlefactor::polygonCoords( geos.handle, poly );
          if ( coords.exterior.points.size() >= 4 )
            region.boundaryParts.push_back( coords.exterior.points );
        }
      }
    }
    report.regions.push_back( std::move( region ) );
  }
  report.holeCount = int( field.holes.size() );
  report.holeAreaTotal = holeArea;
  report.coverageRatio = field.domainArea > 0 ? 1.0 - holeArea / field.domainArea : 0.0;
  if ( overBudgetRegions > 0 )
    report.note += " 超边界预算的 " + std::to_string( overBudgetRegions ) +
                   " 个空洞不画轮廓（面积量算不受影响）。";
  return report;
}

CoverageReport diagnoseCoverage( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const SitingOptions &options )
{
  const SitingField field = sampleField( domain, wells, options );
  return describeField( field );
}

// ---------------------------------------------------------------------------
// 候选点位生成
// ---------------------------------------------------------------------------

std::vector<CandidatePoint> generateCandidates( const SitingField &field,
                                                const AvoidSurfaces &avoid,
                                                const CandidateOptions &options,
                                                const SitingOptions &siting )
{
  std::vector<CandidatePoint> out;
  if ( field.status != Status::Ok )
    return out;
  const double spacing = options.gridSpacing > 0 ? options.gridSpacing : siting.controlRadius;
  if ( !( spacing > 0 ) )
    return out;
  const int step = std::max( 1, int( std::llround( spacing / siting.cellSize ) ) );

  PointIndex index;
  index.build( field.wells );

  const auto acceptable = [&]( const Point2 &p ) {
    if ( options.minWellDistance > 0 )
    {
      if ( index.nearestDistance( p.x, p.y ) < options.minWellDistance )
        return false;
    }
    if ( options.boundaryMargin > 0 )
    {
      // 距工区边界 ≈ 距各外环折线的最短距（域为多边形并集时即最近边界）。
      for ( const Polygon &polygon : field.domain )
        if ( paleo::singlefactor::distanceToPolyline( p, polygon.exterior.points ) <
             options.boundaryMargin )
          return false;
    }
    if ( avoid.lineBuffer > 0 )
    {
      for ( const std::vector<Point2> &line : avoid.lines )
        if ( paleo::singlefactor::distanceToPolyline( p, line ) < avoid.lineBuffer )
          return false;
    }
    for ( const Polygon &polygon : avoid.polygons )
    {
      if ( paleo::singlefactor::pointInDomain( { polygon }, p, 1e-9 ) )
        return false; // 多边形内部直接禁钻
      if ( avoid.polygonBuffer > 0 &&
           paleo::singlefactor::distanceToPolyline( p, polygon.exterior.points ) <
               avoid.polygonBuffer )
        return false;
    }
    return true;
  };

  for ( std::size_t h = 0; h < field.holes.size(); ++h )
  {
    const std::vector<int> &cells = field.holes[h];
    const int holeId = int( h ) + 1;
    double deepest = -1;
    Point2 deepestAt{};
    for ( int cell : cells )
    {
      const double d = field.dist[std::size_t( cell )];
      if ( d > deepest )
      {
        deepest = d;
        deepestAt = paleo::singlefactor::cellCenter(
            field.grid, cell % field.grid.cols, cell / field.grid.cols );
      }
    }
    if ( options.includeDeepest && acceptable( deepestAt ) )
      out.push_back( CandidatePoint{ deepestAt, "deepest", holeId, deepest } );

    // 规则网格点：锚定采样场原点，间距 spacing（取整到格步长 step）。
    std::vector<CandidatePoint> gridOnes;
    for ( int cell : cells )
    {
      const int col = cell % field.grid.cols, row = cell / field.grid.cols;
      if ( col % step != 0 || row % step != 0 )
        continue;
      const Point2 p = paleo::singlefactor::cellCenter( field.grid, col, row );
      if ( !acceptable( p ) )
        continue;
      gridOnes.push_back(
          CandidatePoint{ p, "grid", holeId, field.dist[std::size_t( cell )] } );
    }
    // 上限截断按空洞深度降序（保深不保先），与「最大空洞圆心优先」同口径。
    if ( options.perHoleLimit > 0 && int( gridOnes.size() ) > options.perHoleLimit )
    {
      std::sort( gridOnes.begin(), gridOnes.end(),
                 []( const CandidatePoint &a, const CandidatePoint &b ) {
                   return a.holeDistance > b.holeDistance;
                 } );
      gridOnes.resize( std::size_t( options.perHoleLimit ) );
    }
    for ( CandidatePoint &c : gridOnes )
      out.push_back( std::move( c ) );
  }
  return out;
}

// ---------------------------------------------------------------------------
// 方案评估
// ---------------------------------------------------------------------------

ScenarioMetrics scenarioMetrics( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const std::vector<Point2> &candidates,
                                 const std::vector<LayerWells> &layers,
                                 const SitingOptions &options )
{
  ScenarioMetrics metrics;
  // 与诊断同格采样（实井 + 候选一次到位），保证前后指标可比；评估只
  // 消费格计量——不提取 GEOS 边界（大工区省时）。
  std::vector<Point2> combined = wells;
  combined.insert( combined.end(), candidates.begin(), candidates.end() );
  const SitingField field = sampleField( domain, combined, options );
  if ( field.status != Status::Ok )
  {
    metrics.note = field.message;
    return metrics;
  }
  const CoverageReport report = describeField( field, /*withBoundaries=*/false );
  metrics.holeAreaTotal = report.holeAreaTotal;
  metrics.holeCount = report.holeCount;
  metrics.coverageRatio = report.coverageRatio;
  metrics.domainMeanDistance = report.domainMeanDistance;
  metrics.interWellMeanSpacing = report.spacing.valid ? report.spacing.mean : 0;
  metrics.note = report.note;

  // 按层位加权井控密度（口/km²，域面积取栅格口径）：密度 = 控制井数 /
  // 域面积；加权 = Σ w·d / Σ w。候选井假定控制全部层位。
  double weightSum = 0, acc = 0;
  for ( const LayerWells &layer : layers )
  {
    const double w = layer.weight > 0 ? layer.weight : 1;
    const double n = double( layer.points.size() + candidates.size() );
    acc += w * n;
    weightSum += w;
  }
  if ( weightSum > 0 && field.domainArea > 0 )
    metrics.weightedDensity = acc / weightSum / ( field.domainArea / 1.0e6 );
  if ( !candidates.empty() )
    appendScenarioNote( &metrics.note ); // 部署假设只对真有候选的方案注记
  return metrics;
}

ScenarioMetrics baselineMetrics( const std::vector<Polygon> &domain,
                                 const std::vector<Point2> &wells,
                                 const std::vector<LayerWells> &layers,
                                 const SitingOptions &options )
{
  return scenarioMetrics( domain, wells, {}, layers, options );
}

CandidateContribution contributionOf( const SitingField &field,
                                      const Point2 &candidate,
                                      const SitingOptions &options )
{
  CandidateContribution contribution;
  if ( field.status != Status::Ok )
    return contribution;
  std::size_t domainCells = 0;
  double distanceSumReduction = 0;
  bool finiteReduction = true;
  for ( std::size_t i = 0; i < field.dist.size(); ++i )
  {
    if ( !field.inDomain[i] )
      continue;
    ++domainCells;
    const double d = field.dist[i];
    const double dc = std::hypot( field.grid.originX +
                                      ( ( i % field.grid.cols ) + 0.5 ) * field.grid.pixelWidth -
                                      candidate.x,
                                  field.grid.originY +
                                      ( ( i / field.grid.cols ) + 0.5 ) * field.grid.pixelHeight -
                                      candidate.y );
    if ( dc < d )
    {
      distanceSumReduction += d - dc; // d 可为 +inf（无井）——减出仍 inf
      if ( !std::isfinite( d ) )
        finiteReduction = false;
      if ( d > options.controlRadius && dc <= options.controlRadius )
        contribution.holeAreaReduction += field.cellArea;
    }
  }
  if ( domainCells > 0 && finiteReduction )
    contribution.meanDistanceReduction = distanceSumReduction / double( domainCells );
  else
    contribution.meanDistanceReduction = kInf;
  return contribution;
}

} // namespace paleo::wellsiting
