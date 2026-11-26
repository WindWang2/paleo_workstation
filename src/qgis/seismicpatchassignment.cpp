// 层：QGIS 封装
#include "seismicclusterpatches.h"
#include "../domain/faciescatalog.h"
#include <QFile>
#include <QHash>
#include <QObject>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <memory>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

namespace {
bool fail(QString *error, const QString &reason) {
  if (error) *error = reason;
  return false;
}
struct CloseDataset {
  void operator()(GDALDataset *ds) const { GDALClose(ds); }
};
using Dataset = std::unique_ptr<GDALDataset, CloseDataset>;
Dataset openPatches(const QString &path, OGRLayer *&layer, QString *error) {
  GDALAllRegister();
  Dataset ds(GDALDataset::Open(path.toUtf8().constData(), GDAL_OF_VECTOR));
  layer = ds ? ds->GetLayerByName("cluster_patches") : nullptr;
  if (!layer && ds) layer = ds->GetLayerByName("features"); // Published geometry-edit snapshots.
  if (layer && layer->FindFieldIndex("patch_id", TRUE) >= 0 &&
      layer->FindFieldIndex("cluster_id", TRUE) >= 0 && layer->FindFieldIndex("pixel_count", TRUE) >= 0)
    return ds;
  fail(error, QObject::tr("无法读取原始聚类面的编号或像元数"));
  return {};
}
QVariant field(OGRFeature *f, const char *name) {
  const int index = f->GetFieldIndex(name);
  if (index < 0 || !f->IsFieldSetAndNotNull(index)) return {};
  if (f->GetFieldDefnRef(index)->GetType() == OFTString)
    return QString::fromUtf8(f->GetFieldAsString(index));
  return qint64(f->GetFieldAsInteger64(index));
}
bool ensureFields(OGRLayer *layer) {
  for (const auto *key : {"facies_code","well_suggestion","cluster_wells","patch_wells"}) {
    OGRFieldDefn def(key, OFTInteger);
    if (layer->FindFieldIndex(key, TRUE) < 0 && layer->CreateField(&def) != OGRERR_NONE) return false;
  }
  for (const auto *key : {"assignment_status","facies_name","subfacies","microfacies","facies_label","texture"}) {
    OGRFieldDefn def(key, OFTString);
    if (layer->FindFieldIndex(key, TRUE) < 0 && layer->CreateField(&def) != OGRERR_NONE) return false;
  }
  return true;
}
void setCode(OGRFeature &f, const QVariantList &facies, const QVariant &code) {
  if (code.toInt() > 0) f.SetField("facies_code", code.toInt());
  else f.SetFieldNull(f.GetFieldIndex("facies_code"));
  const auto values = FaciesCatalog::attributes(facies, code);
  for (auto it = values.cbegin(); it != values.cend(); ++it)
    f.SetField(it.key().toUtf8().constData(), it.value().toString().toUtf8().constData());
}
using Edit = std::function<bool(OGRFeature &, QString *)>;
bool writeSnapshot(const QString &sourcePath, const QString &outputPath, const Edit &edit,
                   const std::function<bool()> &cancel, QString *error) {
  OGRLayer *source = nullptr;
  const auto input = openPatches(sourcePath, source, error);
  if (!input) return false;
  auto *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!driver || QFile::exists(outputPath)) return fail(error, QObject::tr("聚类赋相输出路径不可用"));
  bool saved = false;
  const auto cleanup = qScopeGuard([&] { if (!saved) QFile::remove(outputPath); });
  Dataset out(driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr));
  if (!out) return fail(error, QObject::tr("无法创建聚类赋相文件"));
  auto *layer = out->CreateLayer("cluster_patches", source->GetSpatialRef(), source->GetGeomType());
  if (!layer) return fail(error, QObject::tr("无法创建聚类赋相面"));
  const auto *schema = source->GetLayerDefn();
  for (int i = 0; i < schema->GetFieldCount(); ++i) {
    OGRFieldDefn def(schema->GetFieldDefn(i));
    if (layer->CreateField(&def) != OGRERR_NONE) return fail(error, QObject::tr("无法复制聚类面字段"));
  }
  if (!ensureFields(layer) || layer->StartTransaction() != OGRERR_NONE)
    return fail(error, QObject::tr("无法准备赋相属性"));
  for (auto &&original : *source) {
    if (cancel && cancel()) return fail(error, QObject::tr("已取消"));
    OGRFeature feature(layer->GetLayerDefn());
    if (feature.SetFrom(original.get()) != OGRERR_NONE) return fail(error, QObject::tr("无法复制聚类图斑"));
    feature.SetFID(original->GetFID());
    if (!edit(feature, error)) return false;
    if (layer->CreateFeature(&feature) != OGRERR_NONE) return fail(error, QObject::tr("无法写入聚类图斑"));
  }
  if (layer->CommitTransaction() != OGRERR_NONE || out->FlushCache() != CE_None)
    return fail(error, QObject::tr("保存聚类赋相失败"));
  saved = true;
  return true;
}
}

QVariantList SeismicClusterPatches::choices(const QString &sourcePath, QString *error, qint64 firstFid, int limit) {
  OGRLayer *layer = nullptr;
  const auto ds = openPatches(sourcePath, layer, error);
  QVariantList rows;
  if (!ds) return rows;
  const auto fid = QString::fromUtf8(layer->GetFIDColumn());
  if (fid.isEmpty()) { fail(error, QObject::tr("聚类面缺少要素编号")); return {}; }
  const auto quotedFid = QStringLiteral("\"%1\"").arg(QString(fid).replace('"', "\"\""));
  const QStringList properties{"patch_id", "cluster_id", "pixel_count", "facies_code", "well_suggestion",
                               "cluster_wells", "patch_wells", "assignment_status"};
  QStringList columns{QStringLiteral("CAST(%1 AS INTEGER) AS feature_id").arg(quotedFid)};
  for (const auto &name : properties)
    if (layer->FindFieldIndex(name.toUtf8().constData(), TRUE) >= 0) columns << name;
  // Read attributes by primary key without transferring polygon geometry or skipping earlier pages.
  const QString sql = QStringLiteral("SELECT %1 FROM \"%2\" WHERE %3 >= %4 ORDER BY %3 LIMIT %5")
      .arg(columns.join(','), QString::fromUtf8(layer->GetName()), quotedFid)
      .arg(firstFid).arg(std::clamp(limit, 1, 129));
  auto *page = ds->ExecuteSQL(sql.toUtf8().constData(), nullptr, nullptr);
  if (!page) { fail(error, QObject::tr("无法读取聚类面编号")); return {}; }
  const auto release = qScopeGuard([&]{ ds->ReleaseResultSet(page); });
  for (auto &&f : *page) {
    QVariantMap row{{"feature_id", field(f.get(), "feature_id")}};
    for (const auto &name : properties) row.insert(name, field(f.get(), name.toUtf8().constData()));
    rows << row;
  }
  return rows;
}

qint64 SeismicClusterPatches::clusterSelection(const QString &sourcePath, int cluster,
                                              QList<qint64> &highlight, QString *error) {
  highlight.clear();
  OGRLayer *layer = nullptr;
  const auto ds = openPatches(sourcePath, layer, error);
  if (!ds) return -1;
  const auto filter = QStringLiteral("cluster_id = %1").arg(cluster).toUtf8();
  if (layer->SetAttributeFilter(filter.constData()) != OGRERR_NONE) {
    fail(error, QObject::tr("无法定位所选聚类面")); return -1;
  }
  const qint64 total = layer->GetFeatureCount();
  const char *ignore[]{"OGR_GEOMETRY", nullptr};
  layer->SetIgnoredFields(ignore);
  for (auto &&feature : *layer) {
    highlight << qint64(feature->GetFID());
    if (highlight.size() == 256) break;
  }
  return total;
}

bool SeismicClusterPatches::locateWells(const QString &sourcePath, QVariantList &supportingWells,
                                        QString *error, const std::function<bool()> &cancel) {
  OGRLayer *layer = nullptr;
  const auto ds = openPatches(sourcePath, layer, error);
  if (!ds) return false;
  for (auto &item : supportingWells) {
    if (cancel && cancel()) return fail(error, QObject::tr("已取消"));
    auto well = item.toMap();
    if (!well.value("voted").toBool()) continue;
    bool hasX = false, hasY = false;
    const double x = well.value("cell_x").toDouble(&hasX), y = well.value("cell_y").toDouble(&hasY);
    if (!hasX || !hasY || !std::isfinite(x) || !std::isfinite(y))
      return fail(error, QObject::tr("井的聚类像元中心无效"));
    OGRPoint center(x, y);
    layer->SetSpatialFilter(&center);
    QVariant patch;
    for (auto &&f : *layer) {
      if (f->GetFieldAsInteger("cluster_id") != well.value("cluster_id").toInt()) continue;
      if (f->GetGeometryRef() && f->GetGeometryRef()->Contains(&center)) {
        if (patch.isValid()) return fail(error, QObject::tr("井像元所属聚类面不唯一"));
        patch = field(f.get(), "patch_id");
      }
    }
    if (!patch.isValid()) return fail(error, QObject::tr("井的原始聚类像元与图斑不匹配"));
    well.insert("patch_id", patch);
    item = well;
  }
  return true;
}

bool SeismicClusterPatches::writeWellAssignment(const QString &sourcePath, const QString &outputPath,
    const QVariantList &facies, const QVariantList &clusterMapping, const QVariantList &supportingWells,
    QString *error, const std::function<bool()> &cancel) {
  QHash<int, QVariantMap> labels;
  QHash<qint64, int> counts;
  for (const auto &item : clusterMapping) {
    const auto row = item.toMap();
    labels.insert(row.value("cluster_id").toInt(), row);
  }
  for (const auto &item : supportingWells) {
    const auto row = item.toMap();
    if (!row.value("voted").toBool()) continue;
    if (row.value("patch_id").isNull()) return fail(error, QObject::tr("支持井缺少图斑编号"));
    ++counts[row.value("patch_id").toLongLong()];
  }
  return writeSnapshot(sourcePath, outputPath, [&](OGRFeature &f, QString *err) {
    const auto row = labels.value(f.GetFieldAsInteger("cluster_id"));
    if (row.isEmpty()) return fail(err, QObject::tr("聚类面缺少对应的井票记录"));
    const auto code = row.value("facies_code");
    setCode(f, facies, code);
    if (code.toInt() > 0) f.SetField("well_suggestion", code.toInt());
    f.SetField("assignment_status", row.value("status").toString().toUtf8().constData());
    f.SetField("cluster_wells", row.value("vote_count").toInt());
    f.SetField("patch_wells", counts.value(f.GetFieldAsInteger64("patch_id")));
    return true;
  }, cancel, error);
}

bool SeismicClusterPatches::writeManualAssignment(const QString &sourcePath, const QString &outputPath,
    const QVariantList &facies, qint64 patchId, bool wholeCluster, int faciesCode,
    int *changedCount, QString *error, qint64 featureId) {
  const bool validCode = std::any_of(facies.begin(), facies.end(), [faciesCode](const QVariant &entry) {
    return entry.toMap().value("code").toInt() == faciesCode && faciesCode > 0;
  });
  if (!validCode) return fail(error, QObject::tr("请选择目标分类中的微相"));
  OGRLayer *layer = nullptr;
  const auto ds = openPatches(sourcePath, layer, error);
  if (!ds) return false;
  OGRFeatureUniquePtr selected;
  if (featureId >= 0) {
    selected.reset(layer->GetFeature(featureId));
  } else {
    if (layer->SetAttributeFilter(QStringLiteral("patch_id = %1").arg(patchId).toUtf8().constData()) != OGRERR_NONE)
      return fail(error, QObject::tr("无法定位所选聚类面"));
    selected.reset(layer->GetNextFeature());
    OGRFeatureUniquePtr duplicate(layer->GetNextFeature());
    if (duplicate) return fail(error, QObject::tr("原始图斑已分割，请从解释图中选择具体的面"));
  }
  if (!selected || (!field(selected.get(), "patch_id").isNull() && selected->GetFieldAsInteger64("patch_id") != patchId))
    return fail(error, QObject::tr("所选聚类面已改变，请重新选择"));
  const int cluster = selected->GetFieldAsInteger("cluster_id");
  if (wholeCluster && cluster <= 0) return fail(error, QObject::tr("所选面包含多个聚类，请按当前面调整"));
  int count = 0;
  const bool saved = writeSnapshot(sourcePath, outputPath, [&](OGRFeature &f, QString *) {
    if (wholeCluster ? f.GetFieldAsInteger("cluster_id") == cluster : f.GetFID() == selected->GetFID()) {
      setCode(f, facies, faciesCode);
      f.SetField("assignment_status", "manual");
      ++count;
    } else if (!f.IsFieldSetAndNotNull(f.GetFieldIndex("assignment_status"))) {
      f.SetField("assignment_status", "unassigned");
    }
    return true;
  }, {}, error);
  if (saved && changedCount) *changedCount = count;
  return saved;
}
