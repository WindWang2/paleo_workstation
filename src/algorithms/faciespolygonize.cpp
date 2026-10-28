// 层：数据
#include "paleoalgorithms.h"
#include "faciessliver.h"

#include <qgsabstractgeometry.h>
#include <qgscurve.h>
#include <qgslinestring.h>
#include <qgsexception.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgscoordinatetransform.h>
#include <qgsgeometry.h>
#include <qgsgeometrycollection.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingparameters.h>
#include <qgsrasterlayer.h>
#include <qgswkbtypes.h>

#include <gdal.h>
#include <gdal_alg.h>
#include <cpl_conv.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

// §36 — classified raster to an editable facies-polygon coverage.
//
// SMOOTH majority passes (3×3 mode filter, nodata-preserving) remove
// salt-and-pepper noise; parts smaller than MIN_CELLS cells, smaller than
// MIN_FRACTION of the valid cells, or under MIN_AREA map units² are then
// absorbed on the grid (4-connected components are the parts polygonize
// would emit). Shared edges are then simplified once
// via GEOS coverage VW and rebuilt from a boundary graph, so adjacent faces
// cannot drift apart. Conflation replaces an arc only when every vertex lies
// inside SNAP_TOLERANCE and the arc is parallel to the constraint.

namespace
{

constexpr int kNodata = -9999;
constexpr double kPi = 3.14159265358979323846;

struct CodedPoly
{
  QgsGeometry geom;
  int code = 0;
};

struct Node
{
  QgsPointXY p;
};

struct Arc
{
  QVector<int> ix;
  bool exterior = false; // 有效区外圈/NoData 边界固定，避免切角缩小覆盖范围
};

struct OrientedArc
{
  int arc = -1;
  bool rev = false;
};

struct RingRef
{
  QVector<OrientedArc> seq;
};

struct FaceGraph
{
  int code = 0;
  RingRef shell;
  QVector<RingRef> holes;
};

struct NKey
{
  qint64 x = 0;
  qint64 y = 0;
  bool operator==( const NKey &o ) const { return x == o.x && y == o.y; }
};

struct EKey
{
  int a = 0;
  int b = 0;
  static EKey make( int u, int v )
  {
    if ( u > v )
      std::swap( u, v );
    return { u, v };
  }
  bool operator==( const EKey &o ) const { return a == o.a && b == o.b; }
};

inline size_t qHash( const NKey &k, size_t seed = 0 ) noexcept
{
  return qHashMulti( seed, k.x, k.y );
}

inline size_t qHash( const EKey &k, size_t seed = 0 ) noexcept
{
  return qHashMulti( seed, k.a, k.b );
}

double dist2( const QgsPointXY &a, const QgsPointXY &b )
{
  const double dx = a.x() - b.x();
  const double dy = a.y() - b.y();
  return dx * dx + dy * dy;
}

double undirectedDiffDeg( double a, double b )
{
  return std::fabs( std::remainder( a - b, kPi ) ) * 180.0 / kPi;
}

void canceled( QgsProcessingFeedback *feedback )
{
  if ( feedback && feedback->isCanceled() )
    throw QgsProcessingException( QStringLiteral( "Canceled" ) );
}

// 3×3 majority (mode) filter: each cell adopts the modal code of its 3×3
// neighborhood, self included. Ties keep the current code (no oscillation);
// nodata cells never change and never cast votes, so holes cannot swallow
// facies and facies cannot bleed into holes. Returns changed cells.
int smoothMajority( QVector<int> &grid, int cols, int rows )
{
  QVector<int> next( grid );
  int changed = 0;
  QHash<int, int> counts;
  for ( int y = 0; y < rows; ++y )
  {
    for ( int x = 0; x < cols; ++x )
    {
      const int i = y * cols + x;
      if ( grid.at( i ) == kNodata )
        continue;
      counts.clear();
      for ( int dy = -1; dy <= 1; ++dy )
      {
        for ( int dx = -1; dx <= 1; ++dx )
        {
          const int nx = x + dx, ny = y + dy;
          if ( nx < 0 || ny < 0 || nx >= cols || ny >= rows )
            continue;
          const int v = grid.at( ny * cols + nx );
          if ( v != kNodata )
            ++counts[v];
        }
      }
      // FB-001：并列裁决与 GeologicalSmoothing 同口径——最高计数胜；并列
      // 保持中心码；挑战者之间并列取数值更小的码。原实现的「先遍历到者胜」
      // 由 QHash 迭代序决定（Qt 6.8 每进程随机种子、应用未钉种子），同一
      // 输入栅格跨进程会产出不同图斑/PROVENANCE——与本文件边界图/合链
      // 两处的键排序钉死同一纪律。
      const int center = grid.at( i );
      int best = center;
      int bestCount = counts.value( center, 0 );
      for ( auto it = counts.cbegin(); it != counts.cend(); ++it )
      {
        if ( it.value() > bestCount ||
             ( it.value() == bestCount && best != center && it.key() < best ) )
        {
          best = it.key();
          bestCount = it.value();
        }
      }
      next[i] = best;
      if ( best != grid.at( i ) )
        ++changed;
    }
  }
  std::swap( grid, next );
  return changed;
}

void collectPolygons( const QgsGeometry &g, int code, QVector<CodedPoly> &out )
{
  if ( g.isEmpty() )
    return;
  const Qgis::WkbType wkb = g.wkbType();
  if ( QgsWkbTypes::geometryType( wkb ) != Qgis::GeometryType::Polygon )
    return;
  if ( QgsWkbTypes::isMultiType( wkb ) )
  {
    const QVector<QgsGeometry> parts = g.asGeometryCollection();
    for ( const QgsGeometry &part : parts )
      collectPolygons( part, code, out );
    return;
  }
  out.append( CodedPoly{ g, code } );
}

QVector<CodedPoly> simplifyCoverage( const QVector<CodedPoly> &polys, double tolerance )
{
  if ( !( tolerance > 0.0 ) || polys.isEmpty() )
    return polys;
  QVector<QgsGeometry> geoms;
  geoms.reserve( polys.size() );
  for ( const CodedPoly &p : polys )
    geoms.append( p.geom );
  const QgsGeometry coll = QgsGeometry::collectGeometry( geoms );
  QgsGeometry simplified;
  try
  {
    simplified = coll.simplifyCoverageVW( tolerance, true );
  }
  catch ( const QgsException &e )
  {
    throw QgsProcessingException(
        QStringLiteral( "coverage simplify failed: %1" ).arg( e.what() ) );
  }
  if ( simplified.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "coverage simplify returned an empty geometry" ) );

  QVector<QgsGeometry> parts;
  if ( polys.size() == 1 &&
       QgsWkbTypes::geometryType( simplified.wkbType() ) == Qgis::GeometryType::Polygon &&
       !QgsWkbTypes::isMultiType( simplified.wkbType() ) )
  {
    parts.append( simplified );
  }
  else
  {
    parts = simplified.asGeometryCollection();
  }
  if ( parts.size() != polys.size() )
  {
    throw QgsProcessingException(
        QStringLiteral( "coverage simplify changed the polygon count from %1 to %2" )
            .arg( polys.size() )
            .arg( parts.size() ) );
  }

  QVector<bool> used( polys.size(), false );
  QVector<CodedPoly> out;
  out.reserve( parts.size() );
  for ( const QgsGeometry &s : parts )
  {
    int best = -1;
    double bestArea = 0.0;
    for ( int i = 0; i < polys.size(); ++i )
    {
      if ( used.at( i ) )
        continue;
      const double area = s.intersection( polys.at( i ).geom ).area();
      if ( area > bestArea )
      {
        bestArea = area;
        best = i;
      }
    }
    if ( best < 0 )
    {
      throw QgsProcessingException(
          QStringLiteral( "coverage simplify produced a polygon that overlaps no input face" ) );
    }
    used[best] = true;
    out.append( CodedPoly{ s, polys.at( best ).code } );
  }
  return out;
}

struct Graph
{
  QVector<Node> nodes;
  QVector<Arc> arcs;
  QVector<FaceGraph> faces;
  double qTol = 1e-8;
};

bool arcGoes( const Arc &arc, int u, int v )
{
  for ( int i = 0; i + 1 < arc.ix.size(); ++i )
  {
    if ( arc.ix.at( i ) == u && arc.ix.at( i + 1 ) == v )
      return true;
  }
  return false;
}

Graph buildBoundaryGraph( const QVector<CodedPoly> &polys, double qTol )
{
  struct DirEdge
  {
    int u = -1;
    int v = -1;
    int neighbor = -1;
  };
  struct RingBuild
  {
    QVector<DirEdge> edges;
  };
  struct FaceBuild
  {
    int code = 0;
    QVector<RingBuild> rings;
  };

  Graph g;
  g.qTol = qTol;
  QHash<NKey, int> nodeIndex;
  auto nodeFor = [&]( const QgsPointXY &p ) -> int {
    const NKey key{ std::llround( p.x() / qTol ), std::llround( p.y() / qTol ) };
    const auto it = nodeIndex.constFind( key );
    if ( it != nodeIndex.constEnd() )
      return it.value();
    const int id = g.nodes.size();
    g.nodes.append( Node{ p } );
    nodeIndex.insert( key, id );
    return id;
  };

  QVector<FaceBuild> builds;
  builds.reserve( polys.size() );
  for ( const CodedPoly &poly : polys )
  {
    const QgsPolygonXY rings = poly.geom.asPolygon();
    if ( rings.isEmpty() )
    {
      throw QgsProcessingException(
          QStringLiteral( "facies polygon is not a simple polygon (wkb %1)" )
              .arg( static_cast<int>( poly.geom.wkbType() ) ) );
    }
    FaceBuild face;
    face.code = poly.code;
    for ( const QgsPolylineXY &ring : rings )
    {
      if ( ring.size() < 4 )
        throw QgsProcessingException( QStringLiteral( "facies ring has fewer than 3 vertices" ) );
      RingBuild rb;
      const int n = ring.size();
      for ( int i = 0; i + 1 < n; ++i )
      {
        const int u = nodeFor( ring.at( i ) );
        const int v = nodeFor( ring.at( i + 1 ) );
        if ( u == v )
          continue;
        rb.edges.append( DirEdge{ u, v, -1 } );
      }
      if ( rb.edges.isEmpty() )
        throw QgsProcessingException( QStringLiteral( "facies ring collapsed while noding" ) );
      face.rings.append( rb );
    }
    if ( face.rings.isEmpty() )
      throw QgsProcessingException( QStringLiteral( "facies polygon has no rings" ) );
    builds.append( face );
  }

  struct Use
  {
    int face = -1;
    int ring = -1;
    int edge = -1;
  };
  QHash<EKey, QVector<Use>> uses;
  for ( int f = 0; f < builds.size(); ++f )
  {
    for ( int r = 0; r < builds.at( f ).rings.size(); ++r )
    {
      const QVector<DirEdge> &edges = builds.at( f ).rings.at( r ).edges;
      for ( int e = 0; e < edges.size(); ++e )
        uses[EKey::make( edges.at( e ).u, edges.at( e ).v )].append( Use{ f, r, e } );
    }
  }
  for ( auto it = uses.constBegin(); it != uses.constEnd(); ++it )
  {
    if ( it.value().size() > 2 )
      throw QgsProcessingException( QStringLiteral( "non-manifold facies boundary" ) );
  }
  for ( int f = 0; f < builds.size(); ++f )
  {
    for ( int r = 0; r < builds[f].rings.size(); ++r )
    {
      QVector<DirEdge> &edges = builds[f].rings[r].edges;
      for ( int e = 0; e < edges.size(); ++e )
      {
        const QVector<Use> &u = uses.value( EKey::make( edges.at( e ).u, edges.at( e ).v ) );
        for ( const Use &o : u )
        {
          if ( o.face != f )
          {
            edges[e].neighbor = o.face;
            break;
          }
        }
      }
    }
  }

  QHash<EKey, int> edgeToArc;
  // 两侧环的起点可能落在同一共享链的不同位置。所有环起点及拓扑分叉点
  // 都须切断弧，否则另一侧从弧中途进入时会引用整弧，重建出自交的大背景面。
  QSet<int> junctions;
  QVector<int> degree(g.nodes.size(), 0);
  for (auto it = uses.cbegin(); it != uses.cend(); ++it) {
    ++degree[it.key().a]; ++degree[it.key().b];
  }
  for (int i = 0; i < degree.size(); ++i)
    if (degree[i] != 2) junctions.insert(i);
  for (const auto &face : builds)
    for (const auto &ring : face.rings)
      junctions.insert(ring.edges.first().u);
  for ( const FaceBuild &face : builds )
  {
    FaceGraph fg;
    fg.code = face.code;
    for ( int r = 0; r < face.rings.size(); ++r )
    {
      const QVector<DirEdge> &edges = face.rings.at( r ).edges;
      RingRef ref;
      int i = 0;
      int guard = 0;
      while ( i < edges.size() )
      {
        if ( ++guard > edges.size() + 2 )
          throw QgsProcessingException( QStringLiteral( "boundary ring did not progress" ) );
        const EKey key = EKey::make( edges.at( i ).u, edges.at( i ).v );
        if ( edgeToArc.contains( key ) )
        {
          const int arcId = edgeToArc.value( key );
          const bool rev = !arcGoes( g.arcs.at( arcId ), edges.at( i ).u, edges.at( i ).v );
          int j = i;
          while ( j + 1 < edges.size() &&
                  edgeToArc.value( EKey::make( edges.at( j + 1 ).u, edges.at( j + 1 ).v ), -2 ) == arcId )
            ++j;
          ref.seq.append( OrientedArc{ arcId, rev } );
          i = j + 1;
        }
        else
        {
          const int neigh = edges.at( i ).neighbor;
          int j = i;
          while ( j + 1 < edges.size() && edges.at( j + 1 ).neighbor == neigh &&
                  !junctions.contains(edges.at(j).v) &&
                  !edgeToArc.contains( EKey::make( edges.at( j + 1 ).u, edges.at( j + 1 ).v ) ) )
            ++j;
          Arc arc;
          arc.exterior = neigh == -1;
          arc.ix.append( edges.at( i ).u );
          const int arcId = g.arcs.size();
          for ( int t = i; t <= j; ++t )
          {
            if ( arc.ix.isEmpty() || arc.ix.last() != edges.at( t ).v )
              arc.ix.append( edges.at( t ).v );
            edgeToArc.insert( EKey::make( edges.at( t ).u, edges.at( t ).v ), arcId );
          }
          if ( arc.ix.size() < 2 )
            throw QgsProcessingException( QStringLiteral( "boundary arc has fewer than 2 nodes" ) );
          g.arcs.append( arc );
          ref.seq.append( OrientedArc{ arcId, false } );
          i = j + 1;
        }
      }
      if ( ref.seq.isEmpty() )
        throw QgsProcessingException( QStringLiteral( "boundary ring produced no arcs" ) );
      if ( r == 0 )
        fg.shell = ref;
      else
        fg.holes.append( ref );
    }
    g.faces.append( fg );
  }
  return g;
}

void collectConstraints( const QgsGeometry &g, QVector<QgsGeometry> &lines, QVector<QgsPointXY> &points )
{
  if ( g.isEmpty() )
    return;
  const Qgis::WkbType wkb = g.wkbType();
  if ( QgsWkbTypes::isMultiType( wkb ) || wkb == Qgis::WkbType::GeometryCollection )
  {
    const QVector<QgsGeometry> parts = g.asGeometryCollection();
    for ( const QgsGeometry &part : parts )
      collectConstraints( part, lines, points );
    return;
  }
  const Qgis::GeometryType type = QgsWkbTypes::geometryType( wkb );
  if ( type == Qgis::GeometryType::Point )
  {
    points.append( g.asPoint() );
    return;
  }
  if ( type == Qgis::GeometryType::Line )
  {
    lines.append( g );
    return;
  }
  if ( type == Qgis::GeometryType::Polygon )
  {
    const QgsPolygonXY poly = g.asPolygon();
    for ( const QgsPolylineXY &ring : poly )
    {
      if ( ring.size() >= 2 )
        lines.append( QgsGeometry::fromPolylineXY( ring ) );
    }
  }
}

QgsPolylineXY arcPoints( const Graph &g, const OrientedArc &oa )
{
  QgsPolylineXY pts;
  const Arc &arc = g.arcs.at( oa.arc );
  pts.reserve( arc.ix.size() );
  for ( int id : arc.ix )
    pts.append( g.nodes.at( id ).p );
  if ( oa.rev )
    std::reverse( pts.begin(), pts.end() );
  return pts;
}

// 弧级 Chaikin 切角平滑：共享边界在图里只有一份弧，两邻面重建出同一
// 折线，coverage 天然不破。开放弧端点（图斑连接点）钉死，只切内部顶点；
// 闭合弧（孤岛/孔洞环）按环整体切角。直弧切角位移为零则跳过，不徒增顶点。
// 返回平滑过的弧数。
int smoothArcs( Graph &g, int iterations )
{
  if ( iterations <= 0 )
    return 0;
  const double eps2 = g.qTol * g.qTol;
  int changed = 0;
  for ( Arc &arc : g.arcs )
  {
    if (arc.exterior) continue;
    QgsPolylineXY pts;
    pts.reserve( arc.ix.size() );
    for ( const int id : arc.ix )
      pts.append( g.nodes.at( id ).p );
    const bool closed = arc.ix.size() > 2 && arc.ix.first() == arc.ix.last();
    if ( closed )
      pts.removeLast(); // 环：尾点与首点重合，去掉避免重复切角
    if ( pts.size() < 3 )
      continue;

    QgsPolylineXY cur = pts;
    // PW-091：每个平滑点记来源原始线段号，偏差只对其邻域线段求距（O(m)）。
    QVector<int> origin( cur.size() );
    for ( int i = 0; i < origin.size(); ++i )
      origin[i] = closed ? i : std::min( i, static_cast<int>( pts.size() ) - 2 );
    for ( int it = 0; it < iterations; ++it )
    {
      QgsPolylineXY next;
      QVector<int> nextOrigin;
      nextOrigin.reserve( 2 * cur.size() );
      if ( closed )
      {
        const int m = cur.size();
        next.reserve( 2 * m );
        for ( int i = 0; i < m; ++i )
        {
          const QgsPointXY &a = cur.at( i );
          const QgsPointXY &b = cur.at( ( i + 1 ) % m );
          next.append( QgsPointXY( 0.75 * a.x() + 0.25 * b.x(),
                                   0.75 * a.y() + 0.25 * b.y() ) );
          next.append( QgsPointXY( 0.25 * a.x() + 0.75 * b.x(),
                                   0.25 * a.y() + 0.75 * b.y() ) );
          nextOrigin.append( origin.at( i ) );
          nextOrigin.append( origin.at( i ) );
        }
      }
      else
      {
        next.reserve( 2 * cur.size() - 2 );
        next.append( cur.first() );
        nextOrigin.append( origin.first() );
        for ( int i = 0; i + 1 < cur.size(); ++i )
        {
          const QgsPointXY &a = cur.at( i );
          const QgsPointXY &b = cur.at( i + 1 );
          next.append( QgsPointXY( 0.75 * a.x() + 0.25 * b.x(),
                                   0.75 * a.y() + 0.25 * b.y() ) );
          next.append( QgsPointXY( 0.25 * a.x() + 0.75 * b.x(),
                                   0.25 * a.y() + 0.75 * b.y() ) );
          nextOrigin.append( origin.at( i ) );
          nextOrigin.append( origin.at( i ) );
        }
        next.append( cur.last() );
        nextOrigin.append( origin.last() );
      }
      cur = next;
      origin = nextOrigin;
    }

    // 位移量化阈值以内 = 全直弧，写回只徒增顶点。
    // PW-091：原实现每点对整条弧做 GEOS distance，O(32·m²)。Chaikin 点总落在
    // 来源线段附近，只对来源 ±kWin 段求点-线段距离：子集最小距离 ≥ 真最小距离，
    // 是偏差的上界——误判只会多保留顶点，不会丢形状。
    const int nSeg = closed ? pts.size() : pts.size() - 1;
    constexpr int kWin = 3;
    auto segDist2 = [&]( const QgsPointXY &p, int sIdx ) {
      const QgsPointXY &a = pts.at( sIdx );
      const QgsPointXY &b = pts.at( ( sIdx + 1 ) % pts.size() );
      const double vx = b.x() - a.x(), vy = b.y() - a.y();
      const double len2 = vx * vx + vy * vy;
      double t = len2 > 0.0 ? ( ( p.x() - a.x() ) * vx + ( p.y() - a.y() ) * vy ) / len2 : 0.0;
      t = std::clamp( t, 0.0, 1.0 );
      const double dx = p.x() - ( a.x() + t * vx ), dy = p.y() - ( a.y() + t * vy );
      return dx * dx + dy * dy;
    };
    bool deviates = false;
    for ( int q = 0; q < cur.size() && !deviates; ++q )
    {
      double best = std::numeric_limits<double>::infinity();
      for ( int w = -kWin; w <= kWin; ++w )
      {
        int sIdx = origin.at( q ) + w;
        if ( closed )
          sIdx = ( ( sIdx % nSeg ) + nSeg ) % nSeg;
        else if ( sIdx < 0 || sIdx >= nSeg )
          continue;
        best = std::min( best, segDist2( cur.at( q ), sIdx ) );
      }
      deviates = best > eps2;
    }
    if ( !deviates )
      continue;

    // 平滑后自相交则回退原弧（不写回）。
    {
      QgsPolylineXY test = cur;
      if ( closed )
        test.append( cur.first() );
      if ( !QgsGeometry::fromPolylineXY( test ).isSimple() )
        continue;
    }

    QVector<int> ix;
    if ( closed )
    {
      for ( const QgsPointXY &p : cur )
      {
        g.nodes.append( Node{ p } );
        ix.append( g.nodes.size() - 1 );
      }
      ix.append( ix.first() );
    }
    else
    {
      ix.append( arc.ix.first() );
      for ( int i = 1; i + 1 < cur.size(); ++i )
      {
        g.nodes.append( Node{ cur.at( i ) } );
        ix.append( g.nodes.size() - 1 );
      }
      ix.append( arc.ix.last() );
    }
    arc.ix = ix;
    ++changed;
  }
  if ( changed == 0 )
    return 0;

  // 被替换掉的旧内部节点压缩掉，保持图整洁（conflate 的点吸附遍历全表）。
  QVector<int> remap( g.nodes.size(), -1 );
  QVector<Node> nodes;
  nodes.reserve( g.nodes.size() );
  for ( Arc &arc : g.arcs )
    for ( int &id : arc.ix )
    {
      int &r = remap[id];
      if ( r < 0 )
      {
        r = nodes.size();
        nodes.append( g.nodes.at( id ) );
      }
      id = r;
    }
  g.nodes = nodes;
  return changed;
}

QgsPolylineXY buildRing( const Graph &g, const RingRef &ring )
{
  QgsPolylineXY out;
  const double tol2 = g.qTol * g.qTol;
  for ( const OrientedArc &oa : ring.seq )
  {
    QgsPolylineXY pts = arcPoints( g, oa );
    if ( pts.isEmpty() )
      continue;
    if ( !out.isEmpty() && dist2( out.last(), pts.first() ) <= tol2 )
      pts.removeFirst();
    for ( const QgsPointXY &p : pts )
    {
      if ( !out.isEmpty() && dist2( out.last(), p ) <= tol2 )
        continue;
      out.append( p );
    }
  }
  if ( out.size() >= 2 && dist2( out.first(), out.last() ) > tol2 )
    out.append( out.first() );
  else if ( !out.isEmpty() )
    out.last() = out.first();
  return out;
}

QVector<CodedPoly> rebuildFaces( const Graph &g )
{
  QVector<CodedPoly> out;
  out.reserve( g.faces.size() );
  for ( const FaceGraph &face : g.faces )
  {
    const QgsPolylineXY shell = buildRing( g, face.shell );
    if ( shell.size() < 4 )
      throw QgsProcessingException( QStringLiteral( "rebuilt facies ring collapsed" ) );
    QgsPolygonXY poly;
    poly.append( shell );
    for ( const RingRef &hole : face.holes )
    {
      const QgsPolylineXY h = buildRing( g, hole );
      if ( h.size() >= 4 )
        poly.append( h );
    }
    QgsGeometry geom = QgsGeometry::fromPolygonXY( poly );
    if ( geom.isEmpty() )
      throw QgsProcessingException( QStringLiteral( "rebuilt facies polygon is empty" ) );
    out.append( CodedPoly{ geom, face.code } );
  }
  return out;
}

int conflateArcs( Graph &g, const QVector<QgsGeometry> &lines, const QVector<QgsPointXY> &points,
                  double snapTol, double angleTolDeg )
{
  if ( !( snapTol > 0.0 ) )
    return 0;
  const double snap2 = snapTol * snapTol;
  int changed = 0;

  // M1：外圈/NoData 边界（arc.exterior）承诺固定（见 Arc 注释与 SIMPLIFY 说明），
  // 旧实现吸附时照样替换外圈弧、移动外圈节点，使覆盖范围被切缩/外扩，
  // checkFootprint 随即整次失败。外圈弧不替换；外圈上的节点只允许沿外圈
  // 直线段滑动（两侧外圈邻点与本点共线、目标落在两邻点连线段上）——此时
  // 覆盖范围不变（如栅格外边上的内部分界端点）；否则不动。
  std::vector<std::vector<int>> exteriorNbrs( std::size_t( g.nodes.size() ) );
  for ( const Arc &arc : g.arcs )
  {
    if ( !arc.exterior )
      continue;
    for ( int i = 0; i < arc.ix.size(); ++i )
    {
      auto &nb = exteriorNbrs[std::size_t( arc.ix[i] )];
      for ( int j : { i - 1, i + 1 } )
        if ( j >= 0 && j < arc.ix.size() && arc.ix[j] != arc.ix[i] &&
             std::find( nb.begin(), nb.end(), arc.ix[j] ) == nb.end() )
          nb.push_back( arc.ix[j] );
    }
  }
  const auto isPinned = [&]( int id ) {
    return id >= 0 && std::size_t( id ) < exteriorNbrs.size() && !exteriorNbrs[std::size_t( id )].empty();
  };
  const auto canMove = [&]( int id, const QgsPointXY &target ) {
    if ( !isPinned( id ) )
      return true;
    const auto &nb = exteriorNbrs[std::size_t( id )];
    if ( nb.size() != 2 )
      return false;
    const QgsPointXY &pa = g.nodes[nb[0]].p, &pb = g.nodes[nb[1]].p, &pc = g.nodes[id].p;
    const double ux = pb.x() - pa.x(), uy = pb.y() - pa.y();
    const double len2 = ux * ux + uy * uy;
    if ( !( len2 > 0 ) )
      return false;
    const double eps = 1e-9 * std::sqrt( len2 );
    const auto offLine = [&]( const QgsPointXY &q ) {
      return std::fabs( ux * ( q.y() - pa.y() ) - uy * ( q.x() - pa.x() ) ) / std::sqrt( len2 );
    };
    if ( offLine( pc ) > eps || offLine( target ) > eps )
      return false;
    const double t = ( ( target.x() - pa.x() ) * ux + ( target.y() - pa.y() ) * uy ) / len2;
    return t > 0.0 && t < 1.0;
  };

  auto moveNode = [&]( int id, const QgsPointXY &target ) {
    if ( canMove( id, target ) && dist2( g.nodes[id].p, target ) <= snap2 )
      g.nodes[id].p = target;
  };

  for ( Arc &arc : g.arcs )
  {
    if ( arc.exterior )
      continue;
    if ( arc.ix.size() < 2 )
      continue;
    const int first = arc.ix.first();
    const int last = arc.ix.last();
    if ( first == last )
      continue; // a closed island outline is not a replaceable span
    const QgsPointXY a = g.nodes.at( first ).p;
    const QgsPointXY b = g.nodes.at( last ).p;
    if ( dist2( a, b ) <= 1e-18 )
      continue;
    const double arcAngle = std::atan2( b.y() - a.y(), b.x() - a.x() );

    QgsPolylineXY current;
    current.reserve( arc.ix.size() );
    for ( int id : arc.ix )
      current.append( g.nodes.at( id ).p );
    const QgsGeometry *best = nullptr;
    double bestMax = snapTol;
    for ( const QgsGeometry &line : lines )
    {
      double maxD = 0.0;
      for ( const QgsPointXY &p : current )
        maxD = std::max( maxD, line.distance( QgsGeometry::fromPointXY( p ) ) );
      if ( maxD > snapTol || maxD > bestMax )
        continue;
      const QgsPointXY mid = current.at( current.size() / 2 );
      const double along = line.lineLocatePoint( QgsGeometry::fromPointXY( mid ) );
      if ( along < 0.0 )
        continue;
      // R3-10：切向探针步长相对化（旧式固定 ±1e-3 在地理/大坐标下要么远小于
      // 双精度分辨率、要么跨过折点）。取 snapTol 的 1%，并不超过线长的 1%。
      const double probe = std::max( 1e-12, std::min( 0.01 * snapTol, 0.01 * line.length() ) );
      const QgsGeometry p0 = line.interpolate( std::max( 0.0, along - probe ) );
      const QgsGeometry p1 = line.interpolate( along + probe );
      if ( p0.isEmpty() || p1.isEmpty() )
        continue;
      const QgsPointXY q0 = p0.asPoint();
      const QgsPointXY q1 = p1.asPoint();
      const double cang = std::atan2( q1.y() - q0.y(), q1.x() - q0.x() );
      if ( undirectedDiffDeg( arcAngle, cang ) > angleTolDeg )
        continue;
      best = &line;
      bestMax = maxD;
    }
    if ( !best )
      continue;

    const double da = best->lineLocatePoint( QgsGeometry::fromPointXY( a ) );
    const double db = best->lineLocatePoint( QgsGeometry::fromPointXY( b ) );
    if ( da < 0.0 || db < 0.0 )
      continue;
    const QgsCurve *curve = qgsgeometry_cast<const QgsCurve *>( best->constGet() );
    if ( !curve )
      continue;
    std::unique_ptr<QgsCurve> sub( curve->curveSubstring( std::min( da, db ), std::max( da, db ) ) );
    if ( !sub )
      continue;
    std::unique_ptr<QgsAbstractGeometry> line( sub->curveToLine() );
    QgsGeometry subGeom( std::move( line ) );
    QgsPolylineXY sp = subGeom.asPolyline();
    if ( sp.size() < 2 )
      continue;
    if ( da > db )
      std::reverse( sp.begin(), sp.end() );
    if ( dist2( sp.first(), sp.last() ) <= 1e-18 )
      continue;

    if ( dist2( a, sp.first() ) <= snap2 && canMove( first, sp.first() ) )
      moveNode( first, sp.first() );
    else
      sp.first() = g.nodes.at( first ).p;
    if ( dist2( b, sp.last() ) <= snap2 && canMove( last, sp.last() ) )
      moveNode( last, sp.last() );
    else
      sp.last() = g.nodes.at( last ).p;

    QVector<int> ix;
    ix.append( first );
    for ( int i = 1; i + 1 < sp.size(); ++i )
    {
      g.nodes.append( Node{ sp.at( i ) } );
      ix.append( g.nodes.size() - 1 );
    }
    ix.append( last );
    arc.ix = ix;
    ++changed;
  }

  for ( const QgsPointXY &pt : points )
  {
    int best = -1;
    double bestD = snap2;
    for ( int i = 0; i < g.nodes.size(); ++i )
    {
      const double d = dist2( g.nodes.at( i ).p, pt );
      if ( d <= bestD )
      {
        bestD = d;
        best = i;
      }
    }
    if ( best < 0 || !canMove( best, pt ) )
      continue;
    bool collapses = false;
    for ( const Arc &arc : g.arcs )
    {
      for ( int i = 0; i < arc.ix.size(); ++i )
      {
        if ( arc.ix.at( i ) != best )
          continue;
        if ( i > 0 && dist2( g.nodes.at( arc.ix.at( i - 1 ) ).p, pt ) <= 1e-18 )
          collapses = true;
        if ( i + 1 < arc.ix.size() && dist2( g.nodes.at( arc.ix.at( i + 1 ) ).p, pt ) <= 1e-18 )
          collapses = true;
      }
    }
    if ( collapses )
      continue;
    g.nodes[best].p = pt;
    ++changed;
  }
  return changed;
}

// GEOS coverage checks require identical edge vertices, not merely collinear
// overlap. A T-junction leaves one long edge against two pieces; noding splits
// that edge so both faces share the same arc.
QVector<CodedPoly> nodeSharedEdges( const QVector<CodedPoly> &polys )
{
  QVector<QgsGeometry> lines;
  for ( const CodedPoly &poly : polys )
  {
    if (!poly.geom.isGeosValid())
      throw QgsProcessingException(QStringLiteral("invalid facies %1 before boundary noding; refusing to drop coverage").arg(poly.code));
    const QgsPolygonXY rings = poly.geom.asPolygon();
    if ( rings.isEmpty() )
    {
      throw QgsProcessingException(
          QStringLiteral( "cannot node a non-polygon facies part (wkb %1)" )
              .arg( static_cast<int>( poly.geom.wkbType() ) ) );
    }
    for ( const QgsPolylineXY &ring : rings )
    {
      if ( ring.size() >= 2 )
        lines.append( QgsGeometry::fromPolylineXY( ring ) );
    }
  }
  if ( lines.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "facies polygons have no boundary lines" ) );

  const QgsGeometry noded = QgsGeometry::unaryUnion( lines );
  if ( noded.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "failed to node facies boundaries" ) );
  const QgsGeometry rebuilt = QgsGeometry::polygonize( { noded } );
  if ( rebuilt.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "failed to rebuild polygons from noded boundaries" ) );

  QVector<QgsGeometry> pieces;
  if ( QgsWkbTypes::geometryType( rebuilt.wkbType() ) == Qgis::GeometryType::Polygon &&
       !QgsWkbTypes::isMultiType( rebuilt.wkbType() ) )
  {
    pieces.append( rebuilt );
  }
  else
  {
    pieces = rebuilt.asGeometryCollection();
  }

  QHash<int, QVector<QgsGeometry>> byCode;
  for ( const QgsGeometry &piece : pieces )
  {
    if ( piece.isEmpty() || QgsWkbTypes::geometryType( piece.wkbType() ) != Qgis::GeometryType::Polygon )
      continue;
    int code = 0;
    double bestArea = 0.0;
    bool found = false;
    for ( const CodedPoly &src : polys )
    {
      const double area = piece.intersection( src.geom ).area();
      if ( area > bestArea )
      {
        bestArea = area;
        code = src.code;
        found = true;
      }
    }
    if ( !found || !( bestArea > 0.0 ) )
      continue;
    byCode[code].append( piece );
  }

  QVector<CodedPoly> out;
  // 成果要素序必须确定性：QHash 迭代序随进程哈希种子漂移，同输入两次
  // 转面会得到不同图斑序（fid 归属随之漂移）。
  auto codeOrder = byCode.keys();
  std::sort( codeOrder.begin(), codeOrder.end() );
  for ( const int code : codeOrder )
  {
    const QVector<QgsGeometry> &geoms = byCode[code];
    const QgsGeometry merged = geoms.size() == 1 ? geoms.at( 0 )
                                                  : QgsGeometry::unaryUnion( geoms );
    if ( merged.isEmpty() )
      throw QgsProcessingException( QStringLiteral( "failed to dissolve noded facies %1" ).arg( code ) );
    collectPolygons( merged, code, out );
  }
  if ( out.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "noded boundaries produced no facies polygons" ) );
  return out;
}

QgsGeometry coverageFootprint(const QVector<CodedPoly> &polys)
{
  QVector<QgsGeometry> geometries;
  for (const auto &poly : polys) geometries.append(poly.geom);
  auto footprint = QgsGeometry::unaryUnion(geometries);
  if (footprint.isEmpty() || !footprint.lastError().isEmpty())
    throw QgsProcessingException(QStringLiteral("cannot determine facies coverage footprint: %1").arg(footprint.lastError()));
  return footprint;
}

double checkFootprint(const QgsGeometry &expected, const QgsGeometry &actual, double cellArea)
{
  const auto difference = expected.symDifference(actual);
  if (!difference.lastError().isEmpty())
    throw QgsProcessingException(QStringLiteral("facies footprint comparison failed: %1").arg(difference.lastError()));
  const double changed = difference.area();
  const double tolerance = std::max(cellArea * 1e-6, expected.area() * 1e-10);
  if (!std::isfinite(changed) || changed > tolerance)
    throw QgsProcessingException(QStringLiteral("facies coverage changed by %1 map units squared (expected %2, actual %3); refusing incomplete output")
        .arg(changed, 0, 'g', 12).arg(expected.area(), 0, 'g', 12).arg(actual.area(), 0, 'g', 12));
  return changed;
}

void checkCoverage( const QVector<CodedPoly> &polys, double gapWidth )
{
  if ( polys.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "facies polygonize produced no polygons" ) );
  QVector<QgsGeometry> geoms;
  geoms.reserve( polys.size() );
  for ( const CodedPoly &p : polys )
    geoms.append( p.geom );
  const QgsGeometry coll = QgsGeometry::collectGeometry( geoms );
  QgsGeometry invalid;
  Qgis::CoverageValidityResult validity = Qgis::CoverageValidityResult::Error;
  try
  {
    validity = coll.validateCoverage( gapWidth, &invalid );
  }
  catch ( const QgsException &e )
  {
    throw QgsProcessingException(
        QStringLiteral( "coverage validation failed: %1" ).arg( e.what() ) );
  }
  if ( validity != Qgis::CoverageValidityResult::Valid )
  {
    throw QgsProcessingException(
        QStringLiteral( "facies polygons do not form a gap-free coverage (%1) %2" )
            .arg( static_cast<int>( validity ) )
            .arg( invalid.isEmpty() ? QString() : invalid.asWkt().left( 240 ) ) );
  }
}

QVector<CodedPoly> polygonizeGrid( const QVector<int> &grid, int cols, int rows,
                                   const double gt[6], const QString &wkt )
{
  GDALDriverH memDrv = GDALGetDriverByName( "MEM" );
  if ( !memDrv )
    throw QgsProcessingException( QStringLiteral( "GDAL MEM driver is not available" ) );
  GDALDatasetH mem = GDALCreate( memDrv, "", cols, rows, 1, GDT_Int32, nullptr );
  if ( !mem )
    throw QgsProcessingException( QStringLiteral( "cannot create in-memory classified raster" ) );
  GDALSetGeoTransform( mem, const_cast<double *>( gt ) );
  if ( !wkt.isEmpty() )
    GDALSetProjection( mem, wkt.toUtf8().constData() );
  GDALRasterBandH band = GDALGetRasterBand( mem, 1 );
  GDALSetRasterNoDataValue( band, kNodata );
  if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<int *>( grid.constData() ),
                     cols, rows, GDT_Int32, 0, 0 ) != CE_None )
  {
    GDALClose( mem );
    throw QgsProcessingException( QStringLiteral( "failed to write classified raster" ) );
  }
  if ( GDALCreateMaskBand( band, 0 ) != CE_None )
  {
    GDALClose( mem );
    throw QgsProcessingException( QStringLiteral( "failed to create polygonize mask" ) );
  }
  GDALRasterBandH mask = GDALGetMaskBand( band );
  QVector<quint8> maskPx( grid.size() );
  bool any = false;
  for ( int i = 0; i < grid.size(); ++i )
  {
    maskPx[i] = grid.at( i ) == kNodata ? 0 : 255;
    any = any || maskPx[i] != 0;
  }
  if ( !any )
  {
    GDALClose( mem );
    throw QgsProcessingException( QStringLiteral( "raster has no classified cells" ) );
  }
  if ( GDALRasterIO( mask, GF_Write, 0, 0, cols, rows, maskPx.data(), cols, rows, GDT_Byte, 0, 0 ) != CE_None )
  {
    GDALClose( mem );
    throw QgsProcessingException( QStringLiteral( "failed to write polygonize mask" ) );
  }

  OGRSpatialReferenceH srs = nullptr;
  if ( !wkt.isEmpty() )
  {
    srs = OSRNewSpatialReference( nullptr );
    QByteArray wkb = wkt.toUtf8();
    char *ptr = wkb.data();
    if ( OSRImportFromWkt( srs, &ptr ) != OGRERR_NONE )
    {
      OSRDestroySpatialReference( srs );
      srs = nullptr;
    }
    else
    {
      OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
    }
  }
  OGRSFDriverH ogrDrv = OGRGetDriverByName( "Memory" );
  OGRDataSourceH ods = ogrDrv ? OGR_Dr_CreateDataSource( ogrDrv, "mem", nullptr ) : nullptr;
  if ( !ods )
  {
    if ( srs )
      OSRDestroySpatialReference( srs );
    GDALClose( mem );
    throw QgsProcessingException( QStringLiteral( "cannot create memory polygon layer" ) );
  }
  OGRLayerH layer = OGR_DS_CreateLayer( ods, "polygonized", srs, wkbPolygon, nullptr );
  OGRFieldDefnH fld = OGR_Fld_Create( "value", OFTInteger );
  OGR_L_CreateField( layer, fld, TRUE );
  OGR_Fld_Destroy( fld );
  const CPLErr perr = GDALPolygonize( band, mask, layer, 0, nullptr, nullptr, nullptr );
  GDALClose( mem );
  if ( perr != CE_None )
  {
    OGR_DS_Destroy( ods );
    if ( srs )
      OSRDestroySpatialReference( srs );
    throw QgsProcessingException( QStringLiteral( "GDALPolygonize failed: %1" )
                                      .arg( QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }

  QHash<int, QVector<QgsGeometry>> byCode;
  OGR_L_ResetReading( layer );
  while ( OGRFeatureH feat = OGR_L_GetNextFeature( layer ) )
  {
    const int code = OGR_F_GetFieldAsInteger( feat, 0 );
    OGRGeometryH og = OGR_F_GetGeometryRef( feat );
    if ( og )
    {
      char *wktPtr = nullptr;
      if ( OGR_G_ExportToWkt( og, &wktPtr ) == OGRERR_NONE && wktPtr )
      {
        QgsGeometry geom = QgsGeometry::fromWkt( QString::fromUtf8( wktPtr ) );
        CPLFree( wktPtr );
        if ( !geom.isEmpty() )
          byCode[code].append( geom );
      }
    }
    OGR_F_Destroy( feat );
  }
  OGR_DS_Destroy( ods );
  if ( srs )
    OSRDestroySpatialReference( srs );

  QVector<CodedPoly> polys;
  // 成果要素序必须确定性：QHash 迭代序随进程哈希种子漂移，同输入两次
  // 转面会得到不同图斑序（fid 归属随之漂移）。
  auto codeOrder = byCode.keys();
  std::sort( codeOrder.begin(), codeOrder.end() );
  for ( const int code : codeOrder )
  {
    const QVector<QgsGeometry> &geoms = byCode[code];
    QgsGeometry merged = geoms.size() == 1 ? geoms.at( 0 )
                                            : QgsGeometry::unaryUnion( geoms );
    if ( merged.isEmpty() )
      throw QgsProcessingException( QStringLiteral( "dissolve failed for facies %1" ).arg( code ) );
    collectPolygons( merged, code, polys );
  }
  if ( polys.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "polygonize produced no facies polygons" ) );
  return polys;
}

// 临时产物清理（含 GPKG 的 -wal/-shm 伴生文件）。
void removeGpkgStaging( const QString &staging )
{
  QFile::remove( staging );
  QFile::remove( staging + QStringLiteral( "-wal" ) );
  QFile::remove( staging + QStringLiteral( "-shm" ) );
}

// 写成功后把临时产物换成正式产物。旧产物先改名 .bak，staging rename 成功后再
// 删 .bak；rename 失败时把 .bak 改回原名——不能先 remove(正式) 再 rename，
// 那个窗口内新旧两失（同 factorcontour #74 / geostatrasterio #73 口径）。
void commitStagingGpkg( const QString &staging, const QString &path )
{
  const QString backup = path + QStringLiteral( ".bak" );
  QFile::remove( backup );
  const bool hadOutput = QFile::exists( path );
  if ( hadOutput && !QFile::rename( path, backup ) )
  {
    removeGpkgStaging( staging );
    throw QgsProcessingException( QStringLiteral( "cannot move existing output aside: %1" )
                                      .arg( path ) );
  }
  if ( !QFile::rename( staging, path ) )
  {
    if ( hadOutput )
      QFile::rename( backup, path ); // 回滚：旧产物还原
    removeGpkgStaging( staging );
    throw QgsProcessingException( QStringLiteral( "cannot move facies polygons into place: %1" )
                                      .arg( path ) );
  }
  QFile::remove( backup );
  removeGpkgStaging( backup );
}

void writeGpkg( const QString &path, const QVector<CodedPoly> &polys, const QString &wkt,
                const QString &provenance, const QHash<int, std::pair<double, bool>> valueBounds[2] = nullptr )
{
  // 先写同目录临时路径，全部成功后才替换正式产物。旧式在正式路径上直接
  // GDALCreate（先删旧文件），WKT 编码失败/CreateFeature 失败只 GDALClose 后
  // 抛异常——OUTPUT 留下截断的半成品 GPKG，而此前的旧输出已被删（部分写 +
  // 失败后状态残留）。
  const QString staging = path + QStringLiteral( ".tmp" );
  removeGpkgStaging( staging );
  GDALDriverH drv = GDALGetDriverByName( "GPKG" );
  if ( !drv )
    throw QgsProcessingException( QStringLiteral( "GPKG driver is not available" ) );
  GDALDatasetH ds = GDALCreate( drv, staging.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr );
  if ( !ds )
  {
    removeGpkgStaging( staging );
    throw QgsProcessingException( QStringLiteral( "cannot create %1: %2" )
                                      .arg( path, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
  OGRSpatialReferenceH srs = nullptr;
  if ( !wkt.isEmpty() )
  {
    srs = OSRNewSpatialReference( nullptr );
    QByteArray bytes = wkt.toUtf8();
    char *ptr = bytes.data();
    if ( OSRImportFromWkt( srs, &ptr ) != OGRERR_NONE )
    {
      OSRDestroySpatialReference( srs );
      srs = nullptr;
    }
    else
    {
      OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
    }
  }
  OGRLayerH layer = GDALDatasetCreateLayer( ds, "facies_polygons", srs, wkbPolygon, nullptr );
  if ( srs )
    OSRDestroySpatialReference( srs );
  if ( !layer )
  {
    GDALClose( ds );
    removeGpkgStaging( staging );
    throw QgsProcessingException( QStringLiteral( "cannot create facies_polygons layer" ) );
  }
  OGRFieldDefnH codeFld = OGR_Fld_Create( "facies_code", OFTInteger );
  OGR_L_CreateField( layer, codeFld, TRUE );
  OGR_Fld_Destroy( codeFld );
  OGRFieldDefnH areaFld = OGR_Fld_Create( "area", OFTReal );
  OGR_L_CreateField( layer, areaFld, TRUE );
  OGR_Fld_Destroy( areaFld );
  const bool withRanges = valueBounds && ( !valueBounds[0].isEmpty() || !valueBounds[1].isEmpty() );
  if ( withRanges )
    for ( const char *name : { "value_min", "value_max" } )
    {
      OGRFieldDefnH fld = OGR_Fld_Create( name, OFTReal );
      OGR_L_CreateField( layer, fld, TRUE );
      OGR_Fld_Destroy( fld );
    }

  for ( const CodedPoly &poly : polys )
  {
    QByteArray wktBytes = poly.geom.asWkt().toUtf8();
    char *wptr = wktBytes.data();
    OGRGeometryH og = nullptr;
    if ( OGR_G_CreateFromWkt( &wptr, nullptr, &og ) != OGRERR_NONE || !og )
    {
      GDALClose( ds );
      removeGpkgStaging( staging );
      throw QgsProcessingException( QStringLiteral( "cannot encode facies polygon WKT" ) );
    }
    OGRFeatureH feat = OGR_F_Create( OGR_L_GetLayerDefn( layer ) );
    OGR_F_SetFieldInteger( feat, 0, poly.code );
    OGR_F_SetFieldDouble( feat, 1, poly.geom.area() );
    if ( withRanges )
      for ( int i = 0; i < 2; ++i )
      {
        const auto bound = valueBounds[i].value( poly.code );
        if ( bound.second )
          OGR_F_SetFieldDouble( feat, 2 + i, bound.first );
      }
    OGR_F_SetGeometry( feat, og );
    OGR_G_DestroyGeometry( og );
    if ( OGR_L_CreateFeature( layer, feat ) != OGRERR_NONE )
    {
      OGR_F_Destroy( feat );
      GDALClose( ds );
      removeGpkgStaging( staging );
      throw QgsProcessingException( QStringLiteral( "failed to write a facies polygon feature" ) );
    }
    OGR_F_Destroy( feat );
  }
  // GPKG 会把 ENGCRS 降成 LOCAL_CS。原始 WKT 留在元数据里，打开时才能
  // 对回工程网格，显式配准才认这层的坐标。
  if ( !wkt.isEmpty() )
    GDALSetMetadataItem( ds, "PALEO_CRS_WKT", wkt.toUtf8().constData(), nullptr );
  GDALSetMetadataItem( ds, "PALEO_PROVENANCE", provenance.toUtf8().constData(), nullptr );
  // GDALClose 才真正落盘；关失败同样算失败（半成品不得进位）。
  if ( GDALClose( ds ) != CE_None )
  {
    removeGpkgStaging( staging );
    throw QgsProcessingException( QStringLiteral( "failed to finalize %1" ).arg( path ) );
  }
  commitStagingGpkg( staging, path );
}

} // namespace

QString FaciesPolygonizeAlgorithm::shortHelpString() const
{
  return QStringLiteral(
      "Turn a facies raster into an editable polygon coverage. Cells are rounded to "
      "integer facies codes. SMOOTH majority passes (3×3 mode filter) remove "
      "salt-and-pepper noise; parts smaller than MIN_CELLS cells, smaller than "
      "MIN_FRACTION of the valid cells, or under MIN_AREA map "
      "units² are absorbed into the largest adjacent facies, and GDALPolygonize + "
      "dissolve builds one polygon per part. "
      "SIMPLIFY preserves the coverage exterior and runs GEOS coverage simplification so each shared boundary is simplified "
      "once. BOUNDARY_SMOOTH then runs Chaikin corner-cutting passes on every shared "
      "boundary arc in the boundary graph: because each shared edge exists exactly once, "
      "both adjacent faces rebuild onto the identical rounded line and the coverage "
      "stays gap-free. Coverage exterior and NoData boundaries stay fixed. Output footprint is checked against the classified grid. Junction nodes (arc endpoints) stay fixed; straight arcs are "
      "skipped. The polygons are then rebuilt from that boundary graph. When SNAP_TOLERANCE "
      "> 0, an arc whose vertices all lie within that distance of a constraint, and whose "
      "direction is within ANGLE_TOLERANCE degrees of it, is replaced by the constraint "
      "geometry. Point constraints snap the nearest graph node. Provenance is returned "
      "as PROVENANCE and stored on the GeoPackage." );
}

void FaciesPolygonizeAlgorithm::initAlgorithm( const QVariantMap & )
{
  addParameter( new QgsProcessingParameterRasterLayer(
      QStringLiteral( "INPUT" ), QStringLiteral( "Classified facies raster" ) ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "SMOOTH" ), QStringLiteral( "Majority smoothing passes (3×3)" ),
      Qgis::ProcessingNumberParameterType::Integer, 1, true, 0.0, 10.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MIN_CELLS" ), QStringLiteral( "Minimum part size (cells)" ),
      Qgis::ProcessingNumberParameterType::Integer, 4, true, 0.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MIN_FRACTION" ),
      QStringLiteral( "Minimum part size (fraction of valid cells)" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0025, true, 0.0, 1.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "MIN_AREA" ), QStringLiteral( "Minimum part area (map units²)" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0, true, 0.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "SIMPLIFY" ), QStringLiteral( "Coverage simplify tolerance" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0, true, 0.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "BOUNDARY_SMOOTH" ),
      QStringLiteral( "Boundary smoothing passes (Chaikin)" ),
      Qgis::ProcessingNumberParameterType::Integer, 2, true, 0.0, 5.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "SNAP_TOLERANCE" ), QStringLiteral( "Constraint snap distance" ),
      Qgis::ProcessingNumberParameterType::Double, 0.0, true, 0.0 ) );
  addParameter( new QgsProcessingParameterNumber(
      QStringLiteral( "ANGLE_TOLERANCE" ), QStringLiteral( "Constraint angle tolerance (degrees)" ),
      Qgis::ProcessingNumberParameterType::Double, 15.0, true, 0.0, 90.0 ) );
  addParameter( new QgsProcessingParameterString(
      QStringLiteral( "VALUE_RANGES" ),
      QStringLiteral( "Class value ranges JSON ({code: [min, max]})" ),
      QVariant(), true, true ) );
  addParameter( new QgsProcessingParameterFeatureSource(
      QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "Constraint lines, polygons, or points" ),
      QList<int>() << static_cast<int>( Qgis::ProcessingSourceType::VectorAnyGeometry ),
      QVariant(), true ) );
  addParameter( new QgsProcessingParameterVectorDestination(
      QStringLiteral( "OUTPUT" ), QStringLiteral( "Facies polygons" ) ) );
}

QVariantMap FaciesPolygonizeAlgorithm::processAlgorithm( const QVariantMap &parameters,
                                                        QgsProcessingContext &context,
                                                        QgsProcessingFeedback *feedback )
{
  canceled( feedback );
  QgsRasterLayer *rl = parameterAsRasterLayer( parameters, QStringLiteral( "INPUT" ), context );
  if ( !rl || !rl->isValid() || rl->source().isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid input raster" ) );
  const double minArea = parameterAsDouble( parameters, QStringLiteral( "MIN_AREA" ), context );
  const int smooth = parameterAsInt( parameters, QStringLiteral( "SMOOTH" ), context );
  const int minCells = parameterAsInt( parameters, QStringLiteral( "MIN_CELLS" ), context );
  const double minFraction = parameterAsDouble( parameters, QStringLiteral( "MIN_FRACTION" ), context );
  const double simplify = parameterAsDouble( parameters, QStringLiteral( "SIMPLIFY" ), context );
  const int boundarySmooth =
      parameterAsInt( parameters, QStringLiteral( "BOUNDARY_SMOOTH" ), context );
  const double snapTol = parameterAsDouble( parameters, QStringLiteral( "SNAP_TOLERANCE" ), context );
  const double angleTol = parameterAsDouble( parameters, QStringLiteral( "ANGLE_TOLERANCE" ), context );
  if ( smooth < 0 || minCells < 0 || minArea < 0.0 || simplify < 0.0 || snapTol < 0.0 ||
       boundarySmooth < 0 || minFraction < 0.0 || minFraction > 1.0 )
    throw QgsProcessingException( QStringLiteral( "SMOOTH, MIN_CELLS, MIN_AREA, SIMPLIFY, BOUNDARY_SMOOTH and SNAP_TOLERANCE must be >= 0; MIN_FRACTION must be within [0, 1]" ) );

  const QString outPath = parameterAsOutputLayer( parameters, QStringLiteral( "OUTPUT" ), context );
  if ( outPath.isEmpty() )
    throw QgsProcessingException( QStringLiteral( "Invalid OUTPUT vector destination" ) );

  // 连续单因素分级的值域（{code: [min, max]}，min/max 可为 null）：写入
  // 每个相面的 value_min/value_max，标注与属性表能回答“这块是砂地比多少”。
  QHash<int, std::pair<double, bool>> valueBounds[2];
  const QString rangesJson = parameters.value( QStringLiteral( "VALUE_RANGES" ) ).toString();
  if ( !rangesJson.isEmpty() )
  {
    const auto ranges = QJsonDocument::fromJson( rangesJson.toUtf8() ).object();
    for ( auto it = ranges.begin(); it != ranges.end(); ++it )
    {
      bool okCode = false;
      const int code = it.key().toInt( &okCode );
      const auto pair = it.value().toArray();
      if ( !okCode || pair.size() != 2 )
        throw QgsProcessingException( QStringLiteral( "VALUE_RANGES entries must be {\"code\": [min, max]}" ) );
      for ( int i = 0; i < 2; ++i )
        if ( pair.at( i ).isDouble() )
          valueBounds[i].insert( code, { pair.at( i ).toDouble(), true } );
    }
  }

  QString src = rl->source();
  const int bar = src.indexOf( QLatin1Char( '|' ) );
  if ( bar > 0 )
    src = src.left( bar );
  GDALAllRegister();
  GDALDatasetH inDs = GDALOpen( src.toUtf8().constData(), GA_ReadOnly );
  if ( !inDs )
  {
    throw QgsProcessingException( QStringLiteral( "GDAL cannot open %1: %2" )
                                      .arg( src, QString::fromUtf8( CPLGetLastErrorMsg() ) ) );
  }
  const int cols = GDALGetRasterXSize( inDs );
  const int rows = GDALGetRasterYSize( inDs );
  double gt[6] = { 0, 1, 0, 0, 0, 1 };
  if ( GDALGetGeoTransform( inDs, gt ) != CE_None )
  {
    gt[0] = 0;
    gt[1] = 1;
    gt[2] = 0;
    gt[3] = 0;
    gt[4] = 0;
    gt[5] = 1;
  }
  const double cellArea = std::fabs( gt[1] * gt[5] - gt[2] * gt[4] );
  if ( !( cellArea > 0.0 ) || cols <= 0 || rows <= 0 )
  {
    GDALClose( inDs );
    throw QgsProcessingException( QStringLiteral( "input raster has no usable grid" ) );
  }
  // L-09：像元预算闸（paleoalgorithms/gridsolver 同口径 1 亿）——本算法
  // raw/grid/next/maskPx 平峰 ≈13 字节/像元同时驻留，无闸的超大输入是
  // bad_alloc/OOM 而不是如实报错。
  if ( qint64( cols ) * rows > qint64( 100'000'000 ) )
  {
    GDALClose( inDs );
    throw QgsProcessingException( QStringLiteral( "input raster exceeds the 100M-cell budget" ) );
  }
  GDALRasterBandH inBand = GDALGetRasterBand( inDs, 1 );
  int ndFlag = 0;
  const double nd = GDALGetRasterNoDataValue( inBand, &ndFlag );
  const bool hasNd = ndFlag != 0;
  QVector<float> raw( static_cast<qsizetype>( cols ) * rows );
  if ( GDALRasterIO( inBand, GF_Read, 0, 0, cols, rows, raw.data(), cols, rows, GDT_Float32, 0, 0 ) != CE_None )
  {
    GDALClose( inDs );
    throw QgsProcessingException( QStringLiteral( "failed to read input raster" ) );
  }
  QString wkt = rl->crs().isValid() ? rl->crs().toWkt( Qgis::CrsWktVariant::Wkt1Gdal ) : QString();
  if ( wkt.isEmpty() )
  {
    const char *proj = GDALGetProjectionRef( inDs );
    if ( proj && proj[0] )
      wkt = QString::fromUtf8( proj );
  }
  GDALClose( inDs );

  QVector<int> grid( raw.size() );
  for ( int i = 0; i < raw.size(); ++i )
  {
    const float v = raw.at( i );
    if ( std::isnan( v ) || ( hasNd && v == static_cast<float>( nd ) ) ) // #165：float 口径
      grid[i] = kNodata;
    else
    {
      const int code = static_cast<int>( std::lround( static_cast<double>( v ) ) );
      grid[i] = code == kNodata ? kNodata : code;
    }
  }
  if ( feedback )
    feedback->setProgress( 15 );

  // 平滑在聚合之前：多数滤波先去掉椒盐噪点，剩余的碎块再按 MIN_CELLS/
  // MIN_AREA 吸收——顺序反过来会把噪声当作图斑保下来。
  int smoothedCells = 0;
  for ( int pass = 0; pass < smooth; ++pass )
  {
    const int changed = smoothMajority( grid, cols, rows );
    smoothedCells += changed;
    canceled( feedback );
    if ( changed == 0 )
      break;
  }
  if ( feedback )
    feedback->setProgress( 22 );

  // 小图斑门限取两种口径的大者：绝对像元数（MIN_CELLS，小栅格防误并）与
  // 有效区占比（MIN_FRACTION，大栅格防碎斑——逐像元聚类的噪声斑随栅格
  // 变大成批出现，固定像元数兜不住）。
  int validCells = 0;
  for ( const int code : grid )
    if ( code != kNodata )
      ++validCells;
  const int minCellsEffective = std::max(
      minCells, static_cast<int>( std::ceil( minFraction * validCells ) ) );
  // A11：吸收步一次标号 + 并查集（旧实现逐斑全图重标号），并可取消。
  static_assert( paleo::facies::kSliverNodata == kNodata, "nodata sentinel must agree" );
  const int slivers = paleo::facies::absorbSmallParts(
      grid, cols, rows, cellArea, minArea, minCellsEffective,
      [feedback] { return feedback && feedback->isCanceled(); } );
  if ( slivers < 0 )
    throw QgsProcessingException( QStringLiteral( "Canceled" ) );
  canceled( feedback );
  if ( feedback )
    feedback->setProgress( 30 );

  QVector<CodedPoly> polys = polygonizeGrid( grid, cols, rows, gt, wkt );
  polys = nodeSharedEdges( polys );
  const auto expectedFootprint = coverageFootprint(polys);
  canceled( feedback );
  if ( feedback )
    feedback->setProgress( 55 );

  if ( simplify > 0.0 )
    polys = simplifyCoverage( polys, simplify );
  canceled( feedback );

  const double qTol = std::max( 1e-8, std::max( std::fabs( gt[1] ), std::fabs( gt[5] ) ) * 1e-9 );
  Graph graph = buildBoundaryGraph( polys, qTol );
  if ( feedback )
    feedback->setProgress( 70 );

  // 边界圆润化在吸附之前：约束线是权威边界，替换弧时平滑结果被丢弃，
  // 约束线仍说了算。
  const int smoothedArcs = boundarySmooth > 0 ? smoothArcs( graph, boundarySmooth ) : 0;
  canceled( feedback );

  int conflated = 0;
  if ( snapTol > 0.0 )
  {
    QVector<QgsGeometry> lines;
    QVector<QgsPointXY> points;
    std::unique_ptr<QgsProcessingFeatureSource> constraints(
        parameterAsSource( parameters, QStringLiteral( "CONSTRAINTS" ), context ) );
    if ( constraints )
    {
      // L-09：约束几何此前按自身 CRS 原始坐标直接与栅格坐标求交——CRS 不
      // 一致时合流静默不生效或在大容差下错误替换弧（distancetransform.cpp
      // 同款变换 + BIZ-10 有效性前置）。
      const QgsCoordinateReferenceSystem from = constraints->sourceCrs();
      const QgsCoordinateReferenceSystem to = rl->crs();
      std::unique_ptr<QgsCoordinateTransform> xform;
      if ( from.isValid() && to.isValid() && from != to )
      {
        try
        {
          xform = std::make_unique<QgsCoordinateTransform>( from, to, context.transformContext() );
        }
        catch ( const QgsCsException &e )
        {
          throw QgsProcessingException(
              QStringLiteral( "Cannot transform constraints into the raster CRS: %1" ).arg( e.what() ) );
        }
        if ( !xform->isValid() )
          throw QgsProcessingException(
              QStringLiteral( "Cannot transform constraints into the %1 CRS: no coordinate operation "
                              "between %2 and %3 (engineering CRS without geodetic datum?)" )
                  .arg( QStringLiteral( "raster" ), from.userFriendlyIdentifier(),
                        to.userFriendlyIdentifier() ) );
      }
      QgsFeatureIterator it = constraints->getFeatures( QgsFeatureRequest() );
      QgsFeature feat;
      while ( it.nextFeature( feat ) )
      {
        if ( feat.hasGeometry() )
        {
          if ( xform )
          {
            QgsGeometry g = feat.geometry();
            try
            {
              g.transform( *xform );
            }
            catch ( const QgsCsException &e )
            {
              throw QgsProcessingException(
                  QStringLiteral( "Cannot transform a constraint feature: %1" ).arg( e.what() ) );
            }
            collectConstraints( g, lines, points );
          }
          else
            collectConstraints( feat.geometry(), lines, points );
        }
      }
    }
    conflated = conflateArcs( graph, lines, points, snapTol, angleTol );
  }
  polys = nodeSharedEdges( rebuildFaces( graph ) );
  canceled( feedback );
  if ( feedback )
    feedback->setProgress( 85 );

  const double gapWidth = std::max( qTol * 10.0, 1e-7 );
  checkCoverage( polys, gapWidth );
  const auto outputFootprint = coverageFootprint(polys);
  const double footprintDifference = checkFootprint(expectedFootprint, outputFootprint, cellArea);

  QJsonObject prov;
  prov.insert( QStringLiteral( "algorithm" ), QStringLiteral( "paleo:paleo_facies_polygonize" ) );
  prov.insert( QStringLiteral( "min_area" ), minArea );
  prov.insert( QStringLiteral( "simplify" ), simplify );
  prov.insert( QStringLiteral( "boundary_smooth" ), boundarySmooth );
  prov.insert( QStringLiteral( "smoothed_arcs" ), smoothedArcs );
  prov.insert( QStringLiteral( "snap_tolerance" ), snapTol );
  prov.insert( QStringLiteral( "angle_tolerance_deg" ), angleTol );
  prov.insert( QStringLiteral( "smooth_passes" ), smooth );
  prov.insert( QStringLiteral( "smoothed_cells" ), smoothedCells );
  prov.insert( QStringLiteral( "min_cells" ), minCells );
  prov.insert( QStringLiteral( "min_fraction" ), minFraction );
  prov.insert( QStringLiteral( "min_cells_effective" ), minCellsEffective );
  prov.insert( QStringLiteral( "slivers_merged" ), slivers );
  prov.insert( QStringLiteral( "faces" ), polys.size() );
  prov.insert( QStringLiteral( "expected_coverage_area" ), expectedFootprint.area() );
  prov.insert( QStringLiteral( "output_coverage_area" ), outputFootprint.area() );
  prov.insert( QStringLiteral( "coverage_difference_area" ), footprintDifference );
  prov.insert( QStringLiteral( "arcs" ), graph.arcs.size() );
  prov.insert( QStringLiteral( "conflated_arcs" ), conflated );
  prov.insert( QStringLiteral( "input" ), src );
  // 方向35：格网口径戳——演化跨期对比用同一格网/坐标域自证同源。
  // gt/cols/rows 是被多边形化栅格的原口径（north-up geotransform）。
  QJsonObject gridStamp;
  gridStamp.insert( QStringLiteral( "cols" ), cols );
  gridStamp.insert( QStringLiteral( "rows" ), rows );
  QJsonArray gtArray;
  for ( const double v : gt )
    gtArray.append( v );
  gridStamp.insert( QStringLiteral( "geotransform" ), gtArray );
  gridStamp.insert( QStringLiteral( "crs_wkt" ), wkt );
  prov.insert( QStringLiteral( "grid" ), gridStamp );
  QJsonArray steps;
  steps.append( QStringLiteral( "recode" ) );
  if ( slivers > 0 )
    steps.append( QStringLiteral( "sliver" ) );
  steps.append( QStringLiteral( "polygonize" ) );
  steps.append( QStringLiteral( "dissolve" ) );
  if ( simplify > 0.0 )
    steps.append( QStringLiteral( "simplify" ) );
  steps.append( QStringLiteral( "boundary-graph" ) );
  if ( smoothedArcs > 0 )
    steps.append( QStringLiteral( "boundary-smooth" ) );
  if ( conflated > 0 )
    steps.append( QStringLiteral( "conflate" ) );
  prov.insert( QStringLiteral( "steps" ), steps );
  const QString provenance = QString::fromUtf8( QJsonDocument( prov ).toJson( QJsonDocument::Compact ) );

  writeGpkg( outPath, polys, wkt, provenance, valueBounds );
  if ( feedback )
    feedback->setProgress( 100 );

  QVariantMap result;
  result.insert( QStringLiteral( "OUTPUT" ), outPath );
  result.insert( QStringLiteral( "PROVENANCE" ), provenance );
  return result;
}
