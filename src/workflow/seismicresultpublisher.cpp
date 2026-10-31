// 层：功能
#include "seismicresultpublisher.h"
#include "services/subintervalfactors.h"
#include "paleo_data/service.h"
#include <QObject>

SeismicPublicationFeedback publishSeismicAggregate(const QString &directory,
    const paleo::strat::UnitSelection &unit, const QVariantMap &params)
{
  if (!unit.isValid() || unit.aggregate)
    return {false, QObject::tr("请先在画布层段分析条选择具体子层段（S1–S4），再发布层段综合")};
  paleo::data::DataService ds;
  QString error;
  if (!paleo::strat::openProjectDataService(directory, ds, &error)) return {false, error};
  QVector<paleo::strat::SeismicSliceVote> votes;
  for (const auto &value : params.value("seismicSliceVotes").toList()) {
    const auto row = value.toMap();
    paleo::strat::SeismicSliceVote vote;
    vote.classCode = row.value("classCode").toInt(); vote.weight = row.value("weight").toDouble();
    votes.append(vote);
  }
  if (votes.isEmpty()) return {false, QObject::tr("请填写至少一行切片投票（相码,权重）")};
  const bool also = params.value("seismicAlsoBindSlices").toBool();
  const auto report = paleo::strat::publishSeismicSliceAggregate(ds, unit, votes,
      QStringLiteral("seismic_facies.%1.interval").arg(unit.subIntervalId),
      params.value("seismicMinCoverage", 0.5).toDouble(), also,
      QStringLiteral("seismic_facies.%1.slice").arg(unit.subIntervalId));
  if (!report.ok) return {false, QObject::tr("层段综合发布失败：%1").arg(report.error)};
  return {true, QObject::tr("已发布层段综合：相码 %1，覆盖率 %2（%3）%4")
      .arg(report.aggregate.classCode).arg(report.aggregate.coverage, 0, 'f', 2)
      .arg(report.aggregate.strategyId).arg(also ? QObject::tr("；并已登记切片绑定") : QString())};
}
