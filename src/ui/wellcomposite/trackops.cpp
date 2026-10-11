// 层：视图
#include "trackops.h"

#include <QTextStream>
#include <algorithm>

#include "domain/wellcompositemodel.h"
#include "wellcompositetrack.h"

namespace WellComposite
{
namespace TrackOps
{

QString exportTrackCsv(const WellTrack &track)
{
  QString out;
  QTextStream ts(&out);
  ts.setGenerateByteOrderMark(false); // BOM 由调用方写文件时附加；字符串体保持纯 CSV

  const auto f1 = [](float v) { return QString::number(v, 'f', 1); };

  switch (track.type())
  {
  case TrackType::Curve:
  {
    const auto *ct = static_cast<const CurveTrack *>(&track);
    QStringList head;
    head << QStringLiteral("depth");
    for (const auto &c : ct->curves())
      head << (c.unit.isEmpty() ? c.name : QStringLiteral("%1(%2)").arg(c.name, c.unit));
    ts << head.join(QLatin1Char(',')) << QLatin1Char('\n');

    // 深度轴取各曲线的并集（多 LAS 合并的曲线各有自己的深度列）：各自
    // 采样点如实落格，不插值、不丢样；旧实现全部对齐第一根曲线的深度
    // 轴，采样率/范围不同即系统性错位。同轴曲线（常态）输出与旧行一致。
    QVector<float> depthUnion;
    for (const auto &c : ct->curves())
      depthUnion += c.depths;
    std::sort(depthUnion.begin(), depthUnion.end());
    depthUnion.erase(std::unique(depthUnion.begin(), depthUnion.end()),
                     depthUnion.end());
    for (const float d : depthUnion)
    {
      QStringList row;
      row << f1(d);
      for (const auto &c : ct->curves())
      {
        // CurveData.depths 按升序契约（valueAtDepth 的二分依赖同一前提）
        const auto it = std::lower_bound(c.depths.cbegin(), c.depths.cend(), d);
        QString val;
        if (it != c.depths.cend() && *it == d)
        {
          const qsizetype idx = it - c.depths.cbegin();
          if (idx < c.values.size() && std::isfinite(c.values.at(idx)))
            val = QString::number(c.values.at(idx), 'g', 6);
        }
        row << val;
      }
      ts << row.join(QLatin1Char(',')) << QLatin1Char('\n');
    }
    break;
  }
  case TrackType::Formation:
  {
    const auto *ft = static_cast<const FormationTrack *>(&track);
    ts << QStringLiteral("top,bottom,name\n");
    for (const auto &fi : ft->intervals())
      ts << f1(fi.topDepth) << QLatin1Char(',') << f1(fi.bottomDepth) << QLatin1Char(',')
         << fi.name << QLatin1Char('\n');
    break;
  }
  case TrackType::Lithology:
  {
    const auto *lt = static_cast<const LithologyTrack *>(&track);
    ts << QStringLiteral("top,bottom,lithology\n");
    for (const auto &li : lt->intervals())
      ts << f1(li.topDepth) << QLatin1Char(',') << f1(li.bottomDepth) << QLatin1Char(',')
         << li.lithoName << QLatin1Char('\n');
    break;
  }
  case TrackType::Text:
  {
    const auto *tt = static_cast<const TextTrack *>(&track);
    ts << QStringLiteral("top,bottom,category,text\n");
    for (const auto &ti : tt->intervals())
      ts << f1(ti.topDepth) << QLatin1Char(',') << f1(ti.bottomDepth) << QLatin1Char(',')
         << ti.category << QLatin1Char(',') << ti.text << QLatin1Char('\n');
    break;
  }
  case TrackType::Core:
  {
    const auto *kt = static_cast<const CoreTrack *>(&track);
    ts << QStringLiteral("top,bottom,barrel,cut,recovered,rate\n");
    for (const auto &b : kt->barrels())
      ts << f1(b.topDepth) << QLatin1Char(',') << f1(b.bottomDepth) << QLatin1Char(',')
         << b.barrelNo << QLatin1Char(',') << QString::number(b.cutLength, 'f', 2)
         << QLatin1Char(',') << QString::number(b.recoveredLength, 'f', 2)
         << QLatin1Char(',') << QString::number(b.recoveryRate, 'f', 1) << QLatin1Char('\n');
    break;
  }
  case TrackType::Symbol:
  {
    const auto *st = static_cast<const SymbolTrack *>(&track);
    ts << QStringLiteral("top,bottom,label\n");
    for (const auto &si : st->items())
      ts << f1(si.topDepth) << QLatin1Char(',') << f1(si.bottomDepth) << QLatin1Char(',')
         << si.label << QLatin1Char('\n');
    break;
  }
  case TrackType::StratigraphyCompound:
  {
    const auto *sc = static_cast<const StratigraphyCompoundTrack *>(&track);
    ts << QStringLiteral("top,bottom,system,series,formation\n");
    for (const auto &si : sc->intervals())
      ts << f1(si.topDepth) << QLatin1Char(',') << f1(si.bottomDepth) << QLatin1Char(',')
         << si.system << QLatin1Char(',') << si.series << QLatin1Char(',') << si.formation
         << QLatin1Char('\n');
    break;
  }
  case TrackType::FaciesCompound:
  {
    const auto *fc = static_cast<const FaciesCompoundTrack *>(&track);
    ts << QStringLiteral("top,bottom,major,sub,micro\n");
    for (const auto &fi : fc->intervals())
      ts << f1(fi.topDepth) << QLatin1Char(',') << f1(fi.bottomDepth) << QLatin1Char(',')
         << fi.majorFacies << QLatin1Char(',') << fi.subFacies << QLatin1Char(',')
         << fi.microFacies << QLatin1Char('\n');
    break;
  }
  default:
    ts << QStringLiteral("top,bottom,value\n");
    break;
  }
  return out;
}

std::shared_ptr<WellTrack> duplicateTrack(const std::shared_ptr<WellTrack> &track)
{
  if (!track)
    return nullptr;

  switch (track->type())
  {
  case TrackType::DepthScale:
  {
    auto src = std::static_pointer_cast<DepthScaleTrack>(track);
    auto t = std::make_shared<DepthScaleTrack>(src->width());
    t->setScaleRatio(src->scaleRatio());
    t->setTitle(src->title());
    return t;
  }
  case TrackType::Text:
  {
    auto src = std::static_pointer_cast<TextTrack>(track);
    auto t = std::make_shared<TextTrack>(src->title(), src->width());
    t->setIntervals(src->intervals());
    return t;
  }
  case TrackType::Formation:
  {
    auto src = std::static_pointer_cast<FormationTrack>(track);
    auto t = std::make_shared<FormationTrack>(src->title(), src->width());
    t->setIntervals(src->intervals());
    return t;
  }
  case TrackType::Lithology:
  {
    auto src = std::static_pointer_cast<LithologyTrack>(track);
    auto t = std::make_shared<LithologyTrack>(src->title(), src->width());
    t->setIntervals(src->intervals());
    return t;
  }
  case TrackType::Core:
  {
    auto src = std::static_pointer_cast<CoreTrack>(track);
    auto t = std::make_shared<CoreTrack>(src->title(), src->width());
    t->setBarrels(src->barrels());
    return t;
  }
  case TrackType::Image:
  {
    auto src = std::static_pointer_cast<ImageTrack>(track);
    auto t = std::make_shared<ImageTrack>(src->title(), src->width());
    t->setItems(src->items());
    return t;
  }
  case TrackType::Curve:
  {
    auto src = std::static_pointer_cast<CurveTrack>(track);
    auto t = std::make_shared<CurveTrack>(src->title(), src->width());
    t->setCurves(src->curves());
    t->setShowGrid(src->showGrid());
    t->setGridDensity(src->gridDensity());
    return t;
  }
  case TrackType::Symbol:
  {
    auto src = std::static_pointer_cast<SymbolTrack>(track);
    auto t = std::make_shared<SymbolTrack>(src->title(), src->width());
    t->setItems(src->items());
    return t;
  }
  case TrackType::StratigraphyCompound:
  {
    auto src = std::static_pointer_cast<StratigraphyCompoundTrack>(track);
    auto t = std::make_shared<StratigraphyCompoundTrack>(src->title(), src->width());
    t->setIntervals(src->intervals());
    return t;
  }
  case TrackType::FaciesCompound:
  {
    auto src = std::static_pointer_cast<FaciesCompoundTrack>(track);
    auto t = std::make_shared<FaciesCompoundTrack>(src->title(), src->width());
    t->setIntervals(src->intervals());
    return t;
  }
  }
  return nullptr;
}

QVector<CurveData> combinedCurvePool(const QVector<CurveData> &continuous,
                                     const QVector<CurveData> &discrete)
{
  QVector<CurveData> pool = continuous;
  pool += discrete;
  return pool;
}

int injectCurvesFromData(CurveTrack &track, const TrackSpec &spec,
                         const QVector<CurveData> &continuousPool,
                         const QVector<CurveData> &discretePool)
{
  const bool discreteMode = spec.typeId == QStringLiteral("discrete");
  const QVector<CurveData> &pool = discreteMode ? discretePool : continuousPool;
  const QVariantMap overrides = spec.curveOverrides();
  int injected = 0;
  track.clearCurves();
  for (const QString &name : spec.curveNames())
  {
    if (track.curveCount() >= 4)
      break;
    bool found = false;
    for (const auto &c : pool)
    {
      if (c.name.compare(name, Qt::CaseInsensitive) == 0)
      {
        CurveData cd = c;
        const QVariantMap ov = overrides.value(name).toMap();
        if (!ov.isEmpty())
          cd = applyCurveOverride(cd, ov);
        if (discreteMode)
          cd.mode = CurveDisplayMode::Discrete;
        if (track.addCurve(cd))
          ++injected;
        found = true;
        break;
      }
    }
    // 找不到的名字静默跳过（曲线池可能尚未加载；面板会在数据到达后重注入）
    Q_UNUSED(found);
  }
  return injected;
}

int injectCurvesFromWellData(CurveTrack &track, const TrackSpec &spec,
                             const ComprehensiveWellData &data)
{
  return injectCurvesFromData(track, spec, data.continuousCurves, data.discreteCurves);
}

} // namespace TrackOps
} // namespace WellComposite
