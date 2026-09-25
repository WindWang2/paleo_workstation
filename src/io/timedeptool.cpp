#include "timedeptool.h"

#include <limits>

namespace
{
  // 取参与插值的 (key, time) 对：key=TVD 或 MD（-99999/缺列剔除），行按 key 升序。
  struct KtRow
  {
    double key = 0.0, t = 0.0;
  };
  QVector<KtRow> collectKeys(const TimeDepthTable &td, bool useMd)
  {
    QVector<KtRow> rows;
    rows.reserve(td.rows.size());
    for (const TdRow &r : td.rows)
    {
      const double key = useMd ? (r.hasMd ? r.md : -1.0) : (r.hasTvd ? r.tvd : -1.0);
      if (key < 0.0) // 缺列或 -99999
        continue;
      rows.append({key, r.timeMs});
    }
    std::sort(rows.begin(), rows.end(),
              [](const KtRow &a, const KtRow &b) { return a.key < b.key; });
    return rows;
  }
} // namespace

namespace TimeDepthTool
{
  double interpolateTimeMs(const TimeDepthTable &td, double depth, bool useMd, bool *ok)
  {
    const QVector<KtRow> rows = collectKeys(td, useMd);
    if (ok)
      *ok = false;
    if (rows.isEmpty())
      return 0.0;

    if (ok)
      *ok = true;
    if (depth <= rows.front().key)
      return rows.front().t; // 浅端外推
    if (depth >= rows.back().key)
      return rows.back().t; // 深端外推
    for (int i = 1; i < rows.size(); ++i)
    {
      if (rows.at(i).key >= depth)
      {
        const KtRow &a = rows.at(i - 1);
        const KtRow &b = rows.at(i);
        const double f = (depth - a.key) / (b.key - a.key);
        return a.t + f * (b.t - a.t);
      }
    }
    return rows.back().t;
  }
} // namespace TimeDepthTool
