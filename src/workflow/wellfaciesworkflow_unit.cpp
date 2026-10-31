// 层：功能
#include "wellfaciesworkflow.h"
#include "derivedassets.h"
#include "catalog/datacatalog.h"
#include "services/subintervalfactors.h"
#include "paleo_data/stratigraphic_framework.h"
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

void WellFaciesWorkflow::setAnalysisUnit(const paleo::strat::UnitSelection &unit) {
  if (m_hasAnalysisContext && paleo::strat::toMap(unit) == paleo::strat::toMap(m_analysisUnit)) return;
  if (m_busy) cancel();
  m_hasAnalysisContext = true;
  m_analysisUnit = unit;
  emit resultCleared();
  updateInput();
}

bool WellFaciesWorkflow::scopePlanToUnit(QString *error) {
  if (!m_hasAnalysisContext) return true; // standalone legacy consumer
  if (!m_analysisUnit.isValid() || m_analysisUnit.aggregate) {
    *error = tr("请先选择具体分析子层段（S1–S4）"); return false;
  }
  const auto wells = m_catalog ? m_catalog->wellsMatchingName(m_data.wellName) : QStringList{};
  if (wells.size() != 1) { *error = tr("当前井身份未唯一解析"); return false; }
  paleo::data::StratigraphicScheme scheme;
  if (!paleo::strat::loadProjectSchemeFromDir(projectDir(), m_analysisUnit.schemeId,
       m_analysisUnit.revision, scheme, nullptr, error)) return false;
  const auto range = paleo::data::framework::wellRange(scheme, paleo::strat::stableWellKey(wells.first()),
       {m_analysisUnit.subIntervalId.toStdString()});
  if (!range.resolved || range.depthType != "MD" || range.depthUnit != "m") {
    *error = range.reason.empty() ? tr("分析层段缺少有效 MD 米深度窗") : QString::fromStdString(range.reason);
    return false;
  }
  QVector<WellFaciesRun> runs;
  for (auto run : m_plan.runs) {
    QJsonArray rows;
    for (const auto &value : run.input.rows) {
      const double depth = value.toObject().value(QStringLiteral("深度")).toDouble(NAN);
      if (std::isfinite(depth) && depth >= range.top &&
          (depth < range.bottom || (range.index == 4 && depth == range.bottom))) rows.append(value);
    }
    if (rows.size() < run.model.window) continue;
    run.input.rows = rows;
    runs.append(run);
  }
  m_plan.runs = runs;
  if (runs.isEmpty()) { *error = tr("当前子层段内有效测井样点不足"); return false; }
  return true;
}

QString WellFaciesWorkflow::publishUnitResult(const WellFaciesResult &result) {
  if (!m_hasAnalysisContext) return {};
  DerivedAssetRegistrar registrar(m_catalog, projectDir());
  QString error;
  const auto stage = registrar.stage("well_facies_prediction",
      tr("%1 · %2 测井相").arg(m_data.wellName, paleo::strat::layerToken(m_analysisUnit)),
      "prediction.json", &error);
  if (!stage.isValid()) return error;
  QJsonArray intervals;
  for (const auto &iv : result.intervals)
    intervals.append(QJsonObject{{"top", iv.topDepth}, {"base", iv.bottomDepth}, {"facies", iv.text}});
  QJsonArray confidence;
  for (qsizetype i = 0; i < qMin(result.confidence.depths.size(), result.confidence.values.size()); ++i)
    confidence.append(QJsonObject{{"depth", result.confidence.depths[i]}, {"value", result.confidence.values[i]}});
  const auto bytes = QJsonDocument(QJsonObject{{"response", result.response}, {"intervals", intervals},
      {"confidence", confidence}, {"facies", QJsonArray::fromVariantList(result.facies)}}).toJson();
  QSaveFile file(stage.absolutePath);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
  QVariantMap extra{{"unitSelection", paleo::strat::toMap(m_analysisUnit)},
                    {"modelName", result.modelName}, {"modelVersion", result.modelVersion}, {"jobId", result.jobId}};
  const auto wells = m_catalog->wellsMatchingName(m_data.wellName);
  if (wells.size() == 1) extra.insert("wellEntityId", wells.first());
  for (const auto &id : m_unitParentVersions) {
    const auto v = m_catalog->versionById(id);
    if (v.id.isEmpty() || m_catalog->currentVersion(v.assetId).id != id) extra.insert("stale", true);
  }
  return registrar.commit(stage, m_unitParentVersions, "welllogfacies://" + result.jobId, extra, &error) ? QString() : error;
}
