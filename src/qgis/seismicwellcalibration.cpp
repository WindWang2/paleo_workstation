// 层：QGIS 封装
#include "seismicwellcalibration.h"
#include "mappingartifactwriter.h"
#include "../catalog/datacatalog.h"
#include "../domain/faciestaxonomy.h"
#include <QSet>
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

namespace {
QStringList path(const QVariantMap &row) {
  return {row.value("facies").toString(), row.value("subfacies").toString(),
          row.value("microfacies").toString()};
}
}
QVector<SeismicClusterWell> readSeismicCalibrationWells(
    const QVariantList &sources, const QVariantList &schema,
    const QString &gridCrsWkt, const QgsCoordinateTransformContext &context,
    QStringList *usedVersions, QString *error, const std::function<bool()> &cancel) {
  error->clear(); usedVersions->clear();
  QVector<SeismicClusterWell> result;
  QSet<QString> seen;
  const auto target = QgsCoordinateReferenceSystem::fromWkt(
      gridCrsWkt.isEmpty() ? DataCatalog::localGridCrsWkt() : gridCrsWkt);
  for (const auto &value : sources) {
    if (cancel && cancel()) { *error = QObject::tr("已取消"); return {}; }
    const auto source = value.toMap();
    const auto file = source.value("path").toString();
    if (DataCatalog::sha256FileHex(file) != source.value("sha256").toString()) {
      *error = QObject::tr("测井相预测源文件缺失或已改变，请重新保存测井结果"); return {};
    }
    QgsVectorLayer layer(file, "well-calibration", "ogr");
    MappingArtifactWriter::restoreVectorCrs(&layer);
    if (!layer.isValid() || layer.geometryType() != Qgis::GeometryType::Point ||
        layer.fields().indexOf("facies_code") < 0 || !layer.crs().isValid() || !target.isValid()) {
      *error = QObject::tr("测井相预测缺少有效井点、相编码或坐标系"); return {};
    }
    QVariantList sourceSchema;
    if (!FaciesTaxonomy::upgrade(source.value("facies").toList(), &sourceSchema, error)) return {};
    QgsCoordinateTransform transform(layer.crs(), target, context);
    if (layer.crs() != target && !transform.isValid()) {
      *error = QObject::tr("测井点无法转换到地震网格坐标系，请先完成配准"); return {};
    }
    auto features = layer.getFeatures();
    QgsFeature feature;
    while (features.nextFeature(feature)) {
      if (cancel && cancel()) { *error = QObject::tr("已取消"); return {}; }
      const auto id = feature.fields().indexOf("id") >= 0
          ? feature.attribute("id").toString() : feature.attribute("well_id").toString();
      if (id.isEmpty() || seen.contains(id)) continue;
      seen.insert(id); // a newer skipped/undefined result also supersedes old predictions
      const auto versionId = source.value("version_id").toString();
      if (!usedVersions->contains(versionId)) *usedVersions << versionId;
      bool ok = false;
      const int sourceCode = feature.attribute("facies_code").toInt(&ok);
      if (!ok || sourceCode <= 0 || feature.geometry().isEmpty() || feature.geometry().isMultipart()) continue;
      if (feature.fields().indexOf("prediction_status") >= 0 &&
          feature.attribute("prediction_status").toString() == QStringLiteral("未预测")) continue;
      if (feature.fields().indexOf("facies_scope") >= 0 &&
          feature.attribute("facies_scope").toString() == QLatin1String("well")) continue;
      QVariantMap leaf;
      for (const auto &row : sourceSchema)
        if (row.toMap().value("code").toInt() == sourceCode) leaf = row.toMap();
      int code = 0;
      if (!leaf.isEmpty())
        for (const auto &row : schema)
          if (path(row.toMap()) == path(leaf)) code = row.toMap().value("code").toInt();
      if (!code) {
        *error = QObject::tr("测井相与当前层位分类不一致，请先统一三级分类归属"); return {};
      }
      try {
        const auto point = transform.transform(feature.geometry().asPoint());
        if (std::isfinite(point.x()) && std::isfinite(point.y()))
          result << SeismicClusterWell{id, versionId, point.x(), point.y(), code};
      } catch (const QgsCsException &) {
        *error = QObject::tr("测井点无法转换到地震网格坐标系，请先完成配准"); return {};
      }
    }
  }
  return result;
}
