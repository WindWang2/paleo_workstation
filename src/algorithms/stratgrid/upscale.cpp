// 层：数据
#include "upscale.h"
#include "../algoerrors_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::stratgrid
{
namespace
{

using paleo::algo_detail::setError;

struct Acc
{
  double wsum = 0;
  double wlen = 0;
  double bestLen = -1;
  int columnI = -1;
  int columnJ = -1;
  std::vector<double> samples;
};

bool columnAt(const ZoneGrid &grid, double x, double y, int *i, int *j)
{
  const double u = (x - grid.originX) / grid.dx;
  const double v = (y - grid.originY) / grid.dy;
  const int ii = static_cast<int>(std::floor(u));
  const int jj = static_cast<int>(std::floor(v));
  if (ii < 0 || jj < 0 || ii >= grid.ni || jj >= grid.nj)
    return false;
  *i = ii;
  *j = jj;
  return true;
}

void noteColumn(Acc *acc, int i, int j, double len)
{
  if (len > acc->bestLen)
  {
    acc->bestLen = len;
    acc->columnI = i;
    acc->columnJ = j;
  }
}

bool integrateLinear(const std::vector<CurvePoint> &curve, double a, double b, double *integ,
                     double *covered)
{
  if (a > b)
    std::swap(a, b);
  *integ = 0;
  *covered = 0;
  if (curve.size() < 2 || !(b > a))
    return false;
  for (std::size_t n = 0; n + 1 < curve.size(); ++n)
  {
    const CurvePoint &p = curve[n];
    const CurvePoint &q = curve[n + 1];
    if (!std::isfinite(p.value) || !std::isfinite(q.value) || !(q.md > p.md))
      continue;
    const double lo = std::max(a, p.md);
    const double hi = std::min(b, q.md);
    if (!(hi > lo))
      continue;
    const double t0 = (lo - p.md) / (q.md - p.md);
    const double t1 = (hi - p.md) / (q.md - p.md);
    const double v0 = p.value + t0 * (q.value - p.value);
    const double v1 = p.value + t1 * (q.value - p.value);
    *integ += 0.5 * (v0 + v1) * (hi - lo);
    *covered += hi - lo;
  }
  return *covered > 0.0;
}

void addGridCrossings(double u0, double du, std::vector<double> *ts)
{
  if (std::fabs(du) < 1e-15)
    return;
  const double u1 = u0 + du;
  const int lo = static_cast<int>(std::floor(std::min(u0, u1)));
  const int hi = static_cast<int>(std::floor(std::max(u0, u1)));
  for (int n = lo + 1; n <= hi; ++n)
  {
    const double t = (static_cast<double>(n) - u0) / du;
    if (t > 1e-12 && t < 1.0 - 1e-12)
      ts->push_back(t);
  }
}

double lerp(double a, double b, double t) { return a + t * (b - a); }

// 非有限 MD 破坏严格弱序，不能进排序。同一 MD 上若有有限值，丢掉非有限值，
// 只留稳定顺序下的最后一个有限值，加权括号才确定。
std::vector<CurvePoint> prepareCurve(const std::vector<CurvePoint> &raw)
{
  std::vector<CurvePoint> curve;
  curve.reserve(raw.size());
  for (const CurvePoint &point : raw)
  {
    if (std::isfinite(point.md))
      curve.push_back(point);
  }
  std::stable_sort(curve.begin(), curve.end(),
                   [](const CurvePoint &a, const CurvePoint &b) { return a.md < b.md; });

  std::vector<CurvePoint> collapsed;
  collapsed.reserve(curve.size());
  for (std::size_t i = 0; i < curve.size();)
  {
    std::size_t end = i + 1;
    while (end < curve.size() && curve[end].md == curve[i].md)
      ++end;
    std::size_t lastFinite = end;
    for (std::size_t k = i; k < end; ++k)
    {
      if (std::isfinite(curve[k].value))
        lastFinite = k;
    }
    if (lastFinite < end)
      collapsed.push_back(curve[lastFinite]);
    else
      collapsed.push_back(curve[i]);
    i = end;
  }
  return collapsed;
}

void accumulateWell(const ZoneGrid &grid, const WellCurve &well, std::vector<Acc> *layers)
{
  const int nk = grid.nk;
  const std::vector<CurvePoint> curve = prepareCurve(well.curve);

  const auto atT = [&](const WellStation &a, const WellStation &b, double t, double *md, double *x,
                       double *y, double *z) {
    *md = lerp(a.md, b.md, t);
    *x = lerp(a.x, b.x, t);
    *y = lerp(a.y, b.y, t);
    *z = lerp(a.z, b.z, t);
  };

  for (std::size_t s = 0; s + 1 < well.stations.size(); ++s)
  {
    const WellStation &a = well.stations[s];
    const WellStation &b = well.stations[s + 1];
    std::vector<double> ts{0.0, 1.0};
    const double u0 = (a.x - grid.originX) / grid.dx;
    const double du = (b.x - a.x) / grid.dx;
    const double v0 = (a.y - grid.originY) / grid.dy;
    const double dv = (b.y - a.y) / grid.dy;
    addGridCrossings(u0, du, &ts);
    addGridCrossings(v0, dv, &ts);
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end(),
                         [](double p, double q) { return std::fabs(p - q) < 1e-12; }),
             ts.end());

    for (std::size_t p = 0; p + 1 < ts.size(); ++p)
    {
      const double t0 = ts[p];
      const double t1 = ts[p + 1];
      if (!(t1 > t0))
        continue;
      double mdM = 0, xM = 0, yM = 0, zM = 0;
      atT(a, b, 0.5 * (t0 + t1), &mdM, &xM, &yM, &zM);
      int colI = 0, colJ = 0;
      if (!columnAt(grid, xM, yM, &colI, &colJ) || !grid.columnLive(colI, colJ))
        continue;
      const int c = grid.columnIndex(colI, colJ);
      const double top = grid.topZ[static_cast<std::size_t>(c)];
      const double bot = grid.botZ[static_cast<std::size_t>(c)];
      const double span = bot - top;
      if (!(span > kThicknessEps))
        continue;

      double md0 = 0, x0 = 0, y0 = 0, z0 = 0;
      double md1 = 0, x1 = 0, y1 = 0, z1 = 0;
      atT(a, b, t0, &md0, &x0, &y0, &z0);
      atT(a, b, t1, &md1, &x1, &y1, &z1);
      const double s0 = (z0 - top) / span;
      const double s1 = (z1 - top) / span;

      std::vector<double> splits{t0, t1};
      if (std::fabs(s1 - s0) > 1e-15)
      {
        const auto addEdge = [&](double sEdge) {
          const double side0 = s0 - sEdge;
          const double side1 = s1 - sEdge;
          if (side0 * side1 > 0.0)
            return;
          const double t = t0 + (sEdge - s0) / (s1 - s0) * (t1 - t0);
          if (t > t0 + 1e-12 && t < t1 - 1e-12)
            splits.push_back(t);
        };
        addEdge(0.0);
        addEdge(1.0);
        for (int k = 1; k < nk; ++k)
          addEdge(static_cast<double>(k) / static_cast<double>(nk));
      }
      std::sort(splits.begin(), splits.end());
      splits.erase(std::unique(splits.begin(), splits.end(),
                               [](double p, double q) { return std::fabs(p - q) < 1e-12; }),
                   splits.end());

      for (std::size_t q = 0; q + 1 < splits.size(); ++q)
      {
        const double p0 = splits[q];
        const double p1 = splits[q + 1];
        if (!(p1 > p0))
          continue;
        double mdA = 0, xa = 0, ya = 0, za = 0;
        double mdB = 0, xb = 0, yb = 0, zb = 0;
        atT(a, b, p0, &mdA, &xa, &ya, &za);
        atT(a, b, p1, &mdB, &xb, &yb, &zb);
        const double sm = ((0.5 * (za + zb)) - top) / span;
        const int layer = layerOfFraction(sm, nk);
        if (layer < 0)
          continue;
        Acc &acc = (*layers)[static_cast<std::size_t>(layer)];
        double integ = 0, covered = 0;
        if (integrateLinear(curve, mdA, mdB, &integ, &covered))
        {
          acc.wsum += integ;
          acc.wlen += covered;
          noteColumn(&acc, colI, colJ, covered);
        }
      }
    }
  }

  for (const CurvePoint &sample : curve)
  {
    if (!std::isfinite(sample.value) || well.stations.empty())
      continue;
    if (sample.md < well.stations.front().md - 1e-9 || sample.md > well.stations.back().md + 1e-9)
      continue;
    std::size_t hi = 1;
    while (hi + 1 < well.stations.size() && well.stations[hi].md < sample.md)
      ++hi;
    if (well.stations.size() == 1)
      hi = 0;
    const WellStation &a = well.stations[hi == 0 ? 0 : hi - 1];
    const WellStation &b = well.stations[std::min(hi, well.stations.size() - 1)];
    double x = a.x, y = a.y, z = a.z;
    if (b.md > a.md)
    {
      const double t = std::clamp((sample.md - a.md) / (b.md - a.md), 0.0, 1.0);
      x = lerp(a.x, b.x, t);
      y = lerp(a.y, b.y, t);
      z = lerp(a.z, b.z, t);
    }
    int colI = 0, colJ = 0;
    if (!columnAt(grid, x, y, &colI, &colJ) || !grid.columnLive(colI, colJ))
      continue;
    const int c = grid.columnIndex(colI, colJ);
    const double top = grid.topZ[static_cast<std::size_t>(c)];
    const double bot = grid.botZ[static_cast<std::size_t>(c)];
    const double span = bot - top;
    if (!(span > kThicknessEps))
      continue;
    const int layer = layerOfFraction((z - top) / span, nk);
    if (layer < 0)
      continue;
    Acc &acc = (*layers)[static_cast<std::size_t>(layer)];
    acc.samples.push_back(sample.value);
    if (acc.columnI < 0)
      noteColumn(&acc, colI, colJ, 0.0);
  }
}

double medianOf(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  if (n % 2 == 1)
    return values[n / 2];
  return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

double modeOf(const std::vector<double> &values)
{
  std::vector<std::pair<double, int>> bins;
  for (double v : values)
  {
    bool found = false;
    for (auto &bin : bins)
    {
      if (bin.first == v)
      {
        ++bin.second;
        found = true;
        break;
      }
    }
    if (!found)
      bins.emplace_back(v, 1);
  }
  int best = -1;
  double chosen = 0;
  for (const auto &bin : bins)
  {
    if (bin.second > best || (bin.second == best && bin.first < chosen))
    {
      best = bin.second;
      chosen = bin.first;
    }
  }
  return chosen;
}

} // namespace

QString aggregatorId(Aggregator aggregator)
{
  switch (aggregator)
  {
  case Aggregator::Mean:
    return QStringLiteral("mean");
  case Aggregator::ThicknessWeightedMean:
    return QStringLiteral("thickness-weighted-mean");
  case Aggregator::Median:
    return QStringLiteral("median");
  case Aggregator::Mode:
    return QStringLiteral("mode");
  }
  return QStringLiteral("unknown");
}

bool upscaleWells(const ZoneGrid &grid, const std::vector<WellCurve> &wells, Aggregator aggregator,
                  UpscaleTable *out, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("粗化输出为空"));
    return false;
  }
  const auto fail = [&](const QString &text) {
    *out = UpscaleTable{};
    setError(error, text);
    return false;
  };
  *out = UpscaleTable{};
  if (grid.nk < 1 || grid.liveColumns < 1)
    return fail(QStringLiteral("格架没有活柱，无法粗化"));
  out->nWells = static_cast<int>(wells.size());
  out->nLayers = grid.nk;
  out->cells.resize(static_cast<std::size_t>(out->nWells * out->nLayers));
  const double kNan = std::numeric_limits<double>::quiet_NaN();
  for (LayerValue &cell : out->cells)
  {
    cell.value = kNan;
    cell.hasValue = false;
  }

  for (int w = 0; w < out->nWells; ++w)
  {
    const WellCurve &well = wells[static_cast<std::size_t>(w)];
    if (well.stations.empty())
      return fail(QStringLiteral("井 %1 没有轨迹站").arg(well.wellId));
    for (std::size_t s = 1; s < well.stations.size(); ++s)
    {
      if (!(well.stations[s].md > well.stations[s - 1].md))
        return fail(QStringLiteral("井 %1 轨迹 MD 未严格递增").arg(well.wellId));
    }
    std::vector<Acc> layers(static_cast<std::size_t>(grid.nk));
    accumulateWell(grid, well, &layers);
    for (int k = 0; k < grid.nk; ++k)
    {
      const Acc &acc = layers[static_cast<std::size_t>(k)];
      LayerValue &cell = out->at(w, k);
      cell.supportLength = acc.wlen;
      cell.columnI = acc.columnI;
      cell.columnJ = acc.columnJ;
      switch (aggregator)
      {
      case Aggregator::Mean:
        if (!acc.samples.empty())
        {
          double sum = 0;
          for (double v : acc.samples)
            sum += v;
          cell.value = sum / static_cast<double>(acc.samples.size());
          cell.hasValue = true;
        }
        break;
      case Aggregator::Median:
        if (!acc.samples.empty())
        {
          cell.value = medianOf(acc.samples);
          cell.hasValue = true;
        }
        break;
      case Aggregator::Mode:
        if (!acc.samples.empty())
        {
          cell.value = modeOf(acc.samples);
          cell.hasValue = true;
        }
        break;
      case Aggregator::ThicknessWeightedMean:
        if (acc.wlen > 0.0)
        {
          cell.value = acc.wsum / acc.wlen;
          cell.hasValue = true;
        }
        break;
      }
    }
  }
  return true;
}

} // namespace paleo::stratgrid
