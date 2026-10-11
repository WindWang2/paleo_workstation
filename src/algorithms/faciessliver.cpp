// 层：数据
#include "faciessliver.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace paleo::facies
{
namespace
{
struct Part
{
  int code = 0;
  int cells = 0;
  int first = 0; // 最小像元下标 = 重标号时的扫描序（旧实现的 comps 下标序）
  bool border = false;
  std::unordered_set<int> neighbors; // 只存活根
};

int findRoot( std::vector<int> &parent, int x )
{
  int r = x;
  while ( parent[r] != r )
    r = parent[r];
  while ( parent[x] != r )
  {
    const int next = parent[x];
    parent[x] = r;
    x = next;
  }
  return r;
}
} // namespace

int absorbSmallParts( QVector<int> &grid, int cols, int rows, double cellArea,
                      double minArea, int minCells,
                      const std::function<bool()> &cancelled )
{
  if ( !( minArea > 0.0 ) && minCells <= 1 )
    return 0;
  if ( cols <= 0 || rows <= 0 || grid.size() != static_cast<qsizetype>( cols ) * rows )
    return 0;
  const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };

  // ---- 一次 4 连通标号 ---------------------------------------------------
  const int n = cols * rows;
  std::vector<int> lab( n, -1 );
  std::vector<Part> parts;
  std::vector<int> stack;
  for ( int start = 0; start < n; ++start )
  {
    if ( lab[start] != -1 )
      continue;
    if ( ( start & 0xFFFF ) == 0 && isCancelled() )
      return -1;
    Part p;
    p.code = grid[start];
    p.first = start;
    const int id = static_cast<int>( parts.size() );
    stack.clear();
    stack.push_back( start );
    lab[start] = id;
    while ( !stack.empty() )
    {
      const int k = stack.back();
      stack.pop_back();
      const int cx = k % cols;
      const int cy = k / cols;
      ++p.cells;
      if ( cx == 0 || cy == 0 || cx == cols - 1 || cy == rows - 1 )
        p.border = true;
      const int nbrs[4] = { cx + 1 < cols ? k + 1 : -1, cx > 0 ? k - 1 : -1,
                            cy + 1 < rows ? k + cols : -1, cy > 0 ? k - cols : -1 };
      for ( const int j : nbrs )
      {
        if ( j >= 0 && lab[j] == -1 && grid[j] == p.code )
        {
          lab[j] = id;
          stack.push_back( j );
        }
      }
    }
    parts.push_back( std::move( p ) );
  }
  for ( int y = 0; y < rows; ++y )
  {
    for ( int x = 0; x < cols; ++x )
    {
      const int i = y * cols + x;
      if ( x + 1 < cols && lab[i] != lab[i + 1] )
      {
        parts[lab[i]].neighbors.insert( lab[i + 1] );
        parts[lab[i + 1]].neighbors.insert( lab[i] );
      }
      if ( y + 1 < rows && lab[i] != lab[i + cols] )
      {
        parts[lab[i]].neighbors.insert( lab[i + cols] );
        parts[lab[i + cols]].neighbors.insert( lab[i] );
      }
    }
  }

  const auto underThreshold = [&]( const Part &p ) {
    const double area = p.cells * cellArea;
    return p.cells < minCells || ( area < minArea && area > 0.0 );
  };
  // 可吸收判定：门限之外的单调性——像元只增不减（门限只会由真转假）、
  // 贴边只会由假转真；nodata 斑的邻居全是相斑（相邻 nodata 必同斑），
  // 「有相邻相斑」只在吞并全部邻居时转假。故出堆时不可吸收即永不可吸收。
  const auto absorbable = [&]( const Part &p ) {
    if ( !underThreshold( p ) )
      return false;
    if ( p.code == kSliverNodata && p.border )
      return false;
    if ( p.code != kSliverNodata )
      return true;
    for ( const int nb : p.neighbors )
      if ( parts[nb].code != kSliverNodata )
        return true;
    return false;
  };

  using Entry = std::tuple<int, int, int>; // (cells, first, id) —— 最小面积优先，等面积扫描序优先
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
  for ( int i = 0; i < static_cast<int>( parts.size() ); ++i )
    if ( underThreshold( parts[i] ) )
      heap.emplace( parts[i].cells, parts[i].first, i );

  std::vector<int> parent( parts.size() );
  for ( int i = 0; i < static_cast<int>( parent.size() ); ++i )
    parent[i] = i;

  int merged = 0;
  std::vector<int> group;
  while ( !heap.empty() )
  {
    const auto [cells, first, id] = heap.top();
    heap.pop();
    if ( parent[id] != id || parts[id].cells != cells || parts[id].first != first )
      continue; // 陈旧条目：已被并入或已长大（长大时另有新条目）
    Part &victim = parts[id];
    if ( !absorbable( victim ) )
      continue;
    if ( ( merged & 0xFF ) == 0 && isCancelled() )
      return -1;

    int target = kSliverNodata;
    int bestCells = -1;
    int bestCode = std::numeric_limits<int>::max();
    for ( const int nb : victim.neighbors )
    {
      const Part &p = parts[nb];
      if ( p.code == kSliverNodata )
        continue;
      if ( p.cells > bestCells || ( p.cells == bestCells && p.code < bestCode ) )
      {
        bestCells = p.cells;
        bestCode = p.code;
        target = p.code;
      }
    }

    // 受害斑改码后，与之相邻的同码斑经它连成一斑（旧实现重标号的效果）。
    group.clear();
    group.push_back( id );
    for ( const int nb : victim.neighbors )
      if ( parts[nb].code == target )
        group.push_back( nb );
    int root = group.front();
    for ( const int g : group )
      if ( parts[g].neighbors.size() > parts[root].neighbors.size() )
        root = g;
    Part &r = parts[root];
    for ( const int g : group )
      r.neighbors.erase( g );
    for ( const int g : group )
    {
      if ( g == root )
        continue;
      Part &p = parts[g];
      for ( const int x : p.neighbors )
      {
        if ( std::find( group.begin(), group.end(), x ) != group.end() )
          continue;
        parts[x].neighbors.erase( g );
        parts[x].neighbors.insert( root );
        r.neighbors.insert( x );
      }
      r.cells += p.cells;
      r.first = std::min( r.first, p.first );
      r.border = r.border || p.border;
      std::unordered_set<int>().swap( p.neighbors );
      parent[g] = root;
    }
    r.code = target;
    if ( underThreshold( r ) )
      heap.emplace( r.cells, r.first, root );
    ++merged;
  }

  for ( int i = 0; i < n; ++i )
    grid[i] = parts[findRoot( parent, lab[i] )].code;
  return merged;
}
} // namespace paleo::facies
