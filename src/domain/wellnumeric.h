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

// 曲线/属性值的空值口径（C.4 / #589）：精确哨兵之外再加一条范围带。LAS /
// SEG-Y 的占位写法不限于上面那四个精确值（-99995 / -1e30 / 1e30 都在野外
// 流传），范围带把它们一并归缺失。
// 坐标/海拔等合法负值域**不得**改用这个口径——那里仍用 isNull 精确匹配
// （见文件头注释：范围比较会把合法负坐标/海拔抹成缺失）。
// 消费点（welllogfacies 曲线插值、workflow/sectionworkbench 的 min/max
// 统计）一律走这里，不再各自硬编码阈值——同一数值不得在一条链判缺失、
// 另一条链判数据。
inline constexpr double kCurveNullBelow = -99990.0;
inline constexpr double kCurveNullAbove = 1e29;

inline bool isNullCurveValue(double value)
{
  return !std::isfinite(value) || isNull(value) || value <= kCurveNullBelow ||
         value >= kCurveNullAbove;
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

// 全工程唯一的深度单位词表（PW-029）：LasParser::depthUnitToMeters 与
// welllogfacies 的 depthUnitToMeters 均委托到这里，只在「空单位」策略上由
// 调用方显式决定。LAS 头里常见的 "M." / "FT." 尾点在此统一剥掉。
inline std::optional<double> depthScaleToMetres(const QString &unit)
{
  QString key = unit.trimmed().toUpper();
  while (key.endsWith(QLatin1Char('.')))
    key.chop(1);
  key = key.trimmed();
  for (const char *alias : kMetreAliases)
    if (key == QLatin1String(alias))
      return 1.0;
  for (const char *alias : kFootAliases)
    if (key == QLatin1String(alias))
      return 0.3048;
  return std::nullopt;
}

// 声波时差单位 → µs/m 乘数。µs/ft 井（如 US/FT）不换算会给 3.28× 速度误差，
// 必须显式换算；空单位 = 遗留数据无元数据，按 µs/m 假设（乘 1，调用方注明）；
// 未知非空单位 nullopt——调用方拒绝该曲线，不猜。
inline std::optional<double> acousticScaleToUsPerM(const QString &unit)
{
  const QString key = unit.trimmed().toUpper();
  if (key.isEmpty())
    return 1.0;
  if (key == QLatin1String("US/M") || key == QLatin1String("USEC/M") ||
      key == QLatin1String("MICROSEC/M") || key == QLatin1String("US/METER"))
    return 1.0;
  if (key == QLatin1String("US/FT") || key == QLatin1String("USEC/FT") ||
      key == QLatin1String("MICROSEC/FT") || key == QLatin1String("US/FOOT"))
    return 1.0 / 0.3048;
  return std::nullopt;
}

// 密度单位 → g/cc 乘数（阻抗 Z=ρ·V 的 ρ 口径）。空单位同上按 g/cc 假设。
inline std::optional<double> densityScaleToGPerCc(const QString &unit)
{
  const QString key = unit.trimmed().toUpper();
  if (key.isEmpty())
    return 1.0;
  if (key == QLatin1String("G/CC") || key == QLatin1String("G/CM3") ||
      key == QLatin1String("G/C3") || key == QLatin1String("GCM3") ||
      key == QLatin1String("G/CC3") || key == QLatin1String("GM/CC") ||
      key == QLatin1String("GM/C3"))
    return 1.0;
  if (key == QLatin1String("KG/M3") || key == QLatin1String("KG/M^3") ||
      key == QLatin1String("KGM3"))
    return 0.001;
  return std::nullopt;
}
} // namespace paleo::wellnumeric
