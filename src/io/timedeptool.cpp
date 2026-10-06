// 层：数据
#include "timedeptool.h"
#include "../domain/wellnumeric.h"

#include <cmath>

namespace
{

  // 取参与插值的 (key, time) 对：key=TVD 或 MD；共享哨兵/缺列/非有限的行剔除。
  // 解析端已滤，这里再滤一遍兜住手工构造的表（mappingworkflow/
  // mapversioncontroller 从 TdSample 回填）。
  // 保持文件顺序——不排序（PROJECT_AREA_PLAN §3）。
  struct KtRow
  {
    double key = 0.0, t = 0.0;
  };
  QVector<KtRow> collectKeys(const TimeDepthTable &td, bool useMd, int *ignored)
  {
    QVector<KtRow> rows;
    rows.reserve(td.rows.size());
    for (const TdRow &r : td.rows)
    {
      const double key = useMd ? r.md : r.tvd;
      if ((useMd ? !r.hasMd : !r.hasTvd) || !paleo::wellnumeric::isUsable(key) ||
          !paleo::wellnumeric::isUsable(r.timeMs)) // 缺列、非有限值或共享哨兵 → 不进插值
      {
        ++*ignored;
        continue;
      }
      rows.append({key, r.timeMs});
    }
    return rows;
  }
} // namespace

namespace TimeDepthTool
{
  TdResult interpolateTimeMs(const TimeDepthTable &td, double depth, bool useMd)
  {
    TdResult out;
    const QVector<KtRow> rows = collectKeys(td, useMd, &out.ignoredRows);
    if (rows.size() < 2) // 可用样点不足两个 → 无时深表
    {
      out.status = TdStatus::NoTable;
      return out;
    }
    // 查找列必须按文件顺序严格递增；否则不插值 → 时深表无序。
    for (int i = 1; i < rows.size(); ++i)
    {
      if (!(rows.at(i).key > rows.at(i - 1).key))
      {
        out.status = TdStatus::NonMonotonic;
        return out;
      }
    }
    // 范围之外不夹取、不外推（NaN 深度也走这里）→ 超出时深表。
    if (!(depth >= rows.first().key && depth <= rows.last().key))
    {
      out.status = TdStatus::OutOfRange;
      return out;
    }
    // 文件顺序相邻两样点间线性插值（首末样点本身经 f=0/1 命中）。
    for (int i = 1; i < rows.size(); ++i)
    {
      if (rows.at(i).key >= depth)
      {
        const KtRow &a = rows.at(i - 1);
        const KtRow &b = rows.at(i);
        const double f = (depth - a.key) / (b.key - a.key);
        out.timeMs = a.t + f * (b.t - a.t);
        out.status = TdStatus::Ok;
        return out;
      }
    }
    out.timeMs = rows.last().t; // depth==末样点时上面 f=1 已命中，此为兜底
    out.status = TdStatus::Ok;
    return out;
  }

  TdDepthResult interpolateDepthAtTimeMs(const TimeDepthTable &td,
                                         double timeMs, bool preferMd)
  {
    // (time, depth) 对：沿用 collectKeys 的行过滤与文件顺序契约
    struct TDrow
    {
      double t = 0.0, d = 0.0;
    };
    TdDepthResult out;
    QVector<TDrow> rows;
    rows.reserve(td.rows.size());
    for (const TdRow &r : td.rows)
    {
      const double d = preferMd ? r.md : r.tvd;
      if ((preferMd ? !r.hasMd : !r.hasTvd) || !paleo::wellnumeric::isUsable(d) ||
          !paleo::wellnumeric::isUsable(r.timeMs))
      {
        ++out.ignoredRows;
        continue;
      }
      rows.append({r.timeMs, d});
    }
    if (rows.size() < 2)
    {
      out.status = TdStatus::NoTable;
      return out;
    }
    for (int i = 1; i < rows.size(); ++i)
    {
      if (!(rows.at(i).t > rows.at(i - 1).t))
      {
        out.status = TdStatus::NonMonotonic;
        return out;
      }
    }
    if (!(timeMs >= rows.first().t && timeMs <= rows.last().t))
    {
      out.status = TdStatus::OutOfRange;
      return out;
    }
    for (int i = 1; i < rows.size(); ++i)
    {
      if (rows.at(i).t >= timeMs)
      {
        const TDrow &a = rows.at(i - 1);
        const TDrow &b = rows.at(i);
        const double f = (timeMs - a.t) / (b.t - a.t);
        out.depth = a.d + f * (b.d - a.d);
        out.status = TdStatus::Ok;
        return out;
      }
    }
    out.depth = rows.last().d;
    out.status = TdStatus::Ok;
    return out;
  }

  QString reasonText(TdStatus status)
  {
    switch (status)
    {
      case TdStatus::Ok:
        return QString();
      case TdStatus::NoTable:
        return QStringLiteral("无时深表");
      case TdStatus::OutOfRange:
        return QStringLiteral("超出时深表");
      case TdStatus::NonMonotonic:
        return QStringLiteral("时深表无序");
    }
    return QString();
  }
} // namespace TimeDepthTool
