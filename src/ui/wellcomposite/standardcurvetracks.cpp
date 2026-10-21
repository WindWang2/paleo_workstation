// 层：视图
#include "trackops.h"
#include <QRegularExpression>
#include <QSet>

namespace WellComposite::TrackOps
{
namespace
{
// 默认三曲线的物理量优先级：GR 必选（若有），其后自然电位、深电阻率、密度、声波、中子……
const QStringList &roleAliases()
{
  static const QStringList aliases = {
      QStringLiteral("GR|SGR|CGR|HSGR|HCGR|GRC|NGR|GAMMA"),   // 0 自然伽马
      QStringLiteral("SP"),                                  // 1 自然电位
      QStringLiteral("RT|RD|RDEP|RLLD|LLD|ILD|AT90|RACELM|RACEL"), // 2 深电阻率
      QStringLiteral("DEN|RHOB|ZDEN"),                       // 3 密度
      QStringLiteral("AC|DT|DTC"),                           // 4 声波
      QStringLiteral("CNL|NPHI|CNCF|TNPH"),                  // 5 中子
      QStringLiteral("RS|RMED|RLLS|LLS|ILM|AT30|RPCELM|RPCEL"), // 6 浅电阻率
      QStringLiteral("RXO|RXOZ|RMSFL|MSFL|RFOC|AT10"),       // 7 冲洗带电阻率
      QStringLiteral("CALI|CAL")};                           // 8 井径
  return aliases;
}

int curveRole(const CurveData &curve)
{
  if (curve.mode != CurveDisplayMode::Continuous || curve.isEmpty())
    return -1;
  const QString name = curve.name.trimmed().toUpper();
  const QStringList &aliases = roleAliases();
  for (int i = 0; i < aliases.size(); ++i)
  {
    const QRegularExpression pattern(QStringLiteral("^(?:%1)(?:[._-].*|[0-9]+)?$").arg(aliases.at(i)));
    if (pattern.match(name).hasMatch())
      return i;
  }
  return -1;
}
} // namespace

QString defaultCurveTrackTitle()
{
  return QCoreApplication::translate("WellComposite::WellCompositePanel", "测井曲线");
}

QVector<int> defaultCurveSelection(const QVector<CurveData> &curves, int count)
{
  // 每个物理量取首根匹配曲线（重复处理版本 GR_2 等留在辅助池）。
  QVector<int> byRole(roleAliases().size(), -1);
  for (int i = 0; i < curves.size(); ++i)
  {
    const int role = curveRole(curves.at(i));
    if (role >= 0 && byRole.at(role) < 0)
      byRole[role] = i;
  }
  QVector<int> picked;
  for (int idx : byRole)
    if (idx >= 0 && picked.size() < count) picked << idx;
  // 不足三根：先补未识别的连续曲线，再补离散曲线（按文件顺序）。
  for (int pass = 0; pass < 2 && picked.size() < count; ++pass)
    for (int i = 0; i < curves.size() && picked.size() < count; ++i)
    {
      const CurveData &c = curves.at(i);
      if (c.isEmpty() || picked.contains(i)) continue;
      if (pass == 0 && c.mode != CurveDisplayMode::Continuous) continue;
      picked << i;
    }
  return picked;
}

QList<std::shared_ptr<WellTrack>> standardCurveTracks(const QVector<CurveData> &curves)
{
  QList<std::shared_ptr<WellTrack>> tracks;
  QSet<int> used;
  // 默认视图：三个单曲线道，道名统一「测井曲线」；曲线助记名仍在道头刻度/图例中显示。
  for (int idx : defaultCurveSelection(curves, 3))
  {
    auto track = std::make_shared<CurveTrack>(defaultCurveTrackTitle(), 150.0);
    track->addCurve(curves.at(idx));
    used.insert(idx);
    tracks << track;
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
