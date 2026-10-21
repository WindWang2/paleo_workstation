// 层：功能
// 预测专项 P0：测井预测的批量逐井准备检查（干跑）。
// 规则不在这里：请求组装与 predict() 共用 wellRequestEntry，逐井判定全部由
// WellPredictionService 的 prepare/plan（与正式运行同一链路）给出。
#include "mappingworkbench.h"
#include "../qgis/wellattributestore.h"
#include "wellpredictionservice.h"
#include <QSet>
#include <QUuid>

QVariantMap MappingWorkbench::wellRequestEntry(const QVariantMap &row,
                                               const QString &h) const {
  auto well = row;
  const QString id = row.value("id").toString();
  well.insert("vector_intervals", WellAttributeStore::rows(m_dir, m_layers, false, id));
  well.insert("vector_factors", WellAttributeStore::rows(m_dir, m_layers, true, id, h));
  well.insert("vector_formations", WellAttributeStore::rows(m_dir, m_layers, WellAttributeStore::Table::Formations, id));
  return well;
}

bool MappingWorkbench::checkWellReadiness(const QString &h, const QStringList &ids,
                                          QString *error) {
  const auto reject = [error](const QString &reason) {
    if (error) *error = reason;
    return false;
  };
  if (!ready(h, error))
    return false;
  if (busy())
    return reject(tr("预测运行中，完成后再检查"));
  if (!m_wellRemote)
    return reject(tr("测井远端预测未装配"));
  if (ids.isEmpty())
    return reject(tr("请至少勾选一口井"));
  RemotePredictionRequest request;
  request.id = QStringLiteral("readiness-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  request.horizon = h;
  request.kind = QStringLiteral("wells");
  QSet<QString> seen;
  for (const auto &v : inputs(QStringLiteral("wells"))) {
    const auto row = v.toMap();
    if (!ids.contains(row.value("id").toString())) continue;
    seen.insert(row.value("id").toString());
    request.wells << wellRequestEntry(row, h);
  }
  // 勾选了但不在可用输入里的井（缺坐标/源文件）同样如实计入不可预测。
  m_readinessMissing.clear();
  for (const auto &id : ids)
    if (!seen.contains(id)) m_readinessMissing << id;
  m_readinessId = request.id;
  m_readinessHorizon = h;
  if (request.wells.isEmpty()) {
    m_readinessId.clear();
    return reject(tr("所选井均缺少可读源文件、测井曲线或有效井坐标"));
  }
  m_wellRemote->check(request);
  return true;
}
