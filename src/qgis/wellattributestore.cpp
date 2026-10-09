// 层：QGIS 封装
#include "wellattributestore.h"
#include "../catalog/datacatalog.h"
#include "qgislayerservice.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <memory>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

namespace {
QString tr(const char *s) {
  return QCoreApplication::translate("WellAttributeStore", s);
}
bool fail(QString *error, const QString &reason) {
  if (error)
    *error = reason;
  return false;
}
QString path(const QString &dir, bool factors) {
  return QDir(dir).filePath(factors
                                ? "artifacts/layers/well-factors.gpkg"
                                : "artifacts/layers/well-log-attributes.gpkg");
}
QString uri(const QString &dir, bool factors) {
  return path(dir, factors) + "|layername=attributes";
}
QVariantMap attributes(const QgsFeature &feature) {
  QVariantMap row;
  for (int i = 0; i < feature.fields().size(); ++i)
    if (feature.fields().at(i).name() != QLatin1String("fid"))
      row.insert(feature.fields().at(i).name(), feature.attribute(i));
  return row;
}
bool span(const QVariantMap &row) {
  bool a = false, b = false;
  const double top = row.value("top_md").toDouble(&a),
               base = row.value("base_md").toDouble(&b);
  return a && b && std::isfinite(top) && std::isfinite(base) && base > top;
}
bool faciesCode(const QVariant &code) {
  if (code.isNull())
    return true;
  bool ok = false;
  const double n = code.toDouble(&ok);
  return ok && std::isfinite(n) && n >= 1 && n <= 32767 && n == std::round(n);
}
QgsFeature featureFor(QgsVectorLayer *layer, DataCatalog *catalog,
                      const QVariantMap &row) {
  QgsFeature f(layer->fields());
  for (auto it = row.cbegin(); it != row.cend(); ++it) {
    const int index = layer->fields().indexOf(it.key());
    if (index >= 0)
      f.setAttribute(index, it.value());
  }
  const auto well = catalog->entityById(row.value("well_id").toString());
  if (well.hasSurface &&
      (well.coordinateStatus == "ok" ||
       well.coordinateStatus == "untransformed") &&
      std::isfinite(well.surfaceX) && std::isfinite(well.surfaceY))
    f.setGeometry(
        QgsGeometry::fromPointXY(QgsPointXY(well.surfaceX, well.surfaceY)));
  f.setAttribute("well_name", well.name);
  return f;
}
bool writable(DataCatalog *catalog, const QString &dir,
              QgisLayerService *layers, QString *error) {
  return catalog && catalog->isOpen() && !catalog->refusesWrites() &&
                 !dir.isEmpty() && layers
             ? true
             : fail(error, tr("井属性维护需要可写工程和图层服务"));
}
bool commit(QgsVectorLayer *layer, QString *error) {
  if (!layer->commitChanges()) {
    const auto reason = layer->commitErrors().join("; ");
    layer->rollBack();
    return fail(error, tr("井属性保存失败：%1").arg(reason));
  }
  layer->triggerRepaint();
  return true;
}
const QVariantMap *at(const QVector<QVariantMap> &rows, double depth) {
  for (const auto &row : rows)
    if (depth >= row.value("top_md").toDouble() &&
        depth < row.value("base_md").toDouble())
      return &row;
  return nullptr;
}
} // namespace

namespace WellAttributeStore {
QString intervalLayerId() { return QStringLiteral("welllog.attributes"); }
QString factorLayerId() { return QStringLiteral("wellfactor.attributes"); }
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, bool factors, bool create,
                     QString *error) {
  if (!catalog || !catalog->isOpen() || !layers || dir.isEmpty())
    return nullptr;
  const QString id = factors ? factorLayerId() : intervalLayerId();
  if (!QFileInfo::exists(path(dir, factors))) {
    if (!create)
      return nullptr;
    if (!writable(catalog, dir, layers, error))
      return nullptr;
    QDir().mkpath(QFileInfo(path(dir, factors)).absolutePath());
    QgsVectorLayer memory("Point", "attributes", "memory");
    memory.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QList<QgsField> fields{QgsField("well_id", QMetaType::QString),
                           QgsField("well_name", QMetaType::QString),
                           QgsField("horizon", QMetaType::QString)};
    if (factors) {
      for (const auto &name :
           {"log_layer_thickness_md", "log_sand_thickness_md", "sand_ratio",
            "porosity", "permeability"})
        fields << QgsField(name, QMetaType::Double);
    } else {
      fields << QgsField("top_md", QMetaType::Double)
             << QgsField("base_md", QMetaType::Double)
             << QgsField("lithology", QMetaType::QString)
             << QgsField("lithology_pattern", QMetaType::QString)
             << QgsField("facies", QMetaType::QString)
             << QgsField("sub_facies", QMetaType::QString)
             << QgsField("micro_facies", QMetaType::QString)
             << QgsField("facies_pattern", QMetaType::QString)
             << QgsField("predicted_facies", QMetaType::QString)
             << QgsField("facies_code", QMetaType::Int)
             << QgsField("model", QMetaType::QString)
             << QgsField("job_id", QMetaType::QString);
    }
    memory.dataProvider()->addAttributes(fields);
    memory.updateFields();
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "GPKG";
    options.layerName = "attributes";
    if (QgsVectorFileWriter::writeAsVectorFormatV3(
            &memory, path(dir, factors),
            QgsProject::instance()->transformContext(), options,
            error) != QgsVectorFileWriter::NoError)
      return nullptr;
  }
  if (!layers->layer(id)) {
    LayerDeclaration declaration;
    declaration.layerId = id;
    declaration.type = "vector";
    declaration.source = uri(dir, factors);
    declaration.group = "00_Data";
    declaration.title = factors ? tr("井点单因素属性") : tr("测井岩性与相属性");
    bool declared = false;
    for (const auto &d : layers->declared())
      declared = declared || d.layerId == id;
    if (!declared &&
        (catalog->refusesWrites() || !layers->declare(declaration, error)))
      return nullptr;
  }
  auto *layer = qobject_cast<QgsVectorLayer *>(layers->instantiate(id, error));
  if (!layer)
    return nullptr;
  layer->setReadOnly(catalog->refusesWrites());
  const QMap<QString, QString> aliases{
      {"well_id", tr("井 ID")},
      {"well_name", tr("井名")},
      {"horizon", tr("层位")},
      {"top_md", tr("顶深 MD（m）")},
      {"base_md", tr("底深 MD（m）")},
      {"lithology", tr("岩性")},
      {"facies", tr("维护相")},
      {"predicted_facies", tr("预测相")},
      {"facies_code", tr("相编码")},
      {"log_layer_thickness_md", tr("层厚 MD（m）")},
      {"log_sand_thickness_md", tr("砂厚 MD（m）")},
      {"sand_ratio", tr("砂地比（0–1）")},
      {"porosity", tr("孔隙度")},
      {"permeability", tr("渗透率")}};
  for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
    const int index = layer->fields().indexOf(it.key());
    if (index >= 0)
      layer->setFieldAlias(index, it.value());
  }
  if (!layer->property("wellAttributeValidation").toBool()) {
    layer->setProperty("wellAttributeValidation", true);
    const QPointer<DataCatalog> owner(catalog);
    QObject::connect(
        layer, &QgsVectorLayer::beforeCommitChanges, layer,
        [layer, owner, factors] {
          QString error;
          const bool ok = owner && !owner->refusesWrites() &&
                          validate(layer, owner, factors, &error);
          layer->setCustomProperty("paleo/wellAttributeValidationError", error);
          layer->setAllowCommit(ok);
          if (!ok)
            layer->raiseError(error.isEmpty() ? tr("工程不可写") : error);
        });
  }
  return layer;
}
QVariantList rows(const QString &dir, QgisLayerService *layers, bool factors,
                  const QString &wellId, const QString &horizon) {
  QVariantList out;
  std::unique_ptr<QgsVectorLayer> owned;
  auto *layer = layers ? qobject_cast<QgsVectorLayer *>(layers->layer(
                             factors ? factorLayerId() : intervalLayerId()))
                       : nullptr;
  if (!layer && QFileInfo::exists(path(dir, factors))) {
    owned = std::make_unique<QgsVectorLayer>(uri(dir, factors), "attributes",
                                             "ogr");
    layer = owned.get();
  }
  if (!layer || !layer->isValid())
    return out;
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    if ((wellId.isEmpty() || f.attribute("well_id").toString() == wellId) &&
        (horizon.isEmpty() || f.attribute("horizon").toString() == horizon))
      out << attributes(f);
  return out;
}
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, bool factors,
              QString *error) {
  QSet<QString> keys;
  QMap<QString, QVector<QVariantMap>> byWell;
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f)) {
    const auto row = attributes(f);
    const auto id = row.value("well_id").toString();
    if (!catalog || catalog->entityById(id).entityType != "well")
      return fail(error, tr("井属性含未解析的井 ID：%1").arg(id));
    if (!factors) {
      if (!span(row))
        return fail(error, tr("井段顶底深必须有限且底深大于顶深"));
      if (!faciesCode(row.value("facies_code")))
        return fail(error, tr("相编码必须为 1–32767 的整数或空值"));
      byWell[id] << row;
    } else {
      const auto key = id + QChar(0x1f) + row.value("horizon").toString();
      if (keys.contains(key) || row.value("horizon").toString().isEmpty())
        return fail(error, tr("井点因子必须按井/层位唯一，层位不能为空"));
      keys.insert(key);
      for (const auto &name :
           {"log_layer_thickness_md", "log_sand_thickness_md", "sand_ratio",
            "porosity", "permeability"}) {
        const auto value = row.value(name);
        if (!value.isNull()) {
          bool ok = false;
          const double n = value.toDouble(&ok);
          if (!ok || !std::isfinite(n) || n < 0 ||
              (QString(name) == "sand_ratio" && n > 1))
            return fail(error,
                        tr("单因素属性必须为非负有限数值，砂地比为 0–1"));
        }
      }
    }
  }
  for (auto &segments : byWell) {
    std::sort(
        segments.begin(), segments.end(), [](const auto &a, const auto &b) {
          return a.value("top_md").toDouble() < b.value("top_md").toDouble();
        });
    for (int i = 1; i < segments.size(); ++i)
      if (segments[i].value("top_md").toDouble() <
          segments[i - 1].value("base_md").toDouble() - 1e-6)
        return fail(error, tr("同一口井的属性井段不能重叠"));
  }
  return true;
}
bool mergeIntervals(DataCatalog *catalog, const QString &dir,
                    QgisLayerService *layers, const QVariantList &updates,
                    QString *error, bool onlyMissing) {
  if (!writable(catalog, dir, layers, error))
    return false;
  QMap<QString, QVector<QVariantMap>> incoming;
  for (const auto &value : updates) {
    const auto row = value.toMap();
    const auto id = row.value("well_id").toString();
    if (!span(row) || catalog->entityById(id).entityType != "well")
      return fail(error, tr("预测井段无效或井 ID 未唯一解析"));
    if (!faciesCode(row.value("facies_code")))
      return fail(error, tr("相编码必须为 1–32767 的整数或空值"));
    incoming[id] << row;
  }
  if (incoming.isEmpty())
    return true;
  for (auto &segments : incoming) {
    std::sort(
        segments.begin(), segments.end(), [](const auto &a, const auto &b) {
          return a.value("top_md").toDouble() < b.value("top_md").toDouble();
        });
    for (int i = 1; i < segments.size(); ++i)
      if (segments[i].value("top_md").toDouble() <
          segments[i - 1].value("base_md").toDouble() - 1e-6)
        return fail(error, tr("输入井段重叠，未更新属性"));
  }
  auto *layer = open(catalog, dir, layers, false, true, error);
  if (!layer)
    return false;
  if (layer->isEditable())
    return fail(error,
                tr("测井属性表正在编辑，请先保存或放弃编辑，再运行预测"));
  if (!validate(layer, catalog, false, error))
    return false;
  QMap<QString, QVector<QVariantMap>> existing;
  QgsFeatureIds remove;
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    if (incoming.contains(f.attribute("well_id").toString())) {
      existing[f.attribute("well_id").toString()] << attributes(f);
      remove.insert(f.id());
    }
  if (onlyMissing) {
    bool missing = false;
    for (auto group = incoming.cbegin(); group != incoming.cend(); ++group) {
      const auto old = existing.value(group.key());
      for (const auto &next : group.value()) {
        const double top = next.value("top_md").toDouble();
        const double base = next.value("base_md").toDouble();
        QVector<double> boundaries{top, base};
        for (const auto &row : old)
          for (const auto &key : {"top_md", "base_md"}) {
            const double d = row.value(key).toDouble();
            if (d > top && d < base)
              boundaries << d;
          }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                         boundaries.end());
        for (int i = 1; i < boundaries.size(); ++i) {
          const auto *previous = at(
              old, boundaries[i - 1] + (boundaries[i] - boundaries[i - 1]) / 2);
          if (!previous) {
            missing = true;
            break;
          }
          for (auto p = next.cbegin(); p != next.cend(); ++p)
            if (p.key() != "top_md" && p.key() != "base_md" &&
                p.key() != "well_id" && !p.value().isNull() &&
                previous->value(p.key()).isNull())
              missing = true;
        }
      }
    }
    if (!missing)
      return true; // 打开/重绘井文件不重复提交，更不打断其它井的在途预测。
  }
  QgsFeatureList add;
  for (auto group = incoming.cbegin(); group != incoming.cend(); ++group) {
    const auto old = existing.value(group.key());
    QVector<double> boundaries;
    for (const auto &set : {old, group.value()})
      for (const auto &row : set)
        boundaries << row.value("top_md").toDouble()
                   << row.value("base_md").toDouble();
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());
    for (int i = 1; i < boundaries.size(); ++i) {
      const double depth =
          boundaries[i - 1] + (boundaries[i] - boundaries[i - 1]) / 2;
      const auto *a = at(old, depth), *b = at(group.value(), depth);
      if (!a && !b)
        continue;
      QVariantMap row = a ? *a : QVariantMap();
      if (b)
        for (auto p = b->cbegin(); p != b->cend(); ++p)
          if (!onlyMissing || row.value(p.key()).isNull())
            row.insert(p.key(), p.value());
      row.insert("well_id", group.key());
      row.insert("top_md", boundaries[i - 1]);
      row.insert("base_md", boundaries[i]);
      add << featureFor(layer, catalog, row);
    }
  }
  if (!layer->startEditing())
    return fail(error, tr("无法开始井属性更新"));
  layer->beginEditCommand(tr("更新测井属性"));
  if (!layer->deleteFeatures(remove) || !layer->addFeatures(add) ||
      !validate(layer, catalog, false, error)) {
    layer->destroyEditCommand();
    layer->rollBack();
    return fail(error, tr("井属性更新失败，已撤销本次更新"));
  }
  layer->endEditCommand();
  return commit(layer, error);
}
bool seedFactors(DataCatalog *catalog, const QString &dir,
                 QgisLayerService *layers, const QVariantList &updates,
                 QString *error) {
  if (updates.isEmpty())
    return true;
  if (!writable(catalog, dir, layers, error))
    return false;
  auto *layer = open(catalog, dir, layers, true, true, error);
  if (!layer)
    return false;
  if (layer->isEditable())
    return fail(error, tr("井点因子表正在编辑，请先保存或放弃编辑"));
  if (!layer->startEditing())
    return fail(error, tr("无法开始井点因子更新"));
  for (const auto &v : updates) {
    const auto row = v.toMap();
    QgsFeature found;
    auto it = layer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
      if (f.attribute("well_id") == row.value("well_id") &&
          f.attribute("horizon") == row.value("horizon")) {
        found = f;
        break;
      }
    if (found.isValid())
      continue; // 已维护行（含显式空值）不被自动重算覆盖。
    auto next = featureFor(layer, catalog, row);
    if (!layer->addFeature(next)) {
      layer->rollBack();
      return fail(error, tr("井点因子新增失败"));
    }
  }
  if (!validate(layer, catalog, true, error)) {
    layer->rollBack();
    return false;
  }
  return commit(layer, error);
}
bool seed(DataCatalog *catalog, const QString &dir, QgisLayerService *layers,
          const QString &wellId,
          const WellComposite::ComprehensiveWellData &data, QString *error) {
  auto *active =
      layers ? qobject_cast<QgsVectorLayer *>(layers->layer(intervalLayerId()))
             : nullptr;
  if (active && active->isEditable())
    return true;
  QVariantList intervals;
  for (const auto &iv : data.lithologyIntervals)
    intervals << QVariantMap{{"well_id", wellId},
                             {"top_md", iv.topDepth},
                             {"base_md", iv.bottomDepth},
                             {"lithology", iv.lithoName},
                             {"lithology_pattern", iv.patternType}};
  if (!mergeIntervals(catalog, dir, layers, intervals, error, true))
    return false;
  intervals.clear();
  for (const auto &iv : data.faciesIntervals)
    intervals << QVariantMap{{"well_id", wellId},
                             {"top_md", iv.topDepth},
                             {"base_md", iv.bottomDepth},
                             {"facies", iv.majorFacies},
                             {"sub_facies", iv.subFacies},
                             {"micro_facies", iv.microFacies},
                             {"facies_pattern", iv.patternType}};
  return mergeIntervals(catalog, dir, layers, intervals, error, true);
}
void apply(const QVariantList &intervals,
           WellComposite::ComprehensiveWellData *data) {
  if (!data || intervals.isEmpty())
    return;
  QVector<WellComposite::LithologyInterval> lithology;
  QVector<WellComposite::FaciesInterval> facies;
  bool hasLithology = false, hasFacies = false;
  for (const auto &v : intervals) {
    const auto row = v.toMap();
    if (!span(row))
      continue;
    hasLithology = hasLithology || !row.value("lithology").isNull();
    hasFacies = hasFacies || !row.value("facies").isNull();
    if (!row.value("lithology").toString().isEmpty()) {
      WellComposite::LithologyInterval iv;
      iv.topDepth = row.value("top_md").toDouble();
      iv.bottomDepth = row.value("base_md").toDouble();
      iv.lithoName = row.value("lithology").toString();
      iv.patternType = row.value("lithology_pattern").toString();
      lithology << iv;
    }
    if (!row.value("facies").toString().isEmpty()) {
      WellComposite::FaciesInterval iv;
      iv.topDepth = row.value("top_md").toDouble();
      iv.bottomDepth = row.value("base_md").toDouble();
      iv.majorFacies = row.value("facies").toString();
      iv.subFacies = row.value("sub_facies").toString();
      iv.microFacies = row.value("micro_facies").toString();
      iv.patternType = row.value("facies_pattern").toString();
      facies << iv;
    }
  }
  const auto less = [](const auto &a, const auto &b) {
    return a.topDepth < b.topDepth;
  };
  std::sort(lithology.begin(), lithology.end(), less);
  std::sort(facies.begin(), facies.end(), less);
  if (hasLithology)
    data->lithologyIntervals = lithology;
  if (hasFacies)
    data->faciesIntervals = facies;
}
} // namespace WellAttributeStore
