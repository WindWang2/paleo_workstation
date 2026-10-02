// 层：数据
#include "rasteralgebra.h"

#include <cmath>
#include <limits>

namespace paleo::rasteralgebra
{
namespace
{

inline float binaryOp(float a, float b, BinaryOp op)
{
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<float>::quiet_NaN();
  switch (op)
  {
    case BinaryOp::Add:
      return a + b;
    case BinaryOp::Subtract:
      return a - b;
    case BinaryOp::Multiply:
      return a * b;
    case BinaryOp::Divide:
      return b == 0.0f ? std::numeric_limits<float>::quiet_NaN() : a / b;
    case BinaryOp::Min:
      return a < b ? a : b;
    case BinaryOp::Max:
      return a > b ? a : b;
  }
  return std::numeric_limits<float>::quiet_NaN();
}

inline bool compareOp(float a, float b, CompareOp op)
{
  if (std::isnan(a) || std::isnan(b))
    return false;
  switch (op)
  {
    case CompareOp::Greater:
      return a > b;
    case CompareOp::GreaterOrEqual:
      return a >= b;
    case CompareOp::Less:
      return a < b;
    case CompareOp::LessOrEqual:
      return a <= b;
  }
  return false;
}

} // namespace

void applyBinary(const float *a, const float *b, std::size_t n, BinaryOp op, float *out)
{
  for (std::size_t i = 0; i < n; ++i)
    out[i] = binaryOp(a[i], b[i], op);
}

void applyConstant(const float *a, std::size_t n, double c, BinaryOp op, float *out)
{
  const float cf = static_cast<float>(c);
  for (std::size_t i = 0; i < n; ++i)
    out[i] = binaryOp(a[i], cf, op);
}

void applyUnary(const float *a, std::size_t n, UnaryOp op, float *out)
{
  for (std::size_t i = 0; i < n; ++i)
  {
    if (std::isnan(a[i]))
    {
      out[i] = a[i];
      continue;
    }
    out[i] = op == UnaryOp::Negate ? -a[i] : std::fabs(a[i]);
  }
}

void compareConstant(const float *a, std::size_t n, double c, CompareOp op,
                     std::uint8_t *mask)
{
  const float cf = static_cast<float>(c);
  for (std::size_t i = 0; i < n; ++i)
    mask[i] = compareOp(a[i], cf, op) ? 1 : 0;
}

void compareRasters(const float *a, const float *b, std::size_t n, CompareOp op,
                    std::uint8_t *mask)
{
  for (std::size_t i = 0; i < n; ++i)
    mask[i] = compareOp(a[i], b[i], op) ? 1 : 0;
}

void where(const std::uint8_t *mask, const float *a, const float *b, std::size_t n,
           float *out)
{
  for (std::size_t i = 0; i < n; ++i)
    out[i] = mask[i] ? a[i] : b[i];
}

} // namespace paleo::rasteralgebra
