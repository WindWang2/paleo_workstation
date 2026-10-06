// 层：数据
#pragma once

#include <QString>
#include <array>
#include <cmath>
#include <optional>

// 井文本表的显式空值词表。精确匹配：合法负坐标/海拔不能因范围比较被抹掉。
// LAS 文件若声明 NULL，仍以该文件声明为准；本词表供无 NULL 元数据的井表使用。
namespace paleo::wellnumeric
{
inline constexpr double kSmiNull = -99999.0;
inline constexpr double kLasDefaultNull = -999.25;
inline constexpr std::array<double, 4> kNullValues = {
    kSmiNull, kLasDefaultNull, -9999.0, -999.0};

inline bool isNull(double value)
{
  for (const double sentinel : kNullValues)
    if (value == sentinel)
      return true;
  return false;
}

inline bool isUsable(double value)
{
  return std::isfinite(value) && !isNull(value);
}

enum class NumberKind { Value, Empty, NonNumeric, NonFinite, NullSentinel };
inline NumberKind parse(const QString &token, double *out)
{
  const QString trimmed = token.trimmed();
  if (trimmed.isEmpty())
    return NumberKind::Empty;
  bool ok = false;
  const double value = trimmed.toDouble(&ok);
  // Qt 对溢出返回 Inf + ok=false；同样归非有限，不归合法数值。
  if (!std::isfinite(value))
    return NumberKind::NonFinite;
  if (!ok)
    return NumberKind::NonNumeric;
  if (isNull(value))
    return NumberKind::NullSentinel;
  if (out)
    *out = value;
  return NumberKind::Value;
}

// 仅认规范词表，不从空单位/未知单位推断。F 保留既有消费面别名。
inline constexpr std::array<const char *, 5> kMetreAliases = {
    "M", "METER", "METERS", "METRE", "METRES"};
inline constexpr std::array<const char *, 4> kFootAliases = {
    "FT", "F", "FOOT", "FEET"};

inline std::optional<double> depthScaleToMetres(const QString &unit)
{
  const QString key = unit.trimmed().toUpper();
  for (const char *alias : kMetreAliases)
    if (key == QLatin1String(alias))
      return 1.0;
  for (const char *alias : kFootAliases)
    if (key == QLatin1String(alias))
      return 0.3048;
  return std::nullopt;
}
} // namespace paleo::wellnumeric
