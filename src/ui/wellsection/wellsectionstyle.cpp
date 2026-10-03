// 层：视图
#include "wellsectionstyle.h"

#include "domain/mappinghorizons.h"

#include <QJsonArray>
#include <QObject>

namespace wellsection {

bool CurveStyle::operator==(const CurveStyle &o) const
{
  return mnemonic == o.mnemonic && label == o.label && min == o.min &&
         max == o.max && logScale == o.logScale && color == o.color;
}

QString TrackSpec::displayTitle() const
{
  if (!title.isEmpty())
    return title;
  switch (kind)
  {
    case TrackKind::Zone:
      return QObject::tr("小层");
    case TrackKind::Depth:
      return QObject::tr("深度/m");
    case TrackKind::Lithology:
      return QObject::tr("岩性");
    case TrackKind::Facies:
      return QObject::tr("相代码");
    case TrackKind::Curve:
    {
      QStringList labels;
      for (const CurveStyle &c : curves)
        labels << (c.label.isEmpty() ? c.mnemonic : c.label);
      return labels.join(QStringLiteral("/"));
    }
  }
  return QString();
}

bool TrackSpec::operator==(const TrackSpec &o) const
{
  return kind == o.kind && title == o.title && width == o.width &&
         curves == o.curves && sandFill == o.sandFill && cutoff == o.cutoff &&
         sourceMnemonic == o.sourceMnemonic;
}

SectionTemplate SectionTemplate::defaults()
{
  SectionTemplate t;

  TrackSpec zone;
  zone.kind = TrackKind::Zone;
  zone.width = 44;
  t.tracks << zone;

  TrackSpec gr;
  gr.kind = TrackKind::Curve;
  gr.width = 56;
  gr.sandFill = true;
  gr.cutoff = 75.0;
  CurveStyle grc;
  grc.mnemonic = QStringLiteral("GR");
  grc.label = QStringLiteral("GR/API");
  grc.min = 20.0;
  grc.max = 150.0;
  grc.logScale = false;
  grc.color = QColor(QStringLiteral("#333333"));
  gr.curves << grc;
  t.tracks << gr;

  TrackSpec depth;
  depth.kind = TrackKind::Depth;
  depth.width = 34;
  t.tracks << depth;

  TrackSpec litho;
  litho.kind = TrackKind::Lithology;
  litho.width = 40;
  litho.cutoff = 75.0;
  litho.sourceMnemonic = QStringLiteral("GR");
  t.tracks << litho;

  TrackSpec res;
  res.kind = TrackKind::Curve;
  res.width = 64;
  CurveStyle rd;
  rd.mnemonic = QStringLiteral("RD");
  rd.label = QStringLiteral("RD/(Ω·m)");
  rd.min = 0.2;
  rd.max = 20.0;
  rd.logScale = true;
  rd.color = QColor(QStringLiteral("#D7263D"));
  CurveStyle rs;
  rs.mnemonic = QStringLiteral("RS");
  rs.label = QStringLiteral("RS/(Ω·m)");
  rs.min = 0.2;
  rs.max = 20.0;
  rs.logScale = true;
  rs.color = QColor(QStringLiteral("#2B59C3"));
  res.curves << rd << rs;
  t.tracks << res;

  return t;
}

QStringList SectionTemplate::mnemonics() const
{
  QStringList out;
  for (const TrackSpec &tr : tracks)
  {
    if (tr.kind == TrackKind::Curve)
      for (const CurveStyle &c : tr.curves)
        if (!c.mnemonic.isEmpty() && !out.contains(c.mnemonic, Qt::CaseInsensitive))
          out << c.mnemonic;
    if (tr.kind == TrackKind::Lithology &&
        !tr.sourceMnemonic.isEmpty() &&
        !out.contains(tr.sourceMnemonic, Qt::CaseInsensitive))
      out << tr.sourceMnemonic;
  }
  return out;
}

int SectionTemplate::columnWidth() const
{
  int w = 0;
  for (const TrackSpec &tr : tracks)
    w += qBound(24, tr.width, 200);
  return w;
}

namespace {
const char *kindKey(TrackKind k)
{
  switch (k)
  {
    case TrackKind::Zone: return "zone";
    case TrackKind::Curve: return "curve";
    case TrackKind::Depth: return "depth";
    case TrackKind::Lithology: return "lithology";
    case TrackKind::Facies: return "facies";
  }
  return "curve";
}

bool kindFromKey(const QString &s, TrackKind *k)
{
  if (s == QLatin1String("zone")) { *k = TrackKind::Zone; return true; }
  if (s == QLatin1String("curve")) { *k = TrackKind::Curve; return true; }
  if (s == QLatin1String("depth")) { *k = TrackKind::Depth; return true; }
  if (s == QLatin1String("lithology")) { *k = TrackKind::Lithology; return true; }
  if (s == QLatin1String("facies")) { *k = TrackKind::Facies; return true; }
  return false;
}

const char *filterKey(TopFilter f)
{
  switch (f)
  {
    case TopFilter::Mapping: return "mapping";
    case TopFilter::All: return "all";
    case TopFilter::Custom: return "custom";
  }
  return "mapping";
}
} // namespace

QJsonObject SectionTemplate::toJson() const
{
  QJsonObject o;
  o.insert(QStringLiteral("version"), 1);
  o.insert(QStringLiteral("topFilter"), QLatin1String(filterKey(topFilter)));
  QJsonArray tops;
  for (const QString &n : customTops)
    tops.append(n);
  o.insert(QStringLiteral("customTops"), tops);
  QJsonArray tracks;
  for (const TrackSpec &tr : this->tracks)
  {
    QJsonObject tj;
    tj.insert(QStringLiteral("kind"), QLatin1String(kindKey(tr.kind)));
    tj.insert(QStringLiteral("title"), tr.title);
    tj.insert(QStringLiteral("width"), tr.width);
    tj.insert(QStringLiteral("sandFill"), tr.sandFill);
    tj.insert(QStringLiteral("cutoff"), tr.cutoff);
    tj.insert(QStringLiteral("source"), tr.sourceMnemonic);
    QJsonArray curves;
    for (const CurveStyle &c : tr.curves)
    {
      QJsonObject cj;
      cj.insert(QStringLiteral("mnemonic"), c.mnemonic);
      cj.insert(QStringLiteral("label"), c.label);
      cj.insert(QStringLiteral("min"), c.min);
      cj.insert(QStringLiteral("max"), c.max);
      cj.insert(QStringLiteral("log"), c.logScale);
      cj.insert(QStringLiteral("color"), c.color.name(QColor::HexArgb));
      curves.append(cj);
    }
    tj.insert(QStringLiteral("curves"), curves);
    tracks.append(tj);
  }
  o.insert(QStringLiteral("tracks"), tracks);
  return o;
}

SectionTemplate SectionTemplate::fromJson(const QJsonObject &o, bool *ok)
{
  bool good = true;
  SectionTemplate t;
  const QJsonArray tracks = o.value(QLatin1String("tracks")).toArray();
  if (tracks.isEmpty())
    good = false;
  for (const auto &v : tracks)
  {
    const QJsonObject tj = v.toObject();
    TrackSpec tr;
    if (!kindFromKey(tj.value(QLatin1String("kind")).toString(), &tr.kind))
    {
      good = false;
      break;
    }
    tr.title = tj.value(QLatin1String("title")).toString();
    tr.width = tj.value(QLatin1String("width")).toInt(56);
    tr.sandFill = tj.value(QLatin1String("sandFill")).toBool(false);
    tr.cutoff = tj.value(QLatin1String("cutoff")).toDouble(75.0);
    tr.sourceMnemonic = tj.value(QLatin1String("source")).toString();
    const QJsonArray curves = tj.value(QLatin1String("curves")).toArray();
    for (const auto &cv : curves)
    {
      const QJsonObject cj = cv.toObject();
      CurveStyle c;
      c.mnemonic = cj.value(QLatin1String("mnemonic")).toString();
      c.label = cj.value(QLatin1String("label")).toString();
      c.min = cj.value(QLatin1String("min")).toDouble(0.0);
      c.max = cj.value(QLatin1String("max")).toDouble(150.0);
      c.logScale = cj.value(QLatin1String("log")).toBool(false);
      c.color = QColor(cj.value(QLatin1String("color")).toString());
      if (c.mnemonic.isEmpty())
      {
        good = false;
        break;
      }
      tr.curves << c;
    }
    if (!good)
      break;
    if (tr.kind == TrackKind::Curve && tr.curves.isEmpty())
    {
      good = false;
      break;
    }
    t.tracks << tr;
  }
  if (good)
  {
    const QString f = o.value(QLatin1String("topFilter")).toString();
    if (f == QLatin1String("all"))
      t.topFilter = TopFilter::All;
    else if (f == QLatin1String("custom"))
      t.topFilter = TopFilter::Custom;
    else
      t.topFilter = TopFilter::Mapping;
    for (const auto &v : o.value(QLatin1String("customTops")).toArray())
      t.customTops << v.toString();
  }
  if (ok)
    *ok = good;
  return good ? t : defaults();
}

bool SectionTemplate::operator==(const SectionTemplate &o) const
{
  return tracks == o.tracks && topFilter == o.topFilter &&
         customTops == o.customTops;
}

QVector<Well> filterTops(const QVector<Well> &wells, const SectionTemplate &t)
{
  QVector<Well> out = wells;
  switch (t.topFilter)
  {
    case TopFilter::All:
      return out;
    case TopFilter::Custom:
      if (t.customTops.isEmpty())
        return out;
      for (Well &w : out)
      {
        QVector<Top> kept;
        for (const Top &top : w.tops)
          if (t.customTops.contains(top.name))
            kept << top;
        w.tops = kept;
      }
      return out;
    case TopFilter::Mapping:
    {
      const QStringList horizons = mappingHorizons();
      bool anyKept = false;
      for (Well &w : out)
      {
        QVector<Top> kept;
        for (const Top &top : w.tops)
          if (horizons.contains(top.name))
            kept << top;
        if (!kept.isEmpty())
          anyKept = true;
        w.tops = kept;
      }
      if (!anyKept)
        return wells; // 滤完剖面上一顶不留 → 按全量画，不出空图
      return out;
    }
  }
  return out;
}

QVector<SectionTheme> SectionTheme::presets()
{
  SectionTheme classic;
  classic.id = QStringLiteral("classic");
  classic.name = QObject::tr("经典");
  classic.paper = QColor(QStringLiteral("#FFFFFF"));
  classic.frame = QColor(QStringLiteral("#3A3A3A"));
  classic.text = QColor(QStringLiteral("#222222"));
  classic.link = QColor(QStringLiteral("#8C8C8C"));
  classic.linkWidth = 1.2;
  classic.curvedLinks = true;
  classic.zoneFill = false;
  classic.sand = QColor(QStringLiteral("#EDE84A"));
  classic.sandDots = QColor(QStringLiteral("#8A8400"));
  classic.lithoSand = QColor(QStringLiteral("#B5B5B5"));
  classic.lithoShale = QColor(QStringLiteral("#8C8C8C"));
  classic.seismicGray = true;
  classic.seismicOpacity = 0.85;
  classic.fault = QColor(QStringLiteral("#B33A3A"));

  SectionTheme colored = classic;
  colored.id = QStringLiteral("colored");
  colored.name = QObject::tr("彩色分层");
  colored.link = QColor(QStringLiteral("#5E5E5E"));
  colored.zoneFill = true;
  colored.seismicGray = false;
  colored.seismicOpacity = 0.8;

  SectionTheme print = classic;
  print.id = QStringLiteral("print");
  print.name = QObject::tr("简洁");
  print.frame = QColor(QStringLiteral("#000000"));
  print.link = QColor(QStringLiteral("#000000"));
  print.linkWidth = 0.8;
  print.curvedLinks = false;
  print.zoneFill = false;
  print.sand = QColor(0, 0, 0, 0); // 不充填，只描轮廓
  print.sandDots = QColor(0, 0, 0, 0);
  print.lithoSand = QColor(QStringLiteral("#FFFFFF"));
  print.lithoShale = QColor(QStringLiteral("#FFFFFF"));
  print.seismicGray = true;
  print.seismicOpacity = 0.7;
  print.fault = QColor(QStringLiteral("#000000"));

  return {classic, colored, print};
}

SectionTheme SectionTheme::byId(const QString &id)
{
  const QVector<SectionTheme> all = presets();
  for (const SectionTheme &t : all)
    if (t.id == id)
      return t;
  return all.first();
}

QColor zoneColor(int index)
{
  // 柔和 pastel 循环色板（数据符号色，不进 UI token）。
  static const QColor kPalette[] = {
      QColor(QStringLiteral("#F5D6C6")), QColor(QStringLiteral("#F3E5AB")),
      QColor(QStringLiteral("#D6E8CE")), QColor(QStringLiteral("#CFE3F2")),
      QColor(QStringLiteral("#E0D5EC")), QColor(QStringLiteral("#F2D1DE")),
      QColor(QStringLiteral("#D8E4BC")), QColor(QStringLiteral("#FCE8CD"))};
  constexpr int n = int(sizeof(kPalette) / sizeof(kPalette[0]));
  const int i = ((index % n) + n) % n;
  return kPalette[i];
}

} // namespace wellsection
