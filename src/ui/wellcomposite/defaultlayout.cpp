// 层：视图
// 单井默认测井查看模板（深度 → 地层 → 岩性 → 测井曲线×3 → 沉积相 → 层序）
#include "trackops.h"
#include "wellcompositetrack.h"
#include <QCoreApplication>
#include <algorithm>

namespace WellComposite::TrackOps
{
namespace
{
QString tr(const char *s) { return QCoreApplication::translate("WellComposite::WellCompositePanel", s); }
}

QList<std::shared_ptr<WellTrack>> defaultLayoutTracks(const ComprehensiveWellData &data,
                                                      const QString &scaleRatio)
{
  QList<std::shared_ptr<WellTrack>> tracks;
  QList<std::shared_ptr<WellTrack>> hidden;

  // 1. 深度道
  auto scale = std::make_shared<DepthScaleTrack>(64.0);
  if (!scaleRatio.isEmpty()) scale->setScaleRatio(scaleRatio);
  tracks << scale;

  // 2. 地层道：分层优先；只有系/统/组数据时用地层系统组合道顶上
  auto strat = std::make_shared<StratigraphyCompoundTrack>(tr("地层系统"), 145.0);
  bool stratHasData = false;
  if (!data.stratigraphyIntervals.isEmpty())
  {
    strat->setIntervals(data.stratigraphyIntervals);
    stratHasData = true;
  }
  else if (!data.formationIntervals.isEmpty())
  {
    strat->autoDeriveStratigraphy(data.formationIntervals, data.minDepth, data.maxDepth);
    const auto derived = strat->intervals();
    stratHasData = std::any_of(derived.cbegin(), derived.cend(),
                               [](const StratigraphyInterval &si) { return !si.system.isEmpty(); });
  }
  if (!data.formationIntervals.isEmpty())
  {
    auto form = std::make_shared<FormationTrack>(tr("地层"), 75.0);
    form->setSource(FormationTrack::FormationSource::Layers);
    form->setIntervals(data.formationIntervals);
    tracks << form;
    if (stratHasData) { strat->setVisible(false); hidden << strat; }
  }
  else if (stratHasData)
  {
    strat->setTitle(tr("地层"));
    tracks << strat;
  }

  // 3. 岩性道
  if (!data.lithologyIntervals.isEmpty())
  {
    auto litho = std::make_shared<LithologyTrack>(tr("岩性"), 90.0);
    litho->setIntervals(data.lithologyIntervals);
    tracks << litho;
  }

  // 4. 测井曲线 ×3（+ 隐藏辅助道，追加到末尾）
  QList<std::shared_ptr<WellTrack>> auxCurves;
  for (const auto &t : standardCurveTracks(combinedCurvePool(data.continuousCurves, data.discreteCurves)))
    (t->isVisible() ? tracks : auxCurves) << t;

  // 5. 沉积相（相 | 亚相 | 微相）
  if (!data.faciesIntervals.isEmpty())
  {
    auto facies = std::make_shared<FaciesCompoundTrack>(tr("沉积相"), 180.0);
    facies->setIntervals(data.faciesIntervals);
    tracks << facies;
  }

  // 6. 层序（有数据才出）
  if (!data.sequenceIntervals.isEmpty())
  {
    auto seq = std::make_shared<FormationTrack>(tr("层序"), 70.0);
    seq->setSource(FormationTrack::FormationSource::Sequence);
    seq->setIntervals(data.sequenceIntervals);
    tracks << seq;
  }

  // 岩心照片属工程附件（目录挂接），有则保持显示，排在模板末尾
  if (!data.images.isEmpty())
  {
    auto t = std::make_shared<ImageTrack>(tr("岩心照片"), 110.0);
    t->setItems(data.images);
    tracks << t;
  }

  // 非默认但有数据的道：隐藏保留
  if (!data.sandIntervals.isEmpty())
  {
    auto t = std::make_shared<FormationTrack>(tr("砂层组"), 60.0);
    t->setSource(FormationTrack::FormationSource::SandGroup);
    t->setIntervals(data.sandIntervals); t->setVisible(false); hidden << t;
  }
  if (!data.symbolItems.isEmpty())
  {
    auto t = std::make_shared<SymbolTrack>(tr("沉积旋回"), 50.0);
    t->setItems(data.symbolItems); t->setVisible(false); hidden << t;
  }
  if (!data.coreBarrels.isEmpty())
  {
    auto t = std::make_shared<CoreTrack>(tr("取心数据"), 65.0);
    t->setBarrels(data.coreBarrels); t->setVisible(false); hidden << t;
  }
  if (!data.textIntervals.isEmpty())
  {
    auto t = std::make_shared<TextTrack>(tr("解释结论/取样"), 120.0);
    t->setIntervals(data.textIntervals); t->setVisible(false); hidden << t;
  }
  return tracks + hidden + auxCurves;
}

QStringList layoutSignature(const QList<TrackSpec> &specs)
{
  QStringList out;
  for (const TrackSpec &s : specs)
  {
    QStringList extra;
    for (auto it = s.params.cbegin(); it != s.params.cend(); ++it)
    {
      if (it.key() == QStringLiteral("curveOverrides") || it.key() == QStringLiteral("scaleRatio")) continue;
      extra << it.key() + QLatin1Char('=') + it.value().toStringList().join(QLatin1Char(',')) +
                   it.value().toString();
    }
    out << QStringLiteral("%1|%2|%3|%4|%5")
               .arg(s.typeId, s.title, s.visible ? QStringLiteral("1") : QStringLiteral("0"))
               .arg(qRound(s.width))
               .arg(extra.join(QLatin1Char(';')));
  }
  return out;
}
} // namespace WellComposite::TrackOps
