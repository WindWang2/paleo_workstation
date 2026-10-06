// 层：功能
#include "../domain/faciescatalog.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "mappingworkbench.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
namespace {
bool reject(QString *error, const QString &message) {
  if (error)
    *error = message;
  return false;
}
QVariantList decode(const QVariant &json) {
  return QJsonDocument::fromJson(json.toString().toUtf8()).toVariant().toList();
}
bool validEvidence(const QgsFeature &feature, QString *error) {
  const auto raw = feature.attribute("facies_evidence").toString().trimmed();
  if (raw.isEmpty())
    return true;
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(raw.toUtf8(), &parse);
  if (parse.error == QJsonParseError::NoError && doc.isArray())
    return true;
  return reject(
      error, QObject::tr("已有证据内容不是有效列表，请先修复；原证据未覆盖"));
}
QString encode(const QVariantList &list) {
  return QString::fromUtf8(
      QJsonDocument::fromVariant(list).toJson(QJsonDocument::Compact));
}
} // namespace
QString MappingWorkbench::displayMode(const QString &id) const {
  auto *layer = m_layers->layer(id);
  return layer ? layer->customProperty("paleo/faciesDisplayMode", "auto")
                     .toString()
               : QStringLiteral("auto");
}
QString MappingWorkbench::resolvedLevel(const QString &id) const {
  return FaciesHierarchy::resolveLevel(displayMode(id), m_displayScale);
}
QVariantList MappingWorkbench::displayLegend(const QString &id) const {
  auto *layer = m_layers->layer(id);
  if (!layer)
    return {};
  const auto cached = layer->customProperty("paleo/faciesDisplayLegend");
  return cached.isValid()
             ? cached.toList()
             : FaciesHierarchyRenderer::legend(
                   layer, versionForLayer(id).extra.value("facies").toList(),
                   resolvedLevel(id));
}
bool MappingWorkbench::setDisplayMode(const QString &id, const QString &mode,
                                      QString *error) {
  if (mode != "auto" && !FaciesHierarchy::levels().contains(mode))
    return reject(error, tr("请选择自动、相、亚相或微相"));
  auto *layer = m_layers->instantiate(id, error);
  if (!layer || versionForLayer(id).extra.value("facies").toList().isEmpty())
    return reject(error, tr("请选择相图件"));
  layer->setCustomProperty("paleo/faciesDisplayMode", mode);
  if (m_project && m_project->project())
    m_project->project()->setDirty(true);
  styleLayer(id);
  emit displayChanged(id);
  return true;
}
void MappingWorkbench::updateDisplayScale(double scale) {
  m_displayScale = scale;
  for (const auto &d : m_layers->declared()) {
    auto *layer = m_layers->layer(d.layerId);
    if (!layer || !layer->customProperty("paleo/faciesResolvedLevel").isValid())
      continue;
    const auto level = resolvedLevel(d.layerId);
    if (layer->customProperty("paleo/faciesResolvedLevel").toString() !=
        level) {
      styleLayer(d.layerId);
      emit displayChanged(d.layerId);
    }
  }
}
bool MappingWorkbench::selectHierarchyMembers(const QString &id,
                                              const QString &level,
                                              QString *error) {
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  if (!v || !FaciesHierarchy::levels().contains(level) ||
      v->selectedFeatureIds().isEmpty())
    return reject(error, tr("请先在画布选中相要素"));
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  QSet<QString> keys;
  for (auto fid : v->selectedFeatureIds())
    keys.insert(FaciesHierarchy::key(
        FaciesCatalog::find(schema,
                            v->getFeature(fid).attribute("facies_code")),
        level));
  QgsFeatureIds members;
  auto it = v->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    if (keys.contains(FaciesHierarchy::key(
            FaciesCatalog::find(schema, f.attribute("facies_code")), level)))
      members.insert(f.id());
  v->selectByIds(members);
  return true;
}
bool MappingWorkbench::assignHierarchy(const QString &id,
                                       const QList<qint64> &ids, int code,
                                       const QString &level, QString *error) {
  if (!FaciesHierarchy::levels().contains(level))
    return reject(error, tr("请选择编辑层级"));
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!id.startsWith("draft.") || !layer || layer->readOnly() || ids.isEmpty())
    return reject(error, tr("请在编辑副本中选择要素"));
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  bool defined = false;
  for (const auto &f : schema)
    defined |= f.toMap().value("code").toInt() == code;
  if (!defined)
    return reject(error, tr("请选择当前图件分类中的目标类别"));
  // Parent edits apply to the entire selected parent class, including
  // unselected leaves.
  QSet<QString> selected;
  for (auto fid : ids) {
    const auto f = layer->getFeature(fid);
    if (!f.isValid())
      return reject(error, tr("选中要素已不存在"));
    selected.insert(FaciesHierarchy::key(
        FaciesCatalog::find(schema, f.attribute("facies_code")), level));
  }
  if (!layer->isEditable() && !layer->startEditing())
    return reject(error, tr("无法开始编辑"));
  layer->beginEditCommand(tr("同步相、亚相、微相分类"));
  auto it = layer->getFeatures();
  QgsFeature f;
  bool ok = true;
  while (it.nextFeature(f)) {
    if (level == "micro_facies"
            ? !ids.contains(f.id())
            : !selected.contains(FaciesHierarchy::key(
                  FaciesCatalog::find(schema, f.attribute("facies_code")),
                  level)))
      continue;
    const int newCode = FaciesHierarchy::reassignedCode(
        schema, f.attribute("facies_code").toInt(), code, level);
    ok = layer->changeAttributeValue(
             f.id(), layer->fields().indexOf("facies_code"), newCode) &&
         ok;
    const auto attrs = FaciesCatalog::attributes(schema, newCode);
    for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
      ok = layer->changeAttributeValue(f.id(), layer->fields().indexOf(a.key()),
                                       a.value()) &&
           ok;
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      auto intervals = decode(f.attribute("facies_intervals"));
      for (auto &interval : intervals) {
        auto value = interval.toMap();
        value.insert("code", newCode);
        interval = value;
      }
      ok = layer->changeAttributeValue(
               f.id(), layer->fields().indexOf("facies_intervals"),
               encode(intervals)) &&
           ok;
    }
    if (!ok)
      break;
  }
  if (!ok) {
    layer->destroyEditCommand();
    return reject(error, tr("三级相分类更新失败，已撤销本次修改"));
  }
  layer->endEditCommand();
  emit faciesEdited(id);
  return true;
}
QList<qint64> MappingWorkbench::selectedFeatures(const QString &id) const {
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  return v ? v->selectedFeatureIds().values() : QList<qint64>();
}
QVariantList MappingWorkbench::evidence(const QString &id,
                                        const QList<qint64> &ids) const {
  QVariantList result;
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  if (!v || v->fields().indexOf("facies_evidence") < 0)
    return result;
  for (auto fid : ids)
    for (const auto &entry :
         decode(v->getFeature(fid).attribute("facies_evidence"))) {
      auto row = entry.toMap();
      row.insert("feature_id", fid);
      const auto schema = versionForLayer(id).extra.value("facies").toList();
      const auto current = FaciesHierarchy::key(
          FaciesCatalog::find(schema,
                              v->getFeature(fid).attribute("facies_code")),
          row.value("level").toString());
      row.insert("needs_review",
                 !row.value("category_key").toString().isEmpty() &&
                     row.value("category_key").toString() != current);
      result << row;
    }
  return result;
}
bool MappingWorkbench::addEvidence(const QString &id, const QList<qint64> &ids,
                                   const QVariantMap &entry, QString *error) {
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!id.startsWith("draft.") || !v || v->readOnly() || ids.isEmpty())
    return reject(error, tr("请在编辑副本中选择要素后添加证据"));
  const auto text = entry.value("text").toString().trimmed();
  const auto level = entry.value("level", resolvedLevel(id)).toString();
  if (text.isEmpty() || text.size() > 10000 ||
      !FaciesHierarchy::levels().contains(level))
    return reject(error, tr("证据内容须为 1–10000 字，并指定相层级"));
  for (auto fid : ids) {
    const auto feature = v->getFeature(fid);
    if (!feature.isValid())
      return reject(error, tr("选中要素已不存在"));
    if (v->fields().indexOf("facies_evidence") >= 0 &&
        !validEvidence(feature, error))
      return false;
  }
  QVariantMap row;
  row.insert("text", text);
  row.insert("level", level);
  row.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
  row.insert("created_at",
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  const auto source = entry.value("source_layer").toString();
  if (source == id)
    return reject(error, tr("证据来源请选择其他图件，或选择手工解释"));
  if (!source.isEmpty()) {
    row.insert("source_layer", source);
    const auto sourceVersion = ensureInputVersion(source, error);
    if (sourceVersion.isEmpty())
      return false;
    row.insert("source_version", sourceVersion);
    row.insert("source_title", declaration(source).title);
  }
  if (!v->isEditable() && !v->startEditing())
    return reject(error, tr("无法开始编辑"));
  v->beginEditCommand(tr("添加相解释证据"));
  bool ok = true;
  if (v->fields().indexOf("facies_evidence") < 0)
    ok = v->addAttribute(QgsField("facies_evidence", QMetaType::Type::QString));
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  for (auto fid : ids) {
    const auto f = v->getFeature(fid);
    auto list = decode(f.attribute("facies_evidence"));
    auto record = row;
    record.insert("category_path",
                  FaciesHierarchy::path(
                      FaciesCatalog::find(schema, f.attribute("facies_code")))
                      .join(" / "));
    record.insert(
        "category_key",
        FaciesHierarchy::key(
            FaciesCatalog::find(schema, f.attribute("facies_code")), level));
    list << record;
    ok = v->changeAttributeValue(fid, v->fields().indexOf("facies_evidence"),
                                 encode(list)) &&
         ok;
  }
  if (!ok) {
    v->destroyEditCommand();
    return reject(error, tr("证据保存失败，已撤销本次修改"));
  }
  v->endEditCommand();
  emit faciesEdited(id);
  return true;
}
bool MappingWorkbench::removeEvidence(const QString &id,
                                      const QList<qint64> &ids,
                                      const QString &evidenceId,
                                      QString *error) {
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!id.startsWith("draft.") || !v || v->readOnly() || ids.isEmpty() ||
      evidenceId.isEmpty() || v->fields().indexOf("facies_evidence") < 0)
    return reject(error, tr("请选择编辑副本中的证据"));
  for (auto fid : ids) {
    const auto feature = v->getFeature(fid);
    if (!feature.isValid())
      return reject(error, tr("选中要素已不存在"));
    if (v->fields().indexOf("facies_evidence") >= 0 &&
        !validEvidence(feature, error))
      return false;
  }
  if (!v->isEditable() && !v->startEditing())
    return reject(error, tr("无法开始编辑"));
  v->beginEditCommand(tr("移除相解释证据"));
  bool ok = true;
  for (auto fid : ids) {
    auto list = decode(v->getFeature(fid).attribute("facies_evidence"));
    for (int i = list.size() - 1; i >= 0; --i)
      if (list[i].toMap().value("id").toString() == evidenceId)
        list.removeAt(i);
    ok = v->changeAttributeValue(fid, v->fields().indexOf("facies_evidence"),
                                 encode(list)) &&
         ok;
  }
  if (!ok) {
    v->destroyEditCommand();
    return reject(error, tr("证据移除失败"));
  }
  v->endEditCommand();
  emit faciesEdited(id);
  return true;
}
