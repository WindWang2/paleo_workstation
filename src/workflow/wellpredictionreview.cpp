// 层：功能
#include "../domain/faciescatalog.h"
#include "../io/welllogread.h"
#include "../domain/wellnumeric.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "mappingworkbench.h"
#include <QFileInfo>
#include <qgsvectorlayer.h>

namespace {
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
  LasHeaderInfo header;
  result.ok = WellLogRead::parse(path, header, result.curves, &result.error);
  result.curveNames = header.curveNames;
  if (result.ok && WellLogRead::detect(path) != WellLogFormat::Las) {
    const auto scale = result.curves.isEmpty() ? std::optional<double>() :
        paleo::wellnumeric::depthScaleToMetres(result.curves.first().unit);
    if (header.indexBasis != QLatin1String("MD") || !scale) {
      result.ok = false;
      result.error = tr("%1：测井预测对比需要已声明米或英尺单位的 MD 测深；当前索引为 %2，单位为 %3，请先转换后查看。")
          .arg(QFileInfo(path).fileName(), header.indexBasis.isEmpty() ? tr("未声明") : header.indexBasis,
               result.curves.isEmpty() ? tr("未知") : result.curves.first().unit);
      result.curves.clear();
    } else {
      for (double &depth : result.curves.first().values) depth *= *scale;
      result.curves.first().unit = QStringLiteral("m");
    }
  }
  m_logVersion = versionId;
  m_logCache = result;
  return result;
}
bool MappingWorkbench::isWellPrediction(const QString &id) const {
  if (auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->layer(id)))
    return layer->fields().indexOf("facies_intervals") >= 0;
  const auto extra = versionForLayer(id).extra;
  return extra.value("kind") == "wells_prediction" ||
      extra.value("result_kind") == "formation_prediction" ||
      extra.value("result_kind") == "dominant_facies";
}
QVariantList MappingWorkbench::wellPredictions(const QString &id) const {
  QVariantList out;
  if (!m_catalog || !m_catalog->isOpen() || declaration(id).type != "vector")
    return out;
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id));
  if (!layer || layer->fields().indexOf("facies_intervals") < 0)
    return out;
  auto it = layer->getFeatures();
  const auto schema = versionForLayer(id).extra.value("facies").toList();
  const auto horizon = declaration(id).horizon;
  QgsFeature feature;
  while (it.nextFeature(feature)) {
    QVariantMap source;
    for (int i = 0; i < feature.fields().size(); ++i)
      source.insert(feature.fields().at(i).name(), feature.attribute(i));
    auto row = FaciesCatalog::formationPrediction(source, schema, horizon);
    // 旧图件也只读最终相及当前地层范围；不向井道返回逐采样井段。
    row.insert("intervals", QVariantList{QVariantMap{
        {"top", row.value("formation_top")}, {"bottom", row.value("formation_base")},
        {"code", row.value("facies_code")}, {"label", row.value("facies_label")}}});
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
    const auto beforeCode = layer->getFeature(fid).attribute("facies_code").toInt();
    ok = setCode(layer, fid, code, schema) && ok;
    ok = recordInterpretationEdit(id, fid, beforeCode, code, error) && ok;
    if (layer->fields().indexOf("facies_intervals") >= 0) {
      const auto feature = layer->getFeature(fid);
      QVariantMap source;
      for (int i = 0; i < feature.fields().size(); ++i)
        source.insert(feature.fields().at(i).name(), feature.attribute(i));
      source.insert("prediction_status", QStringLiteral("已修订"));
      source.insert("prediction_reason", QString());
      const auto final = FaciesCatalog::formationPrediction(source, schema, declaration(id).horizon);
      for (auto a = final.cbegin(); a != final.cend(); ++a)
        if (layer->fields().indexOf(a.key()) >= 0)
          ok = setAttribute(layer, fid, a.key(), a.value()) && ok;
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
    return reject(tr("先复制预测结果，再修订预测相"));
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
  if (index != 0)
    return reject(tr("每井每地层只有一条预测相，请选择该结果"));
  return assignFacies(id, {f.id()}, code, error);
}
