// 层：功能
#include "../ai/chat/llmclient.h"
#include "../domain/faciestaxonomy.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "mappingexplanationservice.h"
#include "mappingworkbench.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUuid>

void MappingWorkbench::setAnalysisUnit(const paleo::strat::UnitSelection &unit) {
  if (m_analysisUnit == unit)
    return;
  if (!m_request.id.isEmpty() || !m_readinessId.isEmpty())
    cancelPrediction();
  m_analysisUnit = unit;
  clearEditEvidence();
}

void MappingWorkbench::setManualEvidenceRequired(bool required) {
  if (m_manualEvidenceRequired == required)
    return;
  m_manualEvidenceRequired = required;
  if (!required)
    clearEditEvidence();
}

void MappingWorkbench::clearEditEvidence() {
  m_editEvidence.clear();
  emit editEvidenceChanged();
}

QVariantMap MappingWorkbench::editEvidence(const QString &horizon) const {
  return m_editEvidence.value("horizon").toString() == horizon ? m_editEvidence : QVariantMap{};
}

bool MappingWorkbench::needsEditEvidence(const QString &layerId) const {
  // 编图过程不必需预先填写解释说明；仅当调用方显式开启严格手动依据模式
  // (setManualEvidenceRequired(true)) 时才要求先设置本次依据。
  return m_manualEvidenceRequired && editEvidence(declaration(layerId).horizon).isEmpty();
}

bool MappingWorkbench::setEditEvidence(const QString &horizon, const QVariantMap &context, QString *error) {
  if (!ready(horizon, error))
    return false;
  auto reject = [error](const QString &message) {
    if (error)
      *error = message;
    return false;
  };
  const auto text = context.value("text").toString().trimmed();
  const auto reason = context.value("reason").toString().trimmed();
  if (text.isEmpty() || text.size() + reason.size() > 9000)
    return reject(tr("请填写解释说明，说明与修改目的合计不超过 9000 字"));
  QVariantList references;
  QSet<QString> seen;
  for (const auto &id : context.value("source_versions").toStringList()) {
    if (seen.contains(id))
      continue;
    seen.insert(id);
    const auto version = m_catalog->versionById(id);
    const auto sourceHorizon = version.extra.value("horizon").toString();
    if (version.id.isEmpty() || sourceHorizon != horizon)
      return reject(tr("参考证据必须为本层位已保存的图件版本，请重新选取"));
    if (!QFileInfo::exists(QDir(m_dir).filePath(version.path)))
      return reject(tr("参考证据文件已不存在，请重新选取"));
    references << QVariantMap{
        {"source_version", id},
        {"source_asset", version.assetId},
        {"source_horizon", sourceHorizon},
        {"source_title", version.extra.value("title", m_catalog->assetById(version.assetId).displayName)},
        {"role", "REFERENCES"},
        {"type", "reference"},
        {"text", text}};
  }
  if (references.size() > 16)
    return reject(tr("本次解释最多引用 16 个参考版本"));
  m_editEvidence = {{"horizon", horizon},
                    {"text", text},
                    {"reason", reason},
                    {"references", references},
                    {"source_versions", context.value("source_versions")}};
  emit editEvidenceChanged();
  return true;
}

void MappingWorkbench::setLlmConfig(const LlmConfig &config) {
  if (m_explanationSvc)
    m_explanationSvc->setLlmConfig(config);
}

bool MappingWorkbench::aiExplanationAvailable() const {
  return m_explanationSvc && m_explanationSvc->aiAvailable();
}

void MappingWorkbench::setAutoExplainEnabled(bool enabled) {
  if (!m_explanationSvc)
    return;
  if (!enabled)
    m_pendingExplanations.clear();
  m_explanationSvc->setEnabled(enabled);
}

bool MappingWorkbench::autoExplainEnabled() const {
  return m_explanationSvc && m_explanationSvc->enabled();
}

bool MappingWorkbench::explanationBusy() const {
  return m_explanationSvc && m_explanationSvc->busy();
}

QString MappingWorkbench::explanationStatus() const {
  return m_explanationSvc ? m_explanationSvc->statusText() : QString();
}

QVariantMap MappingWorkbench::lastAutoExplanation(const QString &horizon) const {
  return m_lastAutoExplanations.value(horizon);
}

QString MappingWorkbench::draftExplanationsPath(const QString &layerId) const {
  if (m_dir.isEmpty() || !layerId.startsWith(QLatin1String("draft.")))
    return {};
  const auto version = versionForLayer(layerId);
  QString workingPath = version.extra.value(QStringLiteral("working_path")).toString();
  if (workingPath.isEmpty())
    workingPath = QStringLiteral("artifacts/working/%1/features.gpkg").arg(layerId);
  const QString fullPath = QDir(m_dir).filePath(workingPath);
  const QString workingRoot = QDir::cleanPath(QDir(m_dir).absoluteFilePath(QStringLiteral("artifacts/working")));
  const QString parentDir = QDir::cleanPath(QFileInfo(fullPath).absolutePath());
  if (!parentDir.startsWith(workingRoot + QLatin1Char('/')))
    return {};
  return QDir(parentDir).filePath(QStringLiteral("auto-explanations.json"));
}

QHash<QString, QVariantMap> MappingWorkbench::loadDraftExplanations(const QString &layerId) const {
  if (m_loadedDraftExplanations.contains(layerId))
    return m_generatedExplanations.value(layerId);
  m_loadedDraftExplanations.insert(layerId);
  QHash<QString, QVariantMap> loaded = m_generatedExplanations.value(layerId);
  const QString sidecar = draftExplanationsPath(layerId);
  if (!sidecar.isEmpty()) {
    QFile file(sidecar);
    if (file.open(QIODevice::ReadOnly)) {
      const auto doc = QJsonDocument::fromJson(file.readAll());
      if (doc.isObject()) {
        const auto entries = doc.object().value(QStringLiteral("entries")).toVariant().toMap();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
          if (!loaded.contains(it.key()) && it.value().typeId() == QMetaType::QVariantMap)
            loaded.insert(it.key(), it.value().toMap());
      }
    }
  }
  const auto stored = versionForLayer(layerId).extra.value(QStringLiteral("auto_explanations")).toMap();
  for (auto it = stored.cbegin(); it != stored.cend(); ++it)
    if (!loaded.contains(it.key()) && it.value().typeId() == QMetaType::QVariantMap)
      loaded.insert(it.key(), it.value().toMap());
  if (!loaded.isEmpty())
    m_generatedExplanations.insert(layerId, loaded);
  return loaded;
}

void MappingWorkbench::saveDraftExplanations(const QString &layerId) const {
  const QString sidecar = draftExplanationsPath(layerId);
  if (sidecar.isEmpty() || !m_catalog || m_catalog->refusesWrites())
    return;
  const auto entries = m_generatedExplanations.value(layerId);
  QVariantMap serialized;
  for (auto it = entries.cbegin(); it != entries.cend(); ++it)
    serialized.insert(it.key(), it.value());
  QDir().mkpath(QFileInfo(sidecar).absolutePath());
  QSaveFile out(sidecar);
  const auto bytes = QJsonDocument(QJsonObject{
      {QStringLiteral("schema"), QStringLiteral("paleo.mapping.auto_explanations.v1")},
      {QStringLiteral("layer_id"), layerId},
      {QStringLiteral("entries"), QJsonObject::fromVariantMap(serialized)}}).toJson(QJsonDocument::Compact);
  if (out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size())
    out.commit();
}

QVariantMap MappingWorkbench::generatedExplanationForFeature(const QString &layerId,
                                                             const QString &featureUid) const {
  if (layerId.isEmpty() || featureUid.isEmpty())
    return {};
  const auto entries = loadDraftExplanations(layerId);
  return entries.value(featureUid);
}

void MappingWorkbench::refreshLastExplanation(const QString &layerId) {
  if (m_lastAutoExplanations.isEmpty())
    return;
  const auto horizon = declaration(layerId).horizon;
  const auto record = m_lastAutoExplanations.value(horizon);
  if (record.value(QStringLiteral("layer_id")).toString() == layerId &&
      !record.value(QStringLiteral("feature_uid")).toString().isEmpty() &&
      !explanationTargetMatches(MappingExplanationResult::fromVariantMap(record))) {
    m_lastAutoExplanations.remove(horizon);
    emit explanationStatusChanged();
  }
}

void MappingWorkbench::scheduleAutoExplanation(const QString &layerId,
                                               const QString &featureUid,
                                               const QString &kind,
                                               int beforeCode) {
  if (!autoExplainEnabled() || layerId.isEmpty())
    return;
  const auto horizon = declaration(layerId).horizon;
  if (horizon.isEmpty() || !editEvidence(horizon).isEmpty())
    return;
  for (auto &existing : m_pendingExplanations) {
    if (existing.layerId == layerId && existing.featureUid == featureUid) {
      existing.kind = kind;
      if (beforeCode != 0)
        existing.beforeCode = beforeCode;
      break;
    }
  }
  const bool found = std::any_of(m_pendingExplanations.cbegin(), m_pendingExplanations.cend(),
                                 [&](const PendingExplainItem &item) {
                                   return item.layerId == layerId && item.featureUid == featureUid;
                                 });
  if (!found)
    m_pendingExplanations.append({layerId, featureUid, kind, beforeCode});
  if (!m_explainFlushQueued) {
    m_explainFlushQueued = true;
    QTimer::singleShot(0, this, &MappingWorkbench::flushPendingExplanations);
  }
}

void MappingWorkbench::flushPendingExplanations() {
  m_explainFlushQueued = false;
  if (!m_explanationSvc || !m_explanationSvc->enabled()) {
    m_pendingExplanations.clear();
    return;
  }
  const auto items = std::move(m_pendingExplanations);
  m_pendingExplanations.clear();
  for (const auto &item : items) {
    const auto horizon = declaration(item.layerId).horizon;
    if (horizon.isEmpty() || !editEvidence(horizon).isEmpty())
      continue;
    MappingExplanationRequest request;
    if (!captureExplanationSnapshot(horizon, item.layerId, item.featureUid, item.kind,
                                    item.beforeCode, {}, false, &request, nullptr))
      continue;
    m_explanationSvc->enqueue(std::move(request));
  }
}

bool MappingWorkbench::requestBackgroundExplanation(const QString &horizon,
                                                    const QString &layerId,
                                                    const QList<qint64> &featureIds,
                                                    const QStringList &referenceVersionIds,
                                                    bool applyToEditBasis,
                                                    QString *error) {
  if (!m_explanationSvc) {
    if (error)
      *error = tr("后台解释服务尚未初始化");
    return false;
  }
  if (horizon.isEmpty()) {
    if (error)
      *error = tr("请先选择层位");
    return false;
  }
  QString featureUid;
  if (!layerId.isEmpty() && !featureIds.isEmpty()) {
    const auto summary = featureEvidenceSummary(layerId, {featureIds.first()});
    featureUid = summary.value(QStringLiteral("feature_uid")).toString();
  }
  MappingExplanationRequest request;
  if (!captureExplanationSnapshot(horizon, layerId, featureUid,
                                  QStringLiteral("manual_request"), 0,
                                  referenceVersionIds, applyToEditBasis, &request, error))
    return false;
  m_explanationSvc->enqueue(std::move(request));
  return true;
}

void MappingWorkbench::cancelBackgroundExplanation() {
  m_pendingExplanations.clear();
  if (m_explanationSvc)
    m_explanationSvc->cancelAll();
}

void MappingWorkbench::onBackgroundExplanationReady(const MappingExplanationResult &result) {
  if (result.cancelled || result.text.trimmed().isEmpty())
    return;
  if (m_dir != result.projectDir || !explanationTargetMatches(result))
    return;
  auto record = result.toVariantMap();
  record.insert(QStringLiteral("materialized"), false);
  const QString key = result.featureUid.isEmpty() ? QStringLiteral("__map__") : result.featureUid;
  if (!result.layerId.isEmpty()) {
    loadDraftExplanations(result.layerId);
    m_generatedExplanations[result.layerId].insert(key, record);
    saveDraftExplanations(result.layerId);
  }
  if (!result.horizon.isEmpty())
    m_lastAutoExplanations.insert(result.horizon, record);
  emit autoExplanationReady(result.horizon, record);
  emit explanationStatusChanged();
  if (!result.layerId.isEmpty())
    emit displayChanged(result.layerId);
}

QString MappingWorkbench::createBlankMap(const QString &horizon, QString *error, const QVariantMap &boundary) {
  if (!ready(horizon, error))
    return {};
  QVariantList schema;
  if (!FaciesTaxonomy::normalize(facies(horizon), &schema, error))
    return {};
  const auto draft = "draft." + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto relative = QStringLiteral("artifacts/working/%1/features.gpkg").arg(draft);
  const auto path = QDir(m_dir).filePath(relative);
  if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
      !MappingArtifactWriter::emptyFacies(path, schema, error, boundary.value("crs").toString())) {
    QDir(QFileInfo(path).absolutePath()).removeRecursively();
    return {};
  }
  QVariantMap extra{{"draft_id", draft},
                          {"working_path", relative},
                          {"facies", schema},
                          {"requires_edit_evidence", false},
                          {"hierarchy_model", "shared-polygon-partition-v1"},
                          {"method", "manual-blank-map"},
                          {"display_mode", "micro_facies"}};
  if (!boundary.isEmpty())
    extra.insert("initial_boundary", QVariantMap{{"layer", boundary.value("layer")}, {"name", boundary.value("name")},
        {"crs", boundary.value("crs")}, {"digest", QString::fromLatin1(boundary.value("digest").toByteArray())}});
  const auto product = record(path, horizon, "edited_facies", tr("%1 新建古地理图").arg(horizon), "vector",
                              {}, extra, error, "|layername=features", draft);
  if (product.isEmpty()) {
    QDir(QFileInfo(path).absolutePath()).removeRecursively();
    return {};
  }
  auto declaration = m_layers->declaration(product);
  declaration.layerId = draft;
  declaration.source = path + "|layername=features";
  declaration.title += tr(" · 工作副本");
  declaration.group = "05_PaleoMap";
  if (!m_layers->declare(declaration, error)) {
    const auto registered = versionForLayer(product);
    m_layers->removeDeclaration(product);
    if (m_catalog && !registered.assetId.isEmpty()) {
      QString dropError;
      m_catalog->removeAsset(registered.assetId, &dropError);
      const QString bytes = DataCatalog::resolvedVersionPath(m_dir, registered);
      if (!bytes.isEmpty()) {
        QFile::setPermissions(bytes, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile::remove(bytes);
      }
    }
    QDir(QFileInfo(path).absolutePath()).removeRecursively();
    if (error && error->isEmpty())
      *error = tr("空白古地理图已撤回：图层声明失败");
    return {};
  }
  emit productReady(horizon, draft);
  return draft;
}
