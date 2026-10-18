// 层：功能
#include "../domain/faciestaxonomy.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "mappingworkbench.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

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
  if (text.isEmpty() || text.size() + reason.size() > 9000 || reason.isEmpty())
    return reject(tr("请填写解释依据和修改目的（合计不超过 9000 字）"));
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

QString MappingWorkbench::createBlankMap(const QString &horizon, QString *error) {
  if (!ready(horizon, error))
    return {};
  QVariantList schema;
  if (!FaciesTaxonomy::normalize(facies(horizon), &schema, error))
    return {};
  const auto draft = "draft." + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto relative = QStringLiteral("artifacts/working/%1/features.gpkg").arg(draft);
  const auto path = QDir(m_dir).filePath(relative);
  if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
      !MappingArtifactWriter::emptyFacies(path, schema, error)) {
    QDir(QFileInfo(path).absolutePath()).removeRecursively();
    return {};
  }
  const QVariantMap extra{{"draft_id", draft},
                          {"working_path", relative},
                          {"facies", schema},
                          {"requires_edit_evidence", true},
                          {"hierarchy_model", "shared-polygon-partition-v1"},
                          {"method", "manual-blank-map"},
                          {"display_mode", "micro_facies"}};
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
  if (!m_layers->declare(declaration, error))
    return {};
  emit productReady(horizon, draft);
  return draft;
}
