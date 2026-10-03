// 层：数据
#pragma once

#include "types.h"

#include <span>
#include <vector>

// buffer_transition 移植（haiyou-visualization 27fdb99）。
// 显式低值缓冲 + 有限宽平滑过渡：核内常值 floor，外侧五次 smoothstep 肩部，
// 重叠取各自独立计算的较低包络（线序不影响结果）。
// 最低值只来自调用方显式给的解释规则；本模块不推断任意断线代表零厚度。
// 层：数据
namespace paleo::singlefactor
{

struct BufferTransitionSpec
{
  std::vector<Point2> points; // 断线折线（<2 点跳过）
  double halfWidth = 0; // 核半宽，>=0
  double transitionWidth = 0; // 肩部宽，>0
  double floor = 0; // 核内显式低值
};

struct BufferTransitionRecord
{
  double floor = 0;
  double halfWidth = 0;
  double transitionWidth = 0;
  int coreCells = 0;
  int affectedCells = 0;
};

struct BufferTransitionResult
{
  Status status = Status::Ok;
  std::string message;
  std::vector<double> values; // 与 source 同尺寸；缺失与带外像元原样保留
  std::vector<BufferTransitionRecord> records;
};

BufferTransitionResult applyBufferTransition( const GridSpec &grid,
                                              const std::vector<double> &source,
                                              std::span<const BufferTransitionSpec> specs,
                                              const std::vector<std::uint8_t> &validMask );

} // namespace paleo::singlefactor
