// 层：功能
#include "../domain/faciescatalog.h"
#include "../io/lasparser.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "mappingworkbench.h"
#include <QJsonDocument>
#include <QMap>
#include <qgsvectorlayer.h>

namespace {
QVariantList intervals(const QVariant &value) {
  return QJsonDocument::fromJson(value.toString().toUtf8())
      .toVariant()
      .toList();
}
QString encodeIntervals(const QVariantList &value) {
  return QString::fromUtf8(
      QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
}
bool setAttribute(QgsVectorLayer *layer, QgsFeatureId id, const QString &name,
                  const QVariant &value) {
  const int field = layer->fields().indexOf(name);
  return field >= 0 && layer->changeAttributeValue(id, field, value);
}
bool setCode(QgsVectorLayer *layer, QgsFeatureId id, int code,
             const QVariantList &schema) {
  bool ok = setAttribute(layer, id, "facies_code", code);
  const auto values = FaciesCatalog::attributes(schema, code);
  for (auto it = values.cbegin(); it != values.cend(); ++it)
    ok = setAttribute(layer, id, it.key(), it.value()) && ok;
  return ok;
}
} // namespace
LasDoc MappingWorkbench::predictionLog(const QString &versionId) const {
  LasDoc result;
  if (!m_catalog || !m_catalog->isOpen() || versionId.isEmpty())
    return result;
  if (m_logVersion == versionId)
    return m_logCache;
  const auto path = DataCatalog::resolvedVersionPath(
      m_dir, m_catalog->versionById(versionId));
  result.ok =
      LasParser::parse(path, result.curveNames, result.curves, &result.error);
  m_logVersion = versionId;
  m_logCache = result;
  return result;
}
QVariantList MappingWorkbench::wellPredictions(const QString &id) const {
  QVariantList out;
  if (!m_catalog || !m_catalog->isOpen() || declaration(id).type != "vector")
    return out;
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id));
  if (!layer || layer->fields().indexOf("facies_intervals") < 0)
    return out;
  auto it = layer->getFeatures();
  QgsFeature feature;
  while (it.nextFeature(feature)) {
    QVariantMap row;
    for (const auto &name :
         {"id", "name", "facies_code", "log_version_id", "depth_mock"})
      row.insert(name, feature.attribute(name));
    row.insert("intervals", intervals(feature.attribute("facies_intervals")));
    row.insert("predicted",
               intervals(feature.attribute("predicted_intervals")));
    row.insert("feature_id", feature.id());
    out << row;
  }
  return out;
}
bool MappingWorkbench::assignFacies(const QString &id, const QList<qint64> &ids,
                                    int code, QString *error) {
  auto reject = [error](const QString &reason) {
    if (error)
      *error = reason;
    return false;
  };
  if (!id.startsWith("draft.") || ids.isEmpty())
    return reject(tr("请在人工编辑副本中选择要素"));
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  bool defined = false;
  for (const auto &v : schema)
    defined = defined || v.toMap().value("code").toInt() == code;
  if (!defined)
    return reject(tr("请选择图件所属分类中的相"));
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer || layer->readOnly())
    return reject(tr("此图件不可编辑，请先创建编辑副本"));
  for (auto fid : ids)
    if (!layer->getFeature(fid).isValid())
      return reject(tr("选择的要素已不存在"));
  if (!layer->isEditable() && !layer->startEditing())
    return reject(tr("无法开始编辑"));
  layer->beginEditCommand(tr("更改相类别"));
  bool ok = true;
  for (auto fid : ids) {
    ok = setCode(layer, fid, code, schema) && ok;
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      auto rows =
          intervals(layer->getFeature(fid).attribute("facies_intervals"));
      for (auto &v : rows) {
        auto row = v.toMap();
        row.insert("code", code);
        v = row;
      }
      ok =
          setAttribute(layer, fid, "facies_intervals", encodeIntervals(rows)) &&
          ok;
    }
  }
  if (!ok) {
    layer->destroyEditCommand();
    return reject(tr("相属性更新失败，已撤销本次修改"));
  }
  layer->endEditCommand();
  layer->triggerRepaint();
  emit faciesEdited(id);
  return true;
}
bool MappingWorkbench::reviseWellInterval(const QString &id,
                                          const QString &wellId, int index,
                                          int code, QString *error) {
  auto reject = [error](const QString &reason) {
    if (error)
      *error = reason;
    return false;
  };
  if (!id.startsWith("draft."))
    return reject(tr("先复制预测结果，再修订井段"));
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  bool defined = false;
  for (const auto &v : schema)
    defined = defined || v.toMap().value("code").toInt() == code;
  if (!defined)
    return reject(tr("请选择图件所属分类中的相"));
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer || layer->readOnly() ||
      layer->fields().indexOf("facies_intervals") < 0)
    return reject(tr("请选择测井相工作副本"));
  auto features = layer->getFeatures();
  QgsFeature f;
  bool found = false;
  while (features.nextFeature(f))
    if (f.attribute("id").toString() == wellId) {
      found = true;
      break;
    }
  if (!found)
    return reject(tr("该井不在当前预测结果中"));
  auto rows = intervals(f.attribute("facies_intervals"));
  if (index < 0 || index >= rows.size())
    return reject(tr("请选择有效井段"));
  auto row = rows[index].toMap();
  row.insert("code", code);
  rows[index] = row;
  QMap<int, double> thickness;
  for (const auto &v : rows) {
    auto r = v.toMap();
    thickness[r.value("code").toInt()] +=
        r.value("bottom").toDouble() - r.value("top").toDouble();
  }
  int dominant = thickness.firstKey();
  for (auto it = thickness.cbegin(); it != thickness.cend(); ++it)
    if (it.value() > thickness.value(dominant))
      dominant = it.key();
  if (!layer->isEditable() && !layer->startEditing())
    return reject(tr("无法开始编辑"));
  layer->beginEditCommand(tr("修订测井相井段"));
  if (!setAttribute(layer, f.id(), "facies_intervals", encodeIntervals(rows)) ||
      !setCode(layer, f.id(), dominant, schema)) {
    layer->destroyEditCommand();
    return reject(tr("井段修订失败，已撤销本次修改"));
  }
  layer->endEditCommand();
  layer->triggerRepaint();
  emit faciesEdited(id);
  return true;
}
