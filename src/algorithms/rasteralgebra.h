// 层：数据
#pragma once
#include <vector>

#include <cstdint>

// rasteralgebra — 栅格代数核（面-面 / 面-常数 / 条件选择；纯数值）。
// NaN 语义（钉死）：
//   · 算术/极值：任一输入 NaN → 输出 NaN（诚实传播，不静默补值）；
//   · 除法分母为精确 0 → 输出 NaN（不是 ±inf——inf 会污染下游统计与
//     GeoTIFF 渲染，null 是更可诊断的语义）；其余浮点溢出按 IEEE 产生
//     ±inf，如实保留；
//   · 比较：NaN 参与的比较恒为假（mask=0）；
//   · where：按 mask 选源，被选中源 NaN → 输出 NaN（未选中源的 NaN
//     不影响输出）。
// 典型用法：等厚图 = 下层位 − 上层位（Subtract）；基准面掩膜 = 比较 +
// where。核不做网格几何校验——调用方保证同网格（见 Processing/工作流层）。

namespace paleo::rasteralgebra
{

enum class BinaryOp
{
  Add,
  Subtract,
  Multiply,
  Divide,
  Min,
  Max
};

enum class UnaryOp
{
  Negate,
  Abs
};

enum class CompareOp
{
  Greater,
  GreaterOrEqual,
  Less,
  LessOrEqual
};

// a <op> b，逐元素（n = 像元数）。
void applyBinary(const float *a, const float *b, std::size_t n, BinaryOp op, float *out);

// a <op> c（c 须有限非 NaN）。
void applyConstant(const float *a, std::size_t n, double c, BinaryOp op, float *out);

void applyUnary(const float *a, std::size_t n, UnaryOp op, float *out);

// a <op> c → mask（0/1）。
void compareConstant(const float *a, std::size_t n, double c, CompareOp op,
                     std::uint8_t *mask);

// a <op> b → mask（0/1）；NaN 任一侧 → 0。
void compareRasters(const float *a, const float *b, std::size_t n, CompareOp op,
                    std::uint8_t *mask);

// mask != 0 → a，否则 b。
void where(const std::uint8_t *mask, const float *a, const float *b, std::size_t n,
           float *out);

} // namespace paleo::rasteralgebra
