// 层：视图
#include "trackops.h"
#include <QRegularExpression>
#include <QSet>

namespace WellComposite::TrackOps
{
namespace
{
int curveRole(const CurveData &curve)
{
  if (curve.mode != CurveDisplayMode::Continuous || curve.isEmpty())
    return -1;
  // 一个物理量只选一根；重复处理版本、气测、井斜与解释曲线留在辅助道。
  static const QStringList aliases = {
      QStringLiteral("GR|SGR|CGR"), QStringLiteral("SP"), QStringLiteral("CALI|CAL"),
      QStringLiteral("AC|DT|DTC"), QStringLiteral("DEN|RHOB|ZDEN"),
      QStringLiteral("CNL|NPHI|CNCF|TNPH"),
      QStringLiteral("RT|RD|RDEP|RLLD|LLD|ILD|AT90|RACELM|RACEL"),
      QStringLiteral("RS|RMED|RLLS|LLS|ILM|AT30|RPCELM|RPCEL"),
      QStringLiteral("RXO|RXOZ|RMSFL|MSFL|RFOC|AT10")};
  const QString name = curve.name.trimmed().toUpper();
  for (int i = 0; i < aliases.size(); ++i)
  {
    const QRegularExpression pattern(QStringLiteral("^(?:%1)(?:[._-].*|[0-9]+)?$").arg(aliases.at(i)));
    if (pattern.match(name).hasMatch())
      return i;
  }
  return -1;
}
}

QList<std::shared_ptr<WellTrack>> standardCurveTracks(const QVector<CurveData> &curves)
{
  QList<std::shared_ptr<WellTrack>> tracks;
  QVector<int> selected(9, -1);
  QSet<int> used;
  for (int i = 0; i < curves.size(); ++i)
  {
    const int role = curveRole(curves.at(i));
    if (role >= 0 && selected.at(role) < 0)
      selected[role] = i;
  }
  const QStringList titles = {QCoreApplication::translate("WellComposite::WellCompositePanel", "岩性测井"),
                             QCoreApplication::translate("WellComposite::WellCompositePanel", "三孔隙测井"),
                             QCoreApplication::translate("WellComposite::WellCompositePanel", "电阻率测井")};
  for (int group = 0; group < 3; ++group)
  {
    auto track = std::make_shared<CurveTrack>(titles.at(group), 180.0);
    for (int role = group * 3; role < group * 3 + 3; ++role)
    {
      const int idx = selected.at(role);
      if (idx < 0) continue;
      track->addCurve(curves.at(idx));
      used.insert(idx);
    }
    if (track->curveCount() > 0) tracks << track;
  }
  // 特殊助记名井也有可读首屏；没有标准曲线时仅显示首个有效曲线道。
  if (tracks.isEmpty())
  {
    auto fallback = std::make_shared<CurveTrack>(QCoreApplication::translate("WellComposite::WellCompositePanel", "常规测井"), 180.0);
    for (int i = 0; i < curves.size() && fallback->curveCount() < 4; ++i)
      if (!curves.at(i).isEmpty()) { fallback->addCurve(curves.at(i)); used.insert(i); }
    if (fallback->curveCount() > 0) tracks << fallback;
  }
  std::shared_ptr<CurveTrack> auxiliary;
  int auxiliaryCount = 0;
  for (int i = 0; i < curves.size(); ++i)
  {
    if (used.contains(i)) continue;
    if (!auxiliary || auxiliary->curveCount() == 4)
    {
      auxiliary = std::make_shared<CurveTrack>(
          QCoreApplication::translate("WellComposite::WellCompositePanel", "辅助曲线 %1").arg(++auxiliaryCount), 180.0);
      auxiliary->setVisible(false);
      tracks << auxiliary;
    }
    auxiliary->addCurve(curves.at(i));
  }
  return tracks;
}
} // namespace WellComposite::TrackOps
