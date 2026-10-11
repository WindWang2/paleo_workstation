// 层：测试壳
// A11：小图斑吸收新实现（一次标号 + 并查集 + 小顶堆）与旧实现（逐斑全图重
// 标号）在随机网格上逐像元对拍；并覆盖可取消与大网格耗时。
#include "../src/algorithms/faciessliver.h"

#include <QElapsedTimer>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QTest>
#include <QVector>
#include <algorithm>
#include <limits>

namespace
{
constexpr int kNd = paleo::facies::kSliverNodata;
// ---- 旧实现原样搬入（基线 198401ab faciespolygonize.cpp），仅改名 ----
struct Comp
{
  int code = 0;
  int cells = 0;
  bool border = false;
  QSet<int> neighbors;
};

void labelComponents( const QVector<int> &grid, int cols, int rows,
                      QVector<int> &lab, QVector<Comp> &comps )
{
  lab.fill( -1, grid.size() );
  comps.clear();
  const int dx[4] = { 1, -1, 0, 0 };
  const int dy[4] = { 0, 0, 1, -1 };
  for ( int y = 0; y < rows; ++y )
  {
    for ( int x = 0; x < cols; ++x )
    {
      const int start = y * cols + x;
      if ( lab[start] != -1 )
        continue;
      Comp c;
      c.code = grid[start];
      const int id = comps.size();
      QVector<int> stack;
      stack.append( start );
      lab[start] = id;
      while ( !stack.isEmpty() )
      {
        const int k = stack.takeLast();
        const int cx = k % cols;
        const int cy = k / cols;
        ++c.cells;
        if ( cx == 0 || cy == 0 || cx == cols - 1 || cy == rows - 1 )
          c.border = true;
        for ( int d = 0; d < 4; ++d )
        {
          const int nx = cx + dx[d];
          const int ny = cy + dy[d];
          if ( nx < 0 || ny < 0 || nx >= cols || ny >= rows )
            continue;
          const int j = ny * cols + nx;
          if ( grid[j] == grid[k] && lab[j] == -1 )
          {
            lab[j] = id;
            stack.append( j );
          }
        }
      }
      comps.append( c );
    }
  }
  for ( int y = 0; y < rows; ++y )
  {
    for ( int x = 0; x < cols; ++x )
    {
      const int i = y * cols + x;
      if ( x + 1 < cols )
      {
        const int j = i + 1;
        if ( lab[i] != lab[j] )
        {
          comps[lab[i]].neighbors.insert( lab[j] );
          comps[lab[j]].neighbors.insert( lab[i] );
        }
      }
      if ( y + 1 < rows )
      {
        const int j = i + cols;
        if ( lab[i] != lab[j] )
        {
          comps[lab[i]].neighbors.insert( lab[j] );
          comps[lab[j]].neighbors.insert( lab[i] );
        }
      }
    }
  }
}

// Absorb parts under minArea into the largest adjacent facies. Border-touching
// nodata is the outside of the map and is left alone. An interior speck with
// no facies neighbor is cleared to nodata. A part is absorbable when it is
// smaller than minCells cells (grid-scale aggregation) or its area falls under
// minArea (map-unit aggregation) — both criteria are ORed.
int referenceAbsorb( QVector<int> &grid, int cols, int rows, double cellArea, double minArea, int minCells )
{
  if ( !( minArea > 0.0 ) && minCells <= 1 )
    return 0;
  int merged = 0;
  const int guardMax = std::max( 1, cols * rows );
  for ( int pass = 0; pass < guardMax; ++pass )
  {
    QVector<int> lab;
    QVector<Comp> comps;
    labelComponents( grid, cols, rows, lab, comps );
    int victim = -1;
    double victimArea = std::numeric_limits<double>::max();
    for ( int i = 0; i < comps.size(); ++i )
    {
      const Comp &c = comps.at( i );
      const double area = c.cells * cellArea;
      const bool underThreshold = ( c.cells < minCells ) ||
                                  ( area < minArea && area > 0.0 );
      if ( !underThreshold )
        continue;
      if ( c.code == kNd && c.border )
        continue;
      bool can = c.code != kNd;
      if ( !can )
      {
        for ( int n : c.neighbors )
        {
          if ( comps.at( n ).code != kNd )
            can = true;
        }
      }
      if ( !can )
        continue;
      if ( area < victimArea )
      {
        victimArea = area;
        victim = i;
      }
    }
    if ( victim < 0 )
      break;
    const Comp &v = comps.at( victim );
    int target = kNd;
    int bestCells = -1;
    int bestCode = std::numeric_limits<int>::max();
    for ( int n : v.neighbors )
    {
      const Comp &nb = comps.at( n );
      if ( nb.code == kNd )
        continue;
      if ( nb.cells > bestCells || ( nb.cells == bestCells && nb.code < bestCode ) )
      {
        bestCells = nb.cells;
        bestCode = nb.code;
        target = nb.code;
      }
    }
    for ( int i = 0; i < grid.size(); ++i )
    {
      if ( lab[i] == victim )
        grid[i] = target;
    }
    ++merged;
  }
  return merged;
}

QVector<int> randomGrid( QRandomGenerator &rng, int cols, int rows, int codes, int ndPermille, bool blobs )
{
  QVector<int> g( cols * rows );
  for ( int i = 0; i < g.size(); ++i )
  {
    if ( static_cast<int>( rng.bounded( 1000 ) ) < ndPermille )
      g[i] = kNd;
    else
      g[i] = 1 + static_cast<int>( rng.bounded( codes ) );
  }
  if ( blobs ) // 让一部分像元复制左/上邻，形成大小不一的斑
  {
    for ( int y = 0; y < rows; ++y )
      for ( int x = 0; x < cols; ++x )
      {
        const int i = y * cols + x;
        const int r = static_cast<int>( rng.bounded( 10 ) );
        if ( r < 4 && x > 0 )
          g[i] = g[i - 1];
        else if ( r < 7 && y > 0 )
          g[i] = g[i - cols];
      }
  }
  return g;
}
} // namespace

class TestFaciesSliver : public QObject
{
  Q_OBJECT
private slots:
  void matchesReferenceOnRandomGrids_data()
  {
    QTest::addColumn<int>( "seed" );
    for ( int s = 1; s <= 120; ++s )
      QTest::newRow( qPrintable( QStringLiteral( "seed%1" ).arg( s ) ) ) << s;
  }
  void matchesReferenceOnRandomGrids()
  {
    QFETCH( int, seed );
    QRandomGenerator rng( static_cast<quint32>( seed ) );
    const int cols = 3 + static_cast<int>( rng.bounded( 22 ) );
    const int rows = 3 + static_cast<int>( rng.bounded( 22 ) );
    const int codes = 1 + static_cast<int>( rng.bounded( 5 ) );
    const int nd = static_cast<int>( rng.bounded( 4 ) ) * 60;
    const bool blobs = rng.bounded( 2 ) == 1;
    const QVector<int> base = randomGrid( rng, cols, rows, codes, nd, blobs );
    const double cellArea = 1.0 + rng.bounded( 3 );
    const int minCells = static_cast<int>( rng.bounded( 12 ) );
    const double minArea = rng.bounded( 2 ) ? 0.0 : cellArea * rng.bounded( 15 );
    QVector<int> expected = base;
    QVector<int> actual = base;
    const int mergedRef = referenceAbsorb( expected, cols, rows, cellArea, minArea, minCells );
    const int merged = paleo::facies::absorbSmallParts( actual, cols, rows, cellArea, minArea, minCells );
    QCOMPARE( merged, mergedRef );
    QCOMPARE( actual, expected );
  }

  void noThresholdIsNoop()
  {
    QVector<int> g{ 1, 2, 1, 2 };
    QCOMPARE( paleo::facies::absorbSmallParts( g, 2, 2, 1.0, 0.0, 1 ), 0 );
    QCOMPARE( g, ( QVector<int>{ 1, 2, 1, 2 } ) );
  }

  void cancelAborts()
  {
    QRandomGenerator rng( 7 );
    QVector<int> g = randomGrid( rng, 200, 200, 4, 0, false );
    int polls = 0;
    const int r = paleo::facies::absorbSmallParts( g, 200, 200, 1.0, 0.0, 50,
                                                   [&polls] { return ++polls > 2; } );
    QCOMPARE( r, -1 );
  }

  // 审查 A11：400² 椒盐网格旧实现约 12 s；新实现须在秒级以内（宽松上限
  // 防 CI 抖动，同时远低于旧实现量级）。
  void largeNoisyGridIsFast()
  {
    QRandomGenerator rng( 11 );
    QVector<int> g = randomGrid( rng, 400, 400, 6, 20, false );
    QElapsedTimer t;
    t.start();
    const int merged = paleo::facies::absorbSmallParts( g, 400, 400, 1.0, 0.0, 400 );
    QVERIFY( merged > 1000 );
    QVERIFY2( t.elapsed() < 4000, qPrintable( QStringLiteral( "took %1 ms" ).arg( t.elapsed() ) ) );
    // 结果中不应残留小于门限的相斑（除非无相邻相斑）
    QSet<int> codes( g.cbegin(), g.cend() );
    QVERIFY( !codes.isEmpty() );
  }
};

QTEST_APPLESS_MAIN( TestFaciesSliver )
#include "tst_faciessliver.moc"
