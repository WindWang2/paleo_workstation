// 层：数据
#include "wellsection.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace wellsection {

double TimeDepth::twtAt(double md) const {
  const double t = model.DepthToTwtMs(md) + shiftMs;
  return std::isfinite(t) ? t : qQNaN();
}

bool Well::hasCoordinates() const {
  return std::isfinite(x) && std::isfinite(y);
}

double Well::topMd(const QString &name) const {
  for (const Top &t : tops)
    if (t.name == name)
      return t.md;
  return qQNaN();
}

const Curve *Well::curve(const QString &mnemonic) const {
  for (const Curve &c : curves)
    if (c.mnemonic.compare(mnemonic, Qt::CaseInsensitive) == 0)
      return &c;
  return nullptr;
}

QVector<Zone> zones(const Well &w, double bottomMd) {
  QVector<Zone> out;
  for (int i = 0; i < w.tops.size(); ++i) {
    const double base =
        i + 1 < w.tops.size() ? w.tops[i + 1].md : bottomMd;
    if (!(base > w.tops[i].md))
      continue;
    out.push_back({w.tops[i].name, w.tops[i].md, base});
  }
  return out;
}

QVector<Link> links(const Well &left, const Well &right) {
  QVector<Link> out;
  for (const Top &t : left.tops) {
    const double r = right.topMd(t.name);
    if (!std::isfinite(r))
      continue;
    bool dup = false;
    for (const Link &l : out)
      if (l.name == t.name) {
        dup = true;
        break;
      }
    if (!dup)
      out.push_back({t.name, t.md, r});
  }
  return out;
}

LinkOverride makeLinkOverride(const QString &aId, const QString &bId,
                              const QString &topName, bool connected) {
  LinkOverride o;
  o.topName = topName;
  o.connected = connected;
  if (aId <= bId) {
    o.leftWellId = aId;
    o.rightWellId = bId;
  } else {
    o.leftWellId = bId;
    o.rightWellId = aId;
  }
  return o;
}

bool linkConnected(const QVector<LinkOverride> &overrides, const QString &aId,
                   const QString &bId, const QString &topName) {
  const QString lo = qMin(aId, bId), hi = qMax(aId, bId);
  for (const LinkOverride &o : overrides)
    if (o.leftWellId == lo && o.rightWellId == hi && o.topName == topName)
      return o.connected;
  return true;
}

QVector<double> gapWidthsFor(const QVector<Well> &wells, SpacingMode mode,
                             double totalGap, double minGap, double maxGap) {
  const int gaps = wells.size() - 1;
  QVector<double> out;
  if (gaps < 1)
    return out;
  out.fill(0.0, gaps);
  if (mode == SpacingMode::Equal || totalGap <= 0.0) {
    out.fill(qBound(minGap, totalGap / gaps, maxGap), gaps);
    return out;
  }
  // 相邻井地图距离（勾股；缺坐标段先记 NaN）。
  QVector<double> dist(gaps, qQNaN());
  int known = 0;
  for (int i = 0; i < gaps; ++i)
    if (wells[i].hasCoordinates() && wells[i + 1].hasCoordinates()) {
      const double dx = wells[i + 1].x - wells[i].x;
      const double dy = wells[i + 1].y - wells[i].y;
      dist[i] = std::sqrt(dx * dx + dy * dy);
      ++known;
    }
  if (known == 0) {
    out.fill(qBound(minGap, totalGap / gaps, maxGap), gaps);
    return out;
  }
  // 缺段用已知段中位距离补（保守：非零、抗离群）。
  if (known < gaps) {
    QVector<double> sorted;
    for (double d : dist)
      if (std::isfinite(d))
        sorted << d;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted.isEmpty() ? 1.0 : sorted.at(sorted.size() / 2);
    for (double &d : dist)
      if (!std::isfinite(d))
        d = qMax(1e-6, med);
  }
  double sum = 0.0;
  for (double d : dist)
    sum += d;
  for (int i = 0; i < gaps; ++i)
    out[i] = qBound(minGap, totalGap * dist[i] / sum, maxGap);
  return out;
}

QStringList orderedTopNames(const QVector<Well> &wells) {
  QStringList out;
  for (const Well &w : wells) {
    for (int i = 0; i < w.tops.size(); ++i) {
      const QString &name = w.tops[i].name;
      if (out.contains(name))
        continue;
      int pos = 0; // 找不到较浅锚 → 插最前
      for (int j = i - 1; j >= 0; --j) {
        const int at = out.indexOf(w.tops[j].name);
        if (at >= 0) {
          pos = at + 1;
          break;
        }
      }
      out.insert(pos, name);
    }
  }
  return out;
}

double flattenOffset(const Well &w, const QString &flattenTop) {
  if (flattenTop.isEmpty())
    return 0.0;
  const double md = w.topMd(flattenTop);
  return std::isfinite(md) ? md : 0.0;
}

double datumOffset(const Well &w, const Datum &d) {
  switch (d.mode) {
  case DatumMode::Flatten:
    return flattenOffset(w, d.flattenTop);
  case DatumMode::Elevation:
    return std::isfinite(w.kb) ? w.kb : 0.0;
  case DatumMode::Depth:
    break;
  }
  return 0.0;
}

QString datumLabel(DatumMode mode) {
  switch (mode) {
  case DatumMode::Elevation:
    return QStringLiteral("海拔 m");
  case DatumMode::Flatten:
    return QStringLiteral("拉平 m");
  case DatumMode::Depth:
    break;
  }
  return QStringLiteral("井深 m");
}

DepthWindow depthWindow(const QVector<Well> &wells, const QString &flattenTop) {
  return depthWindow(wells, Datum{DatumMode::Flatten, flattenTop});
}

DepthWindow depthWindow(const QVector<Well> &wells, const Datum &datum) {
  double top = qQNaN(), base = qQNaN();
  bool anyTops = false;
  for (const Well &w : wells)
    anyTops = anyTops || !w.tops.isEmpty();
  for (const Well &w : wells) {
    const double off = datumOffset(w, datum);
    double lo = qQNaN(), hi = qQNaN();
    if (anyTops) {
      for (const Top &t : w.tops) {
        lo = std::isfinite(lo) ? std::min(lo, t.md) : t.md;
        hi = std::isfinite(hi) ? std::max(hi, t.md) : t.md;
      }
      if (!std::isfinite(lo))
        continue;
    } else {
      for (const Curve &c : w.curves)
        for (const float d : c.depths)
          if (std::isfinite(d)) {
            lo = std::isfinite(lo) ? std::min(lo, double(d)) : double(d);
            hi = std::isfinite(hi) ? std::max(hi, double(d)) : double(d);
          }
      if (!std::isfinite(lo))
        continue;
    }
    lo -= off;
    hi -= off;
    top = std::isfinite(top) ? std::min(top, lo) : lo;
    base = std::isfinite(base) ? std::max(base, hi) : hi;
  }
  DepthWindow win;
  if (!std::isfinite(top) || !std::isfinite(base)) {
    win.top = 0.0;
    win.base = 100.0;
    return win;
  }
  if (base - top < 1.0) {
    const double mid = (top + base) * 0.5;
    top = mid - 0.5;
    base = mid + 0.5;
  }
  const double pad = std::max(5.0, 0.04 * (base - top));
  win.top = top - pad;
  win.base = base + pad;
  return win;
}

bool Interval::valid() const { return std::isfinite(topMd); }
bool Interval::hasBase() const {
  return valid() && std::isfinite(baseMd) && baseMd > topMd;
}

Interval formationInterval(const Well &w, const QString &activeTop,
                           const QString &baseTop) {
  Interval out;
  if (activeTop.isEmpty())
    return out;
  const double top = w.topMd(activeTop);
  if (!std::isfinite(top))
    return out;
  out.topMd = top;
  if (!baseTop.isEmpty()) {
    const double base = w.topMd(baseTop);
    if (std::isfinite(base) && base > top)
      out.baseMd = base;
  }
  return out;
}

namespace {
QString csvCell(const QString &s) {
  if (!s.contains(QLatin1Char(',')) && !s.contains(QLatin1Char('"')) &&
      !s.contains(QLatin1Char('\n')) && !s.contains(QLatin1Char('\r')))
    return s;
  QString out;
  out.reserve(s.size() + 2);
  out += QLatin1Char('"');
  for (const QChar c : s) {
    if (c == QLatin1Char('"'))
      out += QLatin1Char('"');
    out += c;
  }
  out += QLatin1Char('"');
  return out;
}
} // namespace

TopsTable topsTable(const QVector<Well> &wells, const Datum &datum) {
  TopsTable t;
  if (datum.mode == DatumMode::Flatten && !datum.flattenTop.isEmpty())
    t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                QStringLiteral("MD(m)"),
                QStringLiteral("基准面"), datum.flattenTop};
  else
    t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                QStringLiteral("MD(m)"), QStringLiteral("基准面"),
                datumLabel(datum.mode)};
  t.rows.reserve(wells.size() * 8);
  for (const Well &w : wells)
    for (const Top &top : w.tops) {
      if (!std::isfinite(top.md))
        continue;
      t.rows.push_back({w.name, top.name,
                        QString::number(top.md, 'f', 2)});
    }
  return t;
}

QString TopsTable::csv() const {
  QStringList lines;
  QStringList head;
  for (const QString &h : header)
    head << csvCell(h);
  lines << head.join(QLatin1Char(','));
  for (const QStringList &r : rows) {
    QStringList cells;
    for (const QString &c : r)
      cells << csvCell(c);
    lines << cells.join(QLatin1Char(','));
  }
  return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}


QVector<double> wellPathFractions(const QVector<Well> &wells) {
  QVector<double> out;
  if (wells.isEmpty())
    return out;
  for (const Well &w : wells)
    if (!w.hasCoordinates())
      return QVector<double>();
  QVector<double> cum(wells.size(), 0.0);
  double total = 0.0;
  for (int i = 1; i < wells.size(); ++i) {
    const double dx = wells[i].x - wells[i - 1].x;
    const double dy = wells[i].y - wells[i - 1].y;
    total += std::sqrt(dx * dx + dy * dy);
    cum[i] = total;
  }
  if (!(total > 1e-9))
    return QVector<double>();
  for (double c : cum)
    out << c / total;
  return out;
}

QVector<LithoInterval> inferSandShale(const Curve &gr, double cutoff,
                                      double minThicknessM) {
  const int n = qMin(gr.depths.size(), gr.values.size());
  QVector<LithoInterval> out;
  if (n == 0)
    return out;
  // 升序化：只用有限深度判走向（NaN 深度样本视为断点）。
  double firstFinite = qQNaN(), lastFinite = qQNaN();
  for (int i = 0; i < n; ++i) {
    if (std::isfinite(gr.depths[i])) {
      if (!std::isfinite(firstFinite))
        firstFinite = gr.depths[i];
      lastFinite = gr.depths[i];
    }
  }
  const bool descending =
      std::isfinite(firstFinite) && std::isfinite(lastFinite) &&
      firstFinite > lastFinite;
  auto idx = [&](int k) { return descending ? n - 1 - k : k; };
  for (int b = 0; b < n;) {
    const int i0 = idx(b);
    if (!std::isfinite(gr.depths[i0]) || !std::isfinite(gr.values[i0])) {
      ++b;
      continue;
    }
    int e = b; // 连续有限块 [b, e]
    while (e + 1 < n && std::isfinite(gr.depths[idx(e + 1)]) &&
           std::isfinite(gr.values[idx(e + 1)]))
      ++e;
    // 块内同类 run → 原始段（异类间中点分界，块端取样点深度）。
    QVector<LithoInterval> block;
    for (int k = b; k <= e;) {
      const bool sand = gr.values[idx(k)] < cutoff;
      int r = k;
      while (r + 1 <= e && (gr.values[idx(r + 1)] < cutoff) == sand)
        ++r;
      LithoInterval iv;
      iv.sand = sand;
      iv.topMd = k == b ? gr.depths[idx(k)]
                        : (gr.depths[idx(k - 1)] + gr.depths[idx(k)]) * 0.5;
      iv.baseMd = r == e ? gr.depths[idx(r)]
                         : (gr.depths[idx(r)] + gr.depths[idx(r + 1)]) * 0.5;
      block.push_back(iv);
      k = r + 1;
    }
    // 薄段并入前段（块首并入后段），再合并相邻同类；NaN 断块之间不并。
    for (int i = 0; i < block.size(); ++i) {
      if (block[i].baseMd - block[i].topMd >= minThicknessM)
        continue;
      if (block.size() == 1)
        break;
      if (i == 0) {
        block[1].topMd = block[0].topMd;
        block.remove(0);
        --i;
      } else {
        block[i - 1].baseMd = block[i].baseMd;
        block.remove(i);
        --i;
      }
    }
    for (int i = 1; i < block.size(); ++i) {
      if (block[i].sand == block[i - 1].sand) {
        block[i - 1].baseMd = block[i].baseMd;
        block.remove(i);
        --i;
      }
    }
    out += block;
    b = e + 1;
  }
  return out;
}

bool SeismicGap::valid() const {
  return reason.isEmpty() && columns > 0 && samples > 0 && stepMs > 0.0 &&
         values.size() == static_cast<size_t>(columns) * samples;
}

float SeismicGap::sampleAt(double columnFrac, double twtMs) const {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  if (!valid() || !std::isfinite(columnFrac) || !std::isfinite(twtMs) ||
      columnFrac < 0.0 || columnFrac > 1.0)
    return nan;
  const int col = static_cast<int>(std::lround(columnFrac * (columns - 1)));
  const double row = (twtMs - startMs) / stepMs;
  if (row < 0.0 || row > samples - 1)
    return nan;
  const int r0 = static_cast<int>(std::floor(row));
  const int r1 = std::min(r0 + 1, samples - 1);
  const double t = row - r0;
  const float v0 = values[size_t(r0) * columns + col];
  const float v1 = values[size_t(r1) * columns + col];
  if (!std::isfinite(v0) || !std::isfinite(v1))
    return nan;
  return v0 + (v1 - v0) * static_cast<float>(t);
}

bool SeismicStrip::anyValid() const {
  for (const SeismicGap &g : gaps)
    if (g.valid())
      return true;
  return false;
}

float adaptiveClip(const QVector<SeismicGap> &gaps, double percentile) {
  qint64 total = 0;
  for (const SeismicGap &g : gaps)
    if (g.valid())
      total += static_cast<qint64>(g.values.size());
  if (total <= 0)
    return 1.0f;
  const qint64 stride = std::max<qint64>(1, (total + 199999) / 200000);
  std::vector<float> sample;
  sample.reserve(size_t(std::min<qint64>(total / stride + 1, 200001)));
  for (const SeismicGap &g : gaps) {
    if (!g.valid())
      continue;
    for (size_t i = 0; i < g.values.size(); i += size_t(stride)) {
      const float v = g.values[i];
      if (std::isfinite(v))
        sample.push_back(std::fabs(v));
    }
  }
  if (sample.empty())
    return 1.0f;
  const double p = std::isfinite(percentile) ? percentile : 0.99;
  size_t at = static_cast<size_t>(
      std::lround(p * static_cast<double>(sample.size() - 1)));
  at = std::min(at, sample.size() - 1);
  std::nth_element(sample.begin(), sample.begin() + at, sample.end());
  const float clip = sample[at];
  return std::isfinite(clip) && clip > 0.0f ? clip : 1.0f;
}

double gapTwtMs(const Well &a, double offA, const Well &b, double offB,
                double f, double displayDepth) {
  if (!a.timeDepth || !b.timeDepth)
    return qQNaN();
  const double tA = a.timeDepth->twtAt(displayDepth + offA);
  const double tB = b.timeDepth->twtAt(displayDepth + offB);
  if (!std::isfinite(tA) || !std::isfinite(tB))
    return qQNaN();
  return (1.0 - f) * tA + f * tB;
}

} // namespace wellsection
