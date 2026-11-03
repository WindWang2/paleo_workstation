// 层：功能
#include "wellfaciesworkflow.h"
#include "derivedassets.h"
#include "catalog/datacatalog.h"
#include "services/subintervalfactors.h"
#include "services/analysisunitcontext.h"
#include "analysisresultsync.h"
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
void WellFaciesWorkflow::setAllowPartialPrediction(bool enabled) {
  if(m_allowPartialPrediction==enabled)return;
  if(m_busy)cancel();m_allowPartialPrediction=enabled;updateInput();
}

void WellFaciesWorkflow::setStratigraphicTrackFilter(const QString &trackName,
                                                     const QString &unitName,
                                                     const QString &schemeId) {
  if (m_stratTrackName == trackName && m_stratUnitName == unitName && m_stratSchemeId == schemeId)
    return;
  if (m_busy) cancel();
  m_stratTrackName = trackName.trimmed();
  m_stratUnitName = unitName.trimmed();
  m_stratSchemeId = schemeId.trimmed();
  emit resultCleared();
  updateInput();
}

QVariantList WellFaciesWorkflow::availableStratigraphicTracks() const {
  QVariantList out;
  const QString dir = projectDir();
  if (dir.isEmpty()) return out;
  std::vector<paleo::data::StratigraphicTrackDescriptor> tracks;
  if (!paleo::strat::listProjectStratigraphicTracks(dir, tracks, nullptr))
    return out;
  for (const auto &t : tracks) {
    QStringList units;
    for (const auto &u : t.unitNames)
      units.append(QString::fromStdString(u));
    out.append(QVariantMap{
        {QStringLiteral("trackName"), QString::fromStdString(t.trackName)},
        {QStringLiteral("trackType"), QString::fromStdString(t.trackType)},
        {QStringLiteral("schemeId"), QString::fromStdString(t.schemeId)},
        {QStringLiteral("source"),
         t.source == paleo::data::StratigraphicTrackSource::SeismicFramework
             ? QStringLiteral("seismic_framework")
             : QStringLiteral("well_interval_track")},
        {QStringLiteral("wellCount"), t.wellCount},
        {QStringLiteral("unitNames"), units}});
  }
  return out;
}

bool WellFaciesWorkflow::scopePlanToUnit(QString *error, WellComposite::ComprehensiveWellData *input) {
  if (!m_hasAnalysisContext && m_stratUnitName.isEmpty()) return true; // standalone legacy consumer
  if (!m_hasAnalysisContext && !m_stratUnitName.isEmpty() && m_data.modelWellId > 0 && !projectDir().isEmpty()) {
    paleo::data::WellIntervalLogQuery q;
    q.wellIds = {m_data.modelWellId};
    q.stratigraphy.source =
        (m_stratTrackName == QStringLiteral("地震小层") || m_stratTrackName == QStringLiteral("地震层位"))
            ? paleo::data::StratigraphicTrackSource::SeismicFramework
            : paleo::data::StratigraphicTrackSource::WellIntervalTrack;
    q.stratigraphy.schemeId = m_stratSchemeId.toStdString();
    q.stratigraphy.trackName = m_stratTrackName.toStdString();
    q.stratigraphy.unitName = m_stratUnitName.toStdString();
    q.stratigraphy.subIntervalId.value = m_stratUnitName.toStdString();
    q.stratigraphy.intervalId.value = m_stratUnitName.toStdString();
    std::vector<paleo::data::WellStratigraphicLogSlice> slices;
    QString qErr;
    if (!paleo::strat::queryProjectWellLogsByStratigraphy(projectDir(), q, slices, &qErr) || slices.empty()) {
      *error = qErr.isEmpty() ? tr("按地层道查询测井层段失败") : qErr;
      return false;
    }
    const auto &slice = slices.front();
    if (!slice.resolved) {
      *error = slice.unresolvedReason.empty() ? tr("该井在所选地层道上没有可解析的层段")
                                              : QString::fromStdString(slice.unresolvedReason);
      return false;
    }
    if ((!slice.depthType.empty() && slice.depthType != "MD") ||
        (!slice.depthUnit.empty() && slice.depthUnit != "m")) {
      *error = tr("所选地层道的深度不是 MD 米制（%1/%2），不能直接截取预测输入")
                   .arg(QString::fromStdString(slice.depthType), QString::fromStdString(slice.depthUnit));
      return false;
    }
    if(input){
      paleo::data::WellSubIntervalRange range;range.resolved=true;range.depthType="MD";range.depthUnit="m";
      range.top=slice.topDepth;range.bottom=slice.bottomDepth;
      *input=paleo::strat::scopeWellData(*input,range);return true;
    }
    QVector<WellFaciesRun> runs;
    for (auto run : m_plan.runs) {
      QJsonArray rows;
      for (const auto &value : run.input.rows) {
        const double depth = value.toObject().value(QStringLiteral("深度")).toDouble(NAN);
        // 与子层段口径一致：半开区间 [top, bottom)，相邻层段不重复取点。
        if (std::isfinite(depth) && depth >= slice.topDepth && depth < slice.bottomDepth)
          rows.append(value);
      }
      if (rows.size() < run.model.window) continue;
      run.input.rows = rows;
      runs.append(run);
    }
    m_plan.runs = runs;
    if (runs.isEmpty()) { *error = tr("当前层段内有效测井样点不足"); return false; }
    return true;
  }
  if (!m_analysisUnit.isValid() || m_analysisUnit.aggregate) {
    *error = tr("请先选择具体分析子层段（S1–S4）"); return false;
  }
  paleo::strat::UnitSelection frozen;
  if(!paleo::strat::freezeAnalysisUnit(projectDir(),m_analysisUnit,frozen,error))return false;
  m_analysisUnit=frozen;
  const auto wells = m_catalog ? m_catalog->wellsMatchingName(m_data.wellName) : QStringList{};
  if(m_data.modelWellId<=0&&wells.size()==1)m_data.modelWellId=m_catalog->entityById(wells.first()).extra.value("areaModelWellId").toLongLong();
  if (wells.size() != 1 && m_data.modelWellId <= 0) { *error = tr("当前井身份未唯一解析"); return false; }
  paleo::data::StratigraphicScheme scheme;
  if (!paleo::strat::loadProjectSchemeFromDir(projectDir(), m_analysisUnit.schemeId,
       m_analysisUnit.revision, scheme, nullptr, error)) return false;
  const auto range=paleo::strat::analysisWellRange(scheme,m_analysisUnit,
      wells.size()==1?wells.first():QString(),m_data.modelWellId);
  if (!range.resolved || range.depthType != "MD" || range.depthUnit != "m") {
    *error = range.reason.empty() ? tr("分析层段缺少有效 MD 米深度窗") : QString::fromStdString(range.reason);
    return false;
  }
  m_analysisWellId=range.wellId;
  if(input){*input=paleo::strat::scopeWellData(*input,range);return true;}
  QVector<WellFaciesRun> runs;
  for (auto run : m_plan.runs) {
    const auto rows=paleo::strat::scopeWellRows(run.input.rows,range);
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
  const bool geometryStale=!paleo::strat::analysisUnitGeometryCurrent(projectDir(),m_analysisUnit);
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
  extra.insert("modelWellId",m_data.modelWellId);extra.insert("analysisWellId",m_analysisWellId);
  if(geometryStale)extra.insert("stale",true);
  if(!registrar.commit(stage,m_unitParentVersions,"welllogfacies://"+result.jobId,extra,&error))return error;
  return syncAnalysisResults(m_catalog,projectDir());
}
