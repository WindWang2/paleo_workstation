// 层：功能
#include "../domain/faciescatalog.h"
#include "../domain/facieshierarchy.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "mappingworkbench.h"
#include "../services/faciesdisplaysettings.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <qgsproject.h>
#include <qgsfield.h>
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
  return FaciesHierarchy::productDisplayMode(versionForLayer(id).extra.value("kind").toString(),
      layer ? layer->customProperty("paleo/faciesDisplayMode", "auto").toString()
            : QStringLiteral("auto"));
}
QString MappingWorkbench::resolvedLevel(const QString &id) const {
  return FaciesHierarchy::resolveLevel(displayMode(id), m_displayScale, displayThresholds());
}
FaciesHierarchy::ScaleThresholds MappingWorkbench::displayThresholds() const {
  return FaciesDisplaySettings::instance().thresholds();
}
bool MappingWorkbench::setDisplayThresholds(double facies, double micro, QString *error) {
  return FaciesDisplaySettings::instance().save({facies, micro}, error);
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
  if (!supportsFaciesDisplay(id))
    return reject(error, tr("请选择相图件"));
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return reject(error, tr("请选择相图件"));
  layer->setCustomProperty("paleo/faciesDisplayMode", mode);
  if (m_project && m_project->project())
    m_project->project()->setDirty(true);
  styleLayer(id);
  emit displayChanged(id);
  return true;
}
QVariantMap MappingWorkbench::pinnedLevels(const QString &id) const {
  const auto *layer = m_layers->layer(id);
  return layer ? FaciesHierarchy::parsePins(
                     layer->customProperty("paleo/faciesPinnedLevels").toString())
               : QVariantMap();
}
bool MappingWorkbench::setPinnedLevel(const QString &id, const QString &nodeKey,
                                      const QString &mode, QString *error) {
  const bool remove = mode.isEmpty() || mode == "auto";
  if (!remove && !FaciesHierarchy::levels().contains(mode))
    return reject(error, tr("请选择相、亚相或微相"));
  if (!supportsFaciesDisplay(id))
    return reject(error, tr("请选择相图件"));
  bool known = false;
  for (const auto &node : FaciesHierarchy::pinCandidates(versionForLayer(id).extra.value("facies").toList()))
    known |= node.toMap().value("key").toString() == nodeKey;
  if (!known && !(remove && pinnedLevels(id).contains(nodeKey)))
    return reject(error, tr("请选择当前图件分类中的相类"));
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return reject(error, tr("请选择相图件"));
  auto pins = pinnedLevels(id);
  if (remove)
    pins.remove(nodeKey);
  else
    pins.insert(nodeKey, mode);
  const auto encoded = FaciesHierarchy::encodePins(pins);
  // 清空时写入 "{}" 而非删除属性：显式清空不应被版本记录中的固定项重新带回。
  layer->setCustomProperty("paleo/faciesPinnedLevels",
                           encoded.isEmpty() ? QStringLiteral("{}") : encoded);
  if (m_project && m_project->project())
    m_project->project()->setDirty(true);
  styleLayer(id);
  emit displayChanged(id);
  return true;
}
bool MappingWorkbench::isFaciesPolygon(const QString &id) const {
  const auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  return layer && layer->geometryType() == Qgis::GeometryType::Polygon &&
      supportsFaciesDisplay(id);
}
QString MappingWorkbench::featureDisplayMode(const QString &id, qint64 featureId) const {
  const auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  if (!layer || layer->fields().indexOf("facies_display_level") < 0)
    return QStringLiteral("auto");
  const auto mode = layer->getFeature(featureId).attribute("facies_display_level").toString();
  return FaciesHierarchy::levels().contains(mode) ? mode : QStringLiteral("auto");
}
bool MappingWorkbench::setFeatureDisplayMode(const QString &id, const QList<qint64> &ids,
                                            const QString &mode, QString *error) {
  if (mode != "auto" && !FaciesHierarchy::levels().contains(mode))
    return reject(error, tr("请选择跟随图层、相、亚相或微相"));
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!id.startsWith("draft.") || !layer || layer->readOnly() ||
      layer->geometryType() != Qgis::GeometryType::Polygon || ids.isEmpty() ||
      versionForLayer(id).extra.value("facies").toList().isEmpty())
    return reject(error, tr("请在相面编辑副本中选择要锁定的面"));
  for (auto fid : ids)
    if (!layer->getFeature(fid).isValid())
      return reject(error, tr("选中要素已不存在"));
  if (!layer->isEditable() && !layer->startEditing())
    return reject(error, tr("无法开始编辑"));
  layer->beginEditCommand(tr("设置选中相面的显示层级"));
  int field = layer->fields().indexOf("facies_display_level");
  if (field < 0) {
    if (!layer->addAttribute(QgsField("facies_display_level", QMetaType::Type::QString))) {
      layer->destroyEditCommand();
      return reject(error, tr("无法添加相面显示层级字段"));
    }
    field = layer->fields().indexOf("facies_display_level");
  }
  for (auto fid : ids)
    if (!layer->changeAttributeValue(fid, field, mode)) {
      layer->destroyEditCommand();
      return reject(error, tr("相面显示层级更新失败，已撤销本次修改"));
    }
  layer->endEditCommand(); // existing edit/undo observers refresh renderer and legends
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
    if (newCode != f.attribute("facies_code").toInt())
      for (const auto &field : {"value_min", "value_max", "confidence", "probability"})
        if (const int index = layer->fields().indexOf(field); index >= 0)
          ok = layer->changeAttributeValue(f.id(), index, QVariant()) && ok;
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
    ok = recordInterpretationEdit(id, f.id(), f.attribute("facies_code").toInt(), newCode, error) && ok;
    if (!ok) break;
  }
  if (!ok) {
    layer->destroyEditCommand();
    return reject(error, error && !error->isEmpty() ? tr("已撤销本次修改：%1").arg(*error) : tr("三级相分类更新失败，已撤销本次修改"));
  }
  layer->endEditCommand();
  emit faciesEdited(id);
  return true;
}
QList<qint64> MappingWorkbench::selectedFeatures(const QString &id) const {
  auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
  return v ? v->selectedFeatureIds().values() : QList<qint64>();
}
