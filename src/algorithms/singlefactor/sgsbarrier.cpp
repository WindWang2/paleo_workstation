// 层：数据
#include "sgsbarrier.h"

#include <algorithm>
#include <map>

namespace paleo::singlefactor
{

paleo::geostat::SgsBarrierInput toSgsBarrierInput( const BarrierGrid &barrierGrid,
                                                   const std::vector<Sample> &samples,
                                                   const std::string &barrierModel )
{
  paleo::geostat::SgsBarrierInput out;
  // 稳定重编号：每个旧分量号 → 其最小 row-major 格索引，按该索引升序编新号。
  std::map<int, std::size_t> firstCellOf; // 旧分量号 → 首个格索引
  for ( std::size_t cell = 0; cell < barrierGrid.component.size(); ++cell )
  {
    const int label = barrierGrid.component[cell];
    if ( label >= 0 )
      firstCellOf.emplace( label, cell ); // map::emplace 保留首个（cell 升序扫描）
  }
  std::map<int, int> renumber; // 旧 → 新（屏障/域外负值不经此表）
  int next = 0;
  for ( const auto &[label, firstCell] : firstCellOf )
    renumber.emplace( label, next++ );
  out.componentCount = next;

  out.cellComponent.resize( barrierGrid.component.size() );
  for ( std::size_t cell = 0; cell < barrierGrid.component.size(); ++cell )
  {
    const int label = barrierGrid.component[cell];
    out.cellComponent[cell] = label >= 0 ? renumber[label] : label; // -2 屏障格语义原样保留
  }
  out.sampleComponent.resize( samples.size(), -1 ); // 短于样本数的尾部按未归属
                                                    // ——数值核拒绝，不静默归 0 区
  for ( std::size_t i = 0; i < samples.size() && i < barrierGrid.sampleComponent.size(); ++i )
  {
    const int label = barrierGrid.sampleComponent[i];
    out.sampleComponent[i] = label >= 0 ? renumber[label] : label; // -1 未归属：数值核拒绝
  }
  out.consumedBarrierIds = barrierGrid.consumedIds;
  out.barrierModel = barrierModel;
  return out;
}

GridSpec toSinglefactorGrid( const paleo::geostat::GridSpec &grid )
{
  GridSpec out;
  out.cols = grid.cols;
  out.rows = grid.rows;
  out.originX = grid.originX;
  out.originY = grid.originY;
  out.pixelWidth = grid.pixelWidth;
  out.pixelHeight = grid.pixelHeight;
  out.crs = grid.crs;
  return out;
}

} // namespace paleo::singlefactor
