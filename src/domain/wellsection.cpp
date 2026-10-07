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

double Well::tvdOf(double md) const {
  if (!surveyError.isEmpty())
    return qQNaN();
  if (!survey)
    return md; // 无链接 = 无测斜（NoSurvey）：几何恒等按 MD 绘制，TVD 域
               // 如实标注不可用（见 .h TvdStatus 三态注释）。
  return survey->tvdAt(md);
}

double Well::mdOf(double tvd) const {
  if (!surveyError.isEmpty())
    return qQNaN();
  if (!survey)
    return tvd;
  return survey->tvdToMd(tvd);
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
  const double equalW = qBound(minGap, totalGap / gaps, maxGap);
  if (mode == SpacingMode::Equal || totalGap <= 0.0) {
    out.fill(equalW, gaps);
    return out;
  }
  // 契约 v2：相邻两井都有坐标的缝才进比例分摊（真实地图距离）；任一端
  // 缺坐标的缝不参与比例轴——固定等距宽，已知段分摊剩余预算。
  QVector<double> dist(gaps, qQNaN());
  for (int i = 0; i < gaps; ++i)
    if (wells[i].hasCoordinates() && wells[i + 1].hasCoordinates()) {
      const double dx = wells[i + 1].x - wells[i].x;
      const double dy = wells[i + 1].y - wells[i].y;
      dist[i] = std::sqrt(dx * dx + dy * dy);
    }
  int unknown = 0;
  double sumKnown = 0.0;
  for (double d : dist) {
    if (std::isfinite(d))
      sumKnown += d;
    else
      ++unknown;
  }
  if (unknown == gaps || !(sumKnown > 1e-12)) {
    // 全缺坐标 / 已知段全零距（同平台井）：比例无意义，退化等距防 NaN
    // 毒化布局。
    out.fill(equalW, gaps);
    return out;
  }
  const double budget = std::max(0.0, totalGap - unknown * equalW);
  for (int i = 0; i < gaps; ++i) {
    if (!std::isfinite(dist[i]))
      out[i] = equalW;
    else
      out[i] = budget > 1e-12
                   ? qBound(minGap, budget * dist[i] / sumKnown, maxGap)
                   : equalW;
  }
  return out;
}

QStringList unpositionedWellNames(const QVector<Well> &wells,
                                  SpacingMode mode) {
  QStringList out;
  if (mode != SpacingMode::Proportional)
    return out;
  for (const Well &w : wells)
    if (!w.hasCoordinates())
      out << w.name;
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

double datumOffset(const Well &w, const Datum &d, DepthDomain domain) {
  switch (d.mode) {
  case DatumMode::Flatten: {
    if (domain == DepthDomain::TVD)
      return w.tvdOf(flattenOffset(w, d.flattenTop));
    return flattenOffset(w, d.flattenTop);
  }
  case DatumMode::Elevation:
    return std::isfinite(w.kb) ? w.kb : 0.0;
  case DatumMode::Depth:
    break;
  }
  return 0.0;
}

QString datumLabel(DatumMode mode, DepthDomain domain) {
  const bool tvd = domain == DepthDomain::TVD;
  switch (mode) {
  case DatumMode::Elevation:
    return tvd ? QStringLiteral("海拔垂深 m") : QStringLiteral("海拔 m");
  case DatumMode::Flatten:
    return QStringLiteral("拉平 m");
  case DatumMode::Depth:
    break;
  }
  return tvd ? QStringLiteral("垂深 m") : QStringLiteral("井深 m");
}

DepthWindow depthWindow(const QVector<Well> &wells, const QString &flattenTop) {
  return depthWindow(wells, Datum{DatumMode::Flatten, flattenTop});
}

DepthWindow depthWindow(const QVector<Well> &wells, const Datum &datum,
                        DepthDomain domain) {
  const bool tvd = domain == DepthDomain::TVD;
  // 域深：TVD 域先把 MD 值换到垂深（坏表井 NaN → 整井跳过，不下拽邻居）。
  const auto domainDepth = [&](const Well &w, double md) {
    return tvd ? w.tvdOf(md) : md;
  };
  bool anyTops = false;
  for (const Well &w : wells)
    anyTops = anyTops || !w.tops.isEmpty();
  auto wellRange = [&](const Well &w, double &lo, double &hi) -> bool {
    if (anyTops) {
      for (const Top &t : w.tops) {
        const double d = domainDepth(w, t.md);
        if (!std::isfinite(d))
          continue;
        lo = std::isfinite(lo) ? std::min(lo, d) : d;
        hi = std::isfinite(hi) ? std::max(hi, d) : d;
      }
    } else {
      for (const Curve &c : w.curves)
        for (const float f : c.depths) {
          if (!std::isfinite(f))
            continue;
          const double d = domainDepth(w, double(f));
          if (!std::isfinite(d))
            continue;
          lo = std::isfinite(lo) ? std::min(lo, d) : d;
          hi = std::isfinite(hi) ? std::max(hi, d) : d;
        }
    }
    return std::isfinite(lo);
  };
  double top = qQNaN(), base = qQNaN();
  for (const Well &w : wells) {
    double lo = qQNaN(), hi = qQNaN();
    if (!wellRange(w, lo, hi))
      continue;
    lo -= datumOffset(w, datum, domain);
    hi -= datumOffset(w, datum, domain);
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

TopsTable topsTable(const QVector<Well> &wells, const Datum &datum,
                    DepthDomain domain) {
  const bool tvd = domain == DepthDomain::TVD;
  TopsTable t;
  if (tvd) {
    if (datum.mode == DatumMode::Flatten && !datum.flattenTop.isEmpty())
      t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                  QStringLiteral("MD(m)"), QStringLiteral("TVD(m)"),
                  QStringLiteral("基准面"), datum.flattenTop};
    else
      t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                  QStringLiteral("MD(m)"), QStringLiteral("TVD(m)"),
                  QStringLiteral("基准面"), datumLabel(datum.mode, domain)};
  } else if (datum.mode == DatumMode::Flatten && !datum.flattenTop.isEmpty()) {
    t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                QStringLiteral("MD(m)"),
                QStringLiteral("基准面"), datum.flattenTop};
  } else {
    t.header = {QStringLiteral("井名"), QStringLiteral("顶名"),
                QStringLiteral("MD(m)"), QStringLiteral("基准面"),
                datumLabel(datum.mode, domain)};
  }
  t.rows.reserve(wells.size() * 8);
  for (const Well &w : wells)
    for (const Top &top : w.tops) {
      if (!std::isfinite(top.md))
        continue;
      QStringList row{w.name, top.name, QString::number(top.md, 'f', 2)};
      if (tvd) {
        const double tv = w.tvdOf(top.md);
        row << (std::isfinite(tv) ? QString::number(tv, 'f', 2)
                                  : QString()); // 坏表井留空不伪造
      }
      t.rows.push_back(row);
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


FencePlan planFence(const QVector<Well> &wells, int targetSections) {
  FencePlan plan;
  for (const Well &w : wells)
    if (!w.hasCoordinates()) {
      plan.status = FencePlan::Status::MissingCoords;
      return plan;
    }
  const int n = wells.size();
  if (n < 2) {
    plan.status = FencePlan::Status::TooFewWells;
    return plan;
  }
  if (targetSections < 1)
    targetSections = 1;
  targetSections = qMin(targetSections, n / 2);

  // PCA 主轴：2x2 协方差的最大特征向量（闭式解）。
  double cx = 0, cy = 0;
  for (const Well &w : wells) {
    cx += w.x;
    cy += w.y;
  }
  cx /= n;
  cy /= n;
  double sxx = 0, sxy = 0, syy = 0;
  for (const Well &w : wells) {
    const double dx = w.x - cx, dy = w.y - cy;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  // 对称 2x2 特征向量：theta = 0.5*atan2(2sxy, sxx−syy)。
  const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
  const double ux = std::cos(theta), uy = std::sin(theta);
  const double vx = -uy, vy = ux; // 垂直向（条带分割方向）

  // 按 v 排序等分条带（余数摊前几带），条带内按 u 单调。
  QVector<int> byV(n);
  for (int i = 0; i < n; ++i)
    byV[i] = i;
  const auto projV = [&](int i) {
    return (wells[i].x - cx) * vx + (wells[i].y - cy) * vy;
  };
  const auto projU = [&](int i) {
    return (wells[i].x - cx) * ux + (wells[i].y - cy) * uy;
  };
  std::sort(byV.begin(), byV.end(), [&](int a, int b) {
    const double va = projV(a), vb = projV(b);
    if (va != vb)
      return va < vb;
    const double ua = projU(a), ub = projU(b);
    if (ua != ub)
      return ua < ub;
    return wells[a].id < wells[b].id; // 投影重合时确定性输出
  });
  const int base = n / targetSections;
  const int extra = n % targetSections;
  int at = 0;
  for (int b = 0; b < targetSections; ++b) {
    const int count = base + (b < extra ? 1 : 0);
    QVector<int> band(byV.mid(at, count));
    at += count;
    std::sort(band.begin(), band.end(), [&](int a, int c) {
      const double ua = projU(a), uc = projU(c);
      if (ua != uc)
        return ua < uc;
      return wells[a].id < wells[c].id; // 投影重合时确定性输出
    });
    FenceSection sec;
    sec.id = QString::number(b + 1);
    // 剪草机：奇数条带（0 起）倒序——相邻条带走线端点相接不交叉。
    if (b % 2 == 1)
      std::reverse(band.begin(), band.end());
    for (int idx : band)
      sec.wellIds << wells[idx].id;
    plan.sections.push_back(sec);
  }
  // <2 井条带并入邻带（优先前带；首带并入后带）。
  for (int i = 0; i < plan.sections.size();) {
    if (plan.sections[i].wellIds.size() >= 2) {
      ++i;
      continue;
    }
    const QStringList lone = plan.sections[i].wellIds;
    if (i > 0)
      plan.sections[i - 1].wellIds << lone;
    else if (i + 1 < plan.sections.size())
      plan.sections[i + 1].wellIds = lone + plan.sections[i + 1].wellIds;
    plan.sections.removeAt(i);
  }
  if (plan.sections.isEmpty())
    plan.status = FencePlan::Status::TooFewWells;
  return plan;
}

QStringList orderWellsByPosition(const QStringList &ids,
                                 const QVector<Well> &wellsWithCoords) {
  QVector<const Well *> byId;
  byId.resize(wellsWithCoords.size());
  for (int i = 0; i < wellsWithCoords.size(); ++i)
    byId[i] = &wellsWithCoords[i];
  const auto locate = [&](const QString &id) -> const Well * {
    for (const Well *w : byId)
      if (w->id == id)
        return w;
    return nullptr;
  };
  QVector<QPair<double, int>> keyed; // (投影, 输入序)
  QVector<int> tail;                 // 缺坐标（保原序排末）
  for (int i = 0; i < ids.size(); ++i) {
    const Well *w = locate(ids.at(i));
    if (!w || !w->hasCoordinates()) {
      tail << i;
      continue;
    }
    keyed << qMakePair(qQNaN(), i);
  }
  double cx = 0, cy = 0;
  int n = 0;
  for (const QString &id : ids) {
    const Well *w = locate(id);
    if (!w || !w->hasCoordinates())
      continue;
    cx += w->x;
    cy += w->y;
    ++n;
  }
  if (n < 2)
    return ids; // 无从定轴 → 原序
  cx /= n;
  cy /= n;
  double sxx = 0, sxy = 0, syy = 0;
  for (const QString &id : ids) {
    const Well *w = locate(id);
    if (!w || !w->hasCoordinates())
      continue;
    const double dx = w->x - cx, dy = w->y - cy;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }
  const double theta = 0.5 * std::atan2(2 * sxy, sxx - syy);
  const double ux = std::cos(theta), uy = std::sin(theta);
  for (auto &k : keyed) {
    const Well *w = locate(ids.at(k.second));
    k.first = (w->x - cx) * ux + (w->y - cy) * uy;
  }
  std::sort(keyed.begin(), keyed.end(),
            [](const QPair<double, int> &a, const QPair<double, int> &b) {
              if (a.first != b.first)
                return a.first < b.first;
              return a.second < b.second; // 投影重合保输入序（确定性）
            });
  QStringList out;
  for (const auto &k : keyed)
    out << ids.at(k.second);
  for (int i : tail)
    out << ids.at(i);
  return out;
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

GrCutoffLithologyProvider::GrCutoffLithologyProvider(
    const Curve &gr, double cutoff, double minThicknessM,
    const QString &sourceName)
    : m_intervals(inferSandShale(gr, cutoff, minThicknessM)),
      m_sourceName(sourceName) {}

QVector<LithoSegment>
GrCutoffLithologyProvider::lithologyFor(const QString &) const {
  QVector<LithoSegment> out;
  out.reserve(m_intervals.size());
  for (const LithoInterval &iv : m_intervals) {
    LithoSegment seg;
    seg.topMd = iv.topMd;
    seg.baseMd = iv.baseMd;
    seg.litho = iv.sand ? sandWord() : shaleWord();
    seg.source = LithoSource::Inferred;
    out.push_back(seg);
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
                double f, double displayDepth, DepthDomain domain) {
  if (!a.timeDepth || !b.timeDepth)
    return qQNaN();
  // 显示深 → 域深 → MD（TVD 域经井斜反解；坏表 → NaN）。
  const double mdA = domain == DepthDomain::TVD ? a.mdOf(displayDepth + offA)
                                                : displayDepth + offA;
  const double mdB = domain == DepthDomain::TVD ? b.mdOf(displayDepth + offB)
                                                : displayDepth + offB;
  if (!std::isfinite(mdA) || !std::isfinite(mdB))
    return qQNaN();
  const double tA = a.timeDepth->twtAt(mdA);
  const double tB = b.timeDepth->twtAt(mdB);
  if (!std::isfinite(tA) || !std::isfinite(tB))
    return qQNaN();
  return (1.0 - f) * tA + f * tB;
}

} // namespace wellsection
