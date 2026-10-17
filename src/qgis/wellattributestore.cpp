// 层：QGIS 封装
#include "wellattributestore.h"
#include "../catalog/datacatalog.h"
#include "qgisstyleservice.h"
#include "qgislayerservice.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <memory>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgslayertree.h>
#include <qgslayertreeregistrybridge.h>
#include <qgsmaplayer.h>
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
using Table = WellAttributeStore::Table;
QString path(const QString &dir, Table table) {
  return QDir(dir).filePath(
      table == Table::Factors     ? "artifacts/layers/well-factors.gpkg"
      : table == Table::Formations ? "artifacts/layers/well-formation-facies.gpkg"
                                   : "artifacts/layers/well-log-attributes.gpkg");
}
QString uri(const QString &dir, Table table) {
  return path(dir, table) + "|layername=attributes";
}
Table tableOf(bool factors) { return factors ? Table::Factors : Table::Intervals; }
// QGIS 4 的注册表桥在图层节点被摘掉后会排队 removeMapLayers，
// 而且这条路径不看 setEnabled。只断开桥自己的两个槽，树模型仍能刷新。
bool reconnectBridgeSlot(QgsLayerTree *root, QgsLayerTreeRegistryBridge *bridge,
                         const char *signal, const char *slot) {
  const int sig = root->metaObject()->indexOfSignal(
      QMetaObject::normalizedSignature(signal));
  const int slt = bridge->metaObject()->indexOfSlot(
      QMetaObject::normalizedSignature(slot));
  return sig >= 0 && slt >= 0 && QMetaObject::connect(root, sig, bridge, slt);
}
// 岩性与地层相是属性表，不是地图图层。整份测井结果不进图层树，
// 避免按层位和相展开。图层本身留在工程里，属性表仍能打开。
void detachAttributeTableFromLegend(QgsMapLayer *layer) {
  if (!layer)
    return;
  layer->setFlags(layer->flags() | QgsMapLayer::Private);
  QgsProject *project = layer->project();
  QgsLayerTree *root = project ? project->layerTreeRoot() : nullptr;
  if (!root)
    return;
  if (root->hasCustomLayerOrder()) {
    QList<QgsMapLayer *> order = root->customLayerOrder();
    if (order.removeAll(layer) > 0)
      root->setCustomLayerOrder(order);
  }
  QgsLayerTreeLayer *node = root->findLayer(layer->id());
  if (!node)
    return;
  auto *parent = qobject_cast<QgsLayerTreeGroup *>(node->parent());
  if (!parent)
    return;
  auto *bridge = project->layerTreeRegistryBridge();
  const bool droppedWill =
      bridge && QObject::disconnect(
                    root, SIGNAL(willRemoveChildren(QgsLayerTreeNode*,int,int)),
                    bridge, nullptr);
  const bool droppedRemoved =
      bridge && QObject::disconnect(
                    root, SIGNAL(removedChildren(QgsLayerTreeNode*,int,int)),
                    bridge, nullptr);
  parent->removeChildNode(node);
  if (droppedWill &&
      !reconnectBridgeSlot(root, bridge,
                           "willRemoveChildren(QgsLayerTreeNode*,int,int)",
                           "groupWillRemoveChildren(QgsLayerTreeNode*,int,int)"))
    qWarning("WellAttributeStore: failed to restore legend removal tracking");
  if (droppedRemoved &&
      !reconnectBridgeSlot(root, bridge,
                           "removedChildren(QgsLayerTreeNode*,int,int)",
                           "groupRemovedChildren()"))
    qWarning("WellAttributeStore: failed to restore legend removal cleanup");
}
// 地层优势相表的数值列：均为非负有限数，占比/覆盖率另限 0–1。
const QStringList kFormationNumbers{"top_md", "base_md", "thickness_md",
                                    "dominant_thickness_md", "dominant_ratio",
                                    "coverage"};
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
QHash<QString, QVariantList> faciesSchemas(DataCatalog *catalog) {
  QHash<QString, QVariantList> out;
  if (!catalog || !catalog->isOpen())
    return out;
  const QString prefix = QStringLiteral("facies-schema-");
  for (const auto &a : catalog->assets()) {
    if (a.type != QLatin1String("facies_schema") || !a.displayName.startsWith(prefix))
      continue;
    const auto schema =
        catalog->currentVersion(a.id).extra.value(QStringLiteral("facies")).toList();
    if (!schema.isEmpty())
      out.insert(a.displayName.mid(prefix.size()), schema);
  }
  return out;
}
QString intervalLayerId() { return QStringLiteral("welllog.attributes"); }
QString factorLayerId() { return QStringLiteral("wellfactor.attributes"); }
QString formationLayerId() { return QStringLiteral("welllog.formations"); }
QString layerId(Table table) {
  return table == Table::Factors     ? factorLayerId()
         : table == Table::Formations ? formationLayerId()
                                      : intervalLayerId();
}
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, bool factors, bool create,
                     QString *error) {
  return open(catalog, dir, layers, tableOf(factors), create, error);
}
QgsVectorLayer *open(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, Table table, bool create,
                     QString *error) {
  if (!catalog || !catalog->isOpen() || !layers || dir.isEmpty())
    return nullptr;
  const QString id = layerId(table);
  if (!QFileInfo::exists(path(dir, table))) {
    if (!create)
      return nullptr;
    if (!writable(catalog, dir, layers, error))
      return nullptr;
    QDir().mkpath(QFileInfo(path(dir, table)).absolutePath());
    QgsVectorLayer memory("Point", "attributes", "memory");
    memory.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QList<QgsField> fields{QgsField("well_id", QMetaType::QString),
                           QgsField("well_name", QMetaType::QString),
                           QgsField(table == Table::Formations ? "formation"
                                                               : "horizon",
                                    QMetaType::QString)};
    if (table == Table::Formations) {
      fields << QgsField("unit_type", QMetaType::QString)
             << QgsField("top_md", QMetaType::Double)
             << QgsField("base_md", QMetaType::Double)
             << QgsField("thickness_md", QMetaType::Double)
             << QgsField("dominant_facies", QMetaType::QString)
             << QgsField("facies_code", QMetaType::Int)
             << QgsField("predicted_dominant", QMetaType::QString)
             << QgsField("dominant_thickness_md", QMetaType::Double)
             << QgsField("dominant_ratio", QMetaType::Double)
             << QgsField("coverage", QMetaType::Double)
             << QgsField("tie", QMetaType::Bool)
             << QgsField("facies_composition", QMetaType::QString)
             << QgsField("source", QMetaType::QString)
             << QgsField("model", QMetaType::QString)
             << QgsField("job_id", QMetaType::QString);
    } else if (table == Table::Factors) {
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
            &memory, path(dir, table),
            QgsProject::instance()->transformContext(), options,
            error) != QgsVectorFileWriter::NoError)
      return nullptr;
  }
  LayerDeclaration declaration;
  declaration.layerId = id;
  declaration.type = "vector";
  declaration.source = uri(dir, table);
  declaration.group = "00_Data";
  declaration.title = table == Table::Factors ? tr("井点单因素属性")
                       : table == Table::Formations ? tr("测井地层相属性")
                                                    : tr("测井岩性与相属性");
  if (!layers->layer(id)) {
    bool declared = false;
    bool stale = false;
    for (const auto &d : layers->declared())
      if (d.layerId == id) {
        declared = true;
        // 旧工程的声明可能带旧标题（「测井地层优势相属性」）或搬家前的
        // 绝对路径——可写时按当前口径 upsert，只读时沿用旧声明。
        stale = d.title != declaration.title || d.source != declaration.source;
      }
    if (!declared && catalog->refusesWrites()) {
      // 拆开短路：refusesWrites 时不落 error 会让调用方拿到「nullptr + 空
      // 错误」的哑失败，无从给用户解释。
      if (error)
        *error = tr("工程目录被另一实例锁定——本实例只读，无法声明井属性图层");
      return nullptr;
    }
    if (!declared && !layers->declare(declaration, error))
      return nullptr;
    if (stale && !catalog->refusesWrites())
      layers->declare(declaration);
  }
  auto *layer = qobject_cast<QgsVectorLayer *>(layers->instantiate(id, error));
  if (!layer)
    return nullptr;
  if (layer->name() != declaration.title && !catalog->refusesWrites())
    layer->setName(declaration.title);
  layer->setReadOnly(catalog->refusesWrites());
  // 旧工程在首次写入预测时补齐状态字段，保留原有属性和行。
  if (create && table != Table::Factors && !catalog->refusesWrites()) {
    QList<QgsField> missing;
    for (const auto &name : {"prediction_status", "prediction_reason"})
      if (layer->fields().indexOf(name) < 0)
        missing << QgsField(name, QMetaType::QString);
    if (!missing.isEmpty()) {
      if (layer->isEditable() || !layer->dataProvider()->addAttributes(missing)) {
        fail(error, tr("无法添加预测状态字段，请先结束属性表编辑"));
        return nullptr;
      }
      layer->updateFields();
    }
  }
  // 地图符号只服务于仍留在图层树里的井点单因素。岩性与地层相
  // 同样记一份样式，但随后退出图层树，图例不再按层位和相展开。
  if (table == Table::Formations)
    QgisStyleService::applyWellFormationLayerStyle(layer, faciesSchemas(catalog));
  else
    QgisStyleService::applyWellAttributeLayerStyle(layer, faciesSchemas(catalog));
  if (table != Table::Factors)
    detachAttributeTableFromLegend(layer);
  const QMap<QString, QString> aliases{
      {"well_id", tr("井 ID")},
      {"well_name", tr("井名")},
      {"horizon", tr("层位")},
      {"top_md", tr("顶深 MD（m）")},
      {"base_md", tr("底深 MD（m）")},
      {"lithology", tr("岩性")},
      {"facies", tr("维护相")},
      {"predicted_facies", tr("预测相")},
      {"prediction_status", tr("预测状态")},
      {"prediction_reason", tr("未预测原因")},
      {"facies_code", tr("相编码")},
      {"log_layer_thickness_md", tr("层厚 MD（m）")},
      {"log_sand_thickness_md", tr("砂厚 MD（m）")},
      {"sand_ratio", tr("砂地比（0–1）")},
      {"porosity", tr("孔隙度")},
      {"permeability", tr("渗透率")},
      {"formation", tr("地层")},
      {"unit_type", tr("层级（组/段）")},
      {"thickness_md", tr("地层厚度 MD（m）")},
      // 地层表的「相」就是该地层的预测输出（厚度最大者）——列名直接叫
      // 地层相，不再出现「优势」字样（2026-10-10 用户要求）。
      {"dominant_facies", tr("地层相")},
      {"predicted_dominant", tr("预测相")},
      {"dominant_thickness_md", tr("相厚度（m）")},
      {"dominant_ratio", tr("相厚度占比（0–1）")},
      {"coverage", tr("预测覆盖率（0–1）")},
      {"tie", tr("厚度并列")},
      {"facies_composition", tr("相厚度构成")},
      {"source", tr("来源")}};
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
        [layer, owner, table] {
          QString error;
          const bool ok = owner && !owner->refusesWrites() &&
                          validate(layer, owner, table, &error);
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
  return rows(dir, layers, tableOf(factors), wellId, horizon);
}
QVariantList rows(const QString &dir, QgisLayerService *layers, Table table,
                  const QString &wellId, const QString &key) {
  QVariantList out;
  std::unique_ptr<QgsVectorLayer> owned;
  auto *layer = layers ? qobject_cast<QgsVectorLayer *>(
                             layers->layer(layerId(table)))
                       : nullptr;
  if (!layer && QFileInfo::exists(path(dir, table))) {
    owned = std::make_unique<QgsVectorLayer>(uri(dir, table), "attributes",
                                             "ogr");
    layer = owned.get();
  }
  if (!layer || !layer->isValid())
    return out;
  const QString keyField = table == Table::Formations ? "formation" : "horizon";
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    if ((wellId.isEmpty() || f.attribute("well_id").toString() == wellId) &&
        (key.isEmpty() || f.attribute(keyField).toString() == key))
      out << attributes(f);
  return out;
}
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, bool factors,
              QString *error) {
  return validate(layer, catalog, tableOf(factors), error);
}
bool validate(QgsVectorLayer *layer, DataCatalog *catalog, Table table,
              QString *error) {
  const bool factors = table == Table::Factors;
  QSet<QString> keys;
  QMap<QString, QVector<QVariantMap>> byWell;
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f)) {
    const auto row = attributes(f);
    const auto id = row.value("well_id").toString();
    if (!catalog || catalog->entityById(id).entityType != "well")
      return fail(error, tr("井属性含未解析的井 ID：%1").arg(id));
    if (table == Table::Formations) {
      const auto formation = row.value("formation").toString().trimmed();
      const auto key = id + QChar(0x1f) + formation;
      if (formation.isEmpty() || keys.contains(key))
        return fail(error, tr("地层相必须按井/地层唯一，地层名不能为空"));
      keys.insert(key);
      if (!span(row))
        return fail(error, tr("地层顶底深必须有限且底深大于顶深"));
      if (!faciesCode(row.value("facies_code")))
        return fail(error, tr("相编码必须为 1–32767 的整数或空值"));
      for (const auto &name : kFormationNumbers) {
        const auto value = row.value(name);
        if (value.isNull())
          continue;
        bool ok = false;
        const double n = value.toDouble(&ok);
        if (!ok || !std::isfinite(n) || n < 0 ||
            ((name == QLatin1String("dominant_ratio") ||
              name == QLatin1String("coverage")) &&
             n > 1 + 1e-9))
          return fail(error,
                      tr("地层相的厚度必须为非负有限数值，占比和覆盖率为 0–1"));
      }
    } else if (!factors) {
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
      if (b) {
        const bool hasExistingFacies = (row.value("source").toString() == "manual") ||
                                       (!row.value("facies_code").isNull() && row.value("facies_code").toInt() > 0);
        for (auto p = b->cbegin(); p != b->cend(); ++p) {
          const QString &k = p.key();
          const bool isPredictionMeta = (k == "predicted_facies" || k == "model" ||
                                         k == "job_id" || k == "prediction_status" ||
                                         k == "prediction_reason");
          const bool isFaciesAttribute = (k == "facies" || k == "sub_facies" ||
                                          k == "micro_facies" || k == "facies_code" ||
                                          k == "facies_pattern");
          if (!onlyMissing || isPredictionMeta || (!hasExistingFacies && isFaciesAttribute) || row.value(k).isNull() ||
              (row.value(k).typeId() == QMetaType::QString && row.value(k).toString().trimmed().isEmpty()))
            row.insert(k, p.value());
        }
      }
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
bool mergeFormations(DataCatalog *catalog, const QString &dir,
                     QgisLayerService *layers, const QVariantList &updates,
                     QString *error, bool onlyMissing) {
  if (updates.isEmpty())
    return true;
  if (!writable(catalog, dir, layers, error))
    return false;
  const auto keyOf = [](const QVariantMap &row) {
    return row.value("well_id").toString() + QChar(0x1f) +
           row.value("formation").toString().trimmed();
  };
  QSet<QString> batch;
  for (const auto &value : updates) {
    const auto row = value.toMap();
    if (row.value("formation").toString().trimmed().isEmpty() ||
        !span(row) || catalog->entityById(row.value("well_id").toString())
                              .entityType != "well")
      return fail(error, tr("地层相的井 ID、地层名或顶底深无效"));
    if (!faciesCode(row.value("facies_code")))
      return fail(error, tr("相编码必须为 1–32767 的整数或空值"));
    if (batch.contains(keyOf(row)))
      return fail(error, tr("同一口井的地层名重复，未更新地层相"));
    batch.insert(keyOf(row));
  }
  auto *layer = open(catalog, dir, layers, Table::Formations, true, error);
  if (!layer)
    return false;
  if (layer->isEditable())
    return fail(error,
                tr("测井地层相属性表正在编辑，请先保存或放弃编辑，再运行预测"));
  if (!validate(layer, catalog, Table::Formations, error))
    return false;
  QMap<QString, QgsFeatureId> existing;
  auto it = layer->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f))
    existing.insert(f.attribute("well_id").toString() + QChar(0x1f) +
                        f.attribute("formation").toString().trimmed(),
                    f.id());
  if (!layer->startEditing())
    return fail(error, tr("无法开始地层相更新"));
  layer->beginEditCommand(tr("更新测井地层相"));
  bool ok = true;
  for (const auto &value : updates) {
    const auto row = value.toMap();
    const auto found = existing.constFind(keyOf(row));
    if (found == existing.cend()) {
      auto next = featureFor(layer, catalog, row);
      ok = layer->addFeature(next) && ok;
      continue;
    }
    const auto existingFeat = layer->getFeature(found.value());
    const QString existingSource = existingFeat.attribute("source").toString();
    const auto existingCode = existingFeat.attribute("facies_code");
    const bool hasExistingFacies = (existingSource == "manual") ||
                                   (existingCode.isValid() && !existingCode.isNull() && existingCode.toInt() > 0);
    for (auto p = row.cbegin(); p != row.cend(); ++p) {
      const int index = layer->fields().indexOf(p.key());
      if (index >= 0 && p.key() != "well_id" && p.key() != "formation" &&
          p.key() != "well_name") {
        const QString &k = p.key();
        const bool isPredictionMeta = (k == "predicted_dominant" || k == "model" ||
                                       k == "job_id" || k == "prediction_status" ||
                                       k == "prediction_reason" || k == "dominant_thickness_md" ||
                                       k == "dominant_ratio" || k == "coverage" ||
                                       k == "tie" || k == "facies_composition");
        if (onlyMissing && !isPredictionMeta) {
          const bool isFaciesAttribute = (k == "dominant_facies" || k == "facies_code");
          if (hasExistingFacies && isFaciesAttribute)
            continue;
          if (!isFaciesAttribute) {
            const auto currentVal = existingFeat.attribute(index);
            if (currentVal.isValid() && !currentVal.isNull() &&
                !(currentVal.typeId() == QMetaType::QString && currentVal.toString().trimmed().isEmpty())) {
              continue;
            }
          }
        }
        ok = layer->changeAttributeValue(found.value(), index, p.value()) && ok;
      }
    }
  }
  if (!ok || !validate(layer, catalog, Table::Formations, error)) {
    layer->destroyEditCommand();
    layer->rollBack();
    return fail(error, error && !error->isEmpty()
                           ? *error
                           : tr("地层相更新失败，已撤销本次更新"));
  }
  layer->endEditCommand();
  return commit(layer, error);
}
bool mergeIntervalsAndFormations(DataCatalog *catalog, const QString &dir,
                                 QgisLayerService *layers,
                                 const QVariantList &intervalUpdates,
                                 const QVariantList &formationUpdates,
                                 QString *error, bool onlyMissing) {
  // #415：两表分属两个 edit buffer——地层批失败时井段批已提交，井间隔表
  // 被改写而地层表未动即半更新（重试会重复合并）。与 seed() 同口径：先
  // 快照受影响井的旧行，第二批失败删回本批再重加快照，等价单事务。
  QSet<QString> wellIds;
  for (const auto &v : intervalUpdates)
    wellIds.insert(v.toMap().value("well_id").toString());
  QString openErr;
  auto *layer = wellIds.isEmpty()
                    ? nullptr
                    : open(catalog, dir, layers, Table::Intervals, true, &openErr);
  QVariantList previous;
  if (layer && layer->isValid()) {
    auto it = layer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
      if (wellIds.contains(f.attribute("well_id").toString()))
        previous << attributes(f);
  }
  if (!mergeIntervals(catalog, dir, layers, intervalUpdates, error, onlyMissing))
    return false;
  if (mergeFormations(catalog, dir, layers, formationUpdates, error, onlyMissing))
    return true;
  const QString firstErr = error ? *error : QString();
  if (layer && layer->isValid() && !layer->isEditable() &&
      layer->startEditing()) {
    QgsFeatureIds doomed;
    auto it = layer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
      if (wellIds.contains(f.attribute("well_id").toString()))
        doomed.insert(f.id());
    bool ok = doomed.isEmpty() || layer->deleteFeatures(doomed);
    if (ok)
      for (const auto &v : previous) {
        auto next = featureFor(layer, catalog, v.toMap());
        ok = layer->addFeature(next) && ok;
      }
    if (ok)
      layer->commitChanges();
    else
      layer->rollBack();
  }
  return fail(error,
              firstErr.isEmpty() ? tr("地层相更新失败，井段已恢复") : firstErr);
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
  // #415：岩性、相两批 merge 各自提交——先备好两批并快照该井旧行，第二批
  // 失败时回滚第一批已提交的井段，seed 不停在半更新态。
  QString openErr;
  auto *layer = open(catalog, dir, layers, Table::Intervals, true, &openErr);
  QVariantList previous;
  if (layer && layer->isValid()) {
    auto it = layer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
      if (f.attribute("well_id").toString() == wellId)
        previous << attributes(f);
  }
  QVariantList faciesRows;
  for (const auto &iv : data.faciesIntervals)
    faciesRows << QVariantMap{{"well_id", wellId},
                              {"top_md", iv.topDepth},
                              {"base_md", iv.bottomDepth},
                              {"facies", iv.majorFacies},
                              {"sub_facies", iv.subFacies},
                              {"micro_facies", iv.microFacies},
                              {"facies_pattern", iv.patternType}};
  if (!mergeIntervals(catalog, dir, layers, intervals, error, true))
    return false;
  if (mergeIntervals(catalog, dir, layers, faciesRows, error, true))
    return true;
  if (layer && layer->isValid() && !layer->isEditable() &&
      layer->startEditing()) {
    QgsFeatureIds doomed;
    auto it = layer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
      if (f.attribute("well_id").toString() == wellId)
        doomed.insert(f.id());
    bool ok = doomed.isEmpty() || layer->deleteFeatures(doomed);
    if (ok)
      for (const auto &v : previous) {
        auto next = featureFor(layer, catalog, v.toMap());
        ok = layer->addFeature(next) && ok;
      }
    if (ok)
      layer->commitChanges();
    else
      layer->rollBack();
  }
  return false;
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
