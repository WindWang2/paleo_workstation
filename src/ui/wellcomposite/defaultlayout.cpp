// 层：视图
// 单井默认测井查看模板（深度 → 地层 → 岩性 → 测井曲线×3 → 沉积相 → 层序）
#include "trackops.h"
#include "wellcompositetrack.h"
#include <QCoreApplication>
#include <QHash>
#include <algorithm>

namespace WellComposite::TrackOps
{
namespace
{
QString tr(const char *s) { return QCoreApplication::translate("WellComposite::WellCompositePanel", s); }
int cycleRank(const QString &title)
{
  if (title.contains(QStringLiteral("三级"))) return 0;
  if (title.contains(QStringLiteral("四级"))) return 1;
  return 2;
}
QString cycleTrackTitle(const QString &name)
{
  const QString title = name.trimmed();
  if (title.contains(QStringLiteral("三级")) && !title.contains(QStringLiteral("层序")))
    return QStringLiteral("三级层序");
  if (title.contains(QStringLiteral("四级")) && !title.contains(QStringLiteral("层序")))
    return QStringLiteral("四级层序");
  return title;
}
}

QList<std::shared_ptr<WellTrack>> defaultLayoutTracks(const ComprehensiveWellData &data,
                                                      const QString &scaleRatio)
{
  QList<std::shared_ptr<WellTrack>> tracks;
  QList<std::shared_ptr<WellTrack>> hidden;
  QVector<SymbolItem> unnamedSymbols;

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
    QHash<QString, QVector<FormationInterval>> byName;
    QStringList order;
    for (const auto &interval : data.formationIntervals) {
      const QString name = interval.unitType.isEmpty() ? tr("地层") : interval.unitType;
      if (!byName.contains(name)) order << name;
      byName[name].append(interval);
    }
    for (const QString &name : order) {
      auto form = std::make_shared<FormationTrack>(name, 75.0);
      form->setSource(FormationTrack::FormationSource::Layers);
      form->setIntervals(byName.value(name));
      tracks << form;
    }
    if (stratHasData) { strat->setVisible(false); hidden << strat; }
  }
  else if (stratHasData)
  {
    strat->setTitle(tr("地层"));
    tracks << strat;
  }

  // 旋回按道名拆开。道名是三级层序、四级层序，道内画正反三角。
  {
    QHash<QString, QVector<SymbolItem>> named;
    QStringList order;
    for (const auto &item : data.symbolItems) {
      const QString title = cycleTrackTitle(item.track);
      if (title.isEmpty()) { unnamedSymbols.append(item); continue; }
      if (!named.contains(title)) order << title;
      named[title].append(item);
    }
    std::stable_sort(order.begin(), order.end(), [](const QString &a, const QString &b) {
      return cycleRank(a) < cycleRank(b);
    });
    for (const QString &title : order) {
      auto cycle = std::make_shared<SymbolTrack>(title, 96.0);
      cycle->setItems(named.value(title));
      tracks << cycle;
    }
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
  if (!unnamedSymbols.isEmpty())
  {
    auto t = std::make_shared<SymbolTrack>(tr("沉积旋回"), 50.0);
    t->setItems(unnamedSymbols); t->setVisible(false); hidden << t;
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
