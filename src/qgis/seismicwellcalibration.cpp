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
QgsCoordinateReferenceSystem nativeMetreCrs(const QVariantMap &position) {
  QgsCoordinateReferenceSystem crs;
  crs.createFromUserInput(position.value("crs").toString());
  if (crs.mapUnits() == Qgis::DistanceUnit::Meters ||
      crs == QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt())) return crs;
  return {};
}
}
QVector<SeismicClusterWell> readSeismicCalibrationWells(
    const QVariantList &sources, const QVariantList &schema,
    const QString &gridCrsWkt, const QgsCoordinateTransformContext &context,
    QStringList *usedVersions, QString *error, const std::function<bool()> &cancel, QVariantList *excluded,
    const std::function<bool(QVariantMap &,QString *)> &readIntervals, bool verifySkippedIntervals) {
  error->clear(); usedVersions->clear();
  QVector<SeismicClusterWell> result;
  QSet<QString> seen;
  const auto target = QgsCoordinateReferenceSystem::fromWkt(
      gridCrsWkt.isEmpty() ? DataCatalog::localGridCrsWkt() : gridCrsWkt);
  for (const auto &value : sources) {
    if (cancel && cancel()) { *error = QObject::tr("已取消"); return {}; }
    auto source = value.toMap();
    const auto file = source.value("path").toString();
    const auto version=source.value("version_id").toString();
    const auto exclude=[&](const QString &id,const QString &reason) {
      if(excluded) excluded->append(QVariantMap{{"id",id},{"version_id",version},{"voted",false},{"reason",reason}});
    };
    if(source.value("format")=="intervals") {
      const auto id=source.value("well_id").toString();
      if(source.value("invalid_identity").toBool()) { *error=QObject::tr("单井分段预测缺少唯一井身份或有效的井口坐标"); return {}; }
      const bool superseded = seen.contains(id);
      if (verifySkippedIntervals && (superseded || source.contains("exclude_reason")) &&
          DataCatalog::sha256FileHex(file, error, cancel) != source.value("sha256").toString()) {
        *error=QObject::tr("单井分段预测文件已改变"); return {};
      }
      if(superseded) continue;
      seen.insert(id); if(!usedVersions->contains(version)) usedVersions->append(version);
      if (!source.contains("exclude_reason")) {
        if (!readIntervals || !readIntervals(source,error)) {
          if(error->isEmpty()) *error=QObject::tr("无法解析已保存的单井分段预测");
          return {};
        }
      }
      if(source.contains("exclude_reason")) { exclude(id,source.value("exclude_reason").toString()); continue; }
      const auto pos=source.value("position").toMap();
      const auto crs = nativeMetreCrs(pos);
      QgsCoordinateTransform tx(crs,target,context);
      if(!crs.isValid() || !target.isValid() || (crs!=target && !tx.isValid())) {
        *error=QObject::tr("井口坐标无法转换到地震网格"); return {};
      }
      try {
        auto pt=tx.transform(QgsPointXY(pos.value("x").toDouble(),pos.value("y").toDouble()));
        result.append({id,version,pt.x(),pt.y(),source.value("single_code").toInt(),"dataservice_wellhead",pos.value("name").toString()});
      } catch(const QgsCsException &) { *error=QObject::tr("井口坐标转换失败"); return {}; }
      continue;
    }
    if (DataCatalog::sha256FileHex(file, error, cancel) != source.value("sha256").toString()) {
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
    const auto positions=source.value("native_positions").toMap();
    const bool legacyLocal=layer.crs()==QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
    QgsCoordinateTransform transform;
    if(!legacyLocal)transform=QgsCoordinateTransform(layer.crs(),target,context);
    if (!legacyLocal&&layer.crs() != target && !transform.isValid()) {
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
      if (!ok || sourceCode <= 0 || feature.geometry().isEmpty() || feature.geometry().isMultipart()) { exclude(id,"missing_prediction_or_point"); continue; }
      if (feature.fields().indexOf("prediction_status") >= 0 &&
          feature.attribute("prediction_status").toString() == QStringLiteral("未预测")) { exclude(id,"unpredicted"); continue; }
      if (feature.fields().indexOf("facies_scope") >= 0 &&
          feature.attribute("facies_scope").toString() == QLatin1String("well")) { exclude(id,"whole_well"); continue; }
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
        auto point=feature.geometry().asPoint();QString coordinateSource;
        if(legacyLocal){
          const auto native=positions.value(id).toMap();const double x=native.value("x",NAN).toDouble(),y=native.value("y",NAN).toDouble();
          const auto crs = nativeMetreCrs(native);
          if(native.isEmpty()||native.value("id").toLongLong()<=0||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(point.x())||!std::isfinite(point.y())||!crs.isValid()||std::hypot(point.x()-x,point.y()-y)>1e-6){
            *error=QObject::tr("井 %1 的旧预测坐标与 DataService 井位不一致，不能用于地震校准").arg(id);return {};
          }
          QgsCoordinateTransform nativeTransform(crs,target,context);
          if(crs!=target&&!nativeTransform.isValid()){*error=QObject::tr("测井点无法转换到地震网格坐标系，请先完成配准");return {};}
          point=nativeTransform.transform(point);coordinateSource=QStringLiteral("dataservice_verified_legacy:%1").arg(native.value("id").toLongLong());
        }else { point=transform.transform(point); coordinateSource=QStringLiteral("prediction_point"); }
        if (std::isfinite(point.x()) && std::isfinite(point.y()))
          result << SeismicClusterWell{id, versionId, point.x(), point.y(), code,coordinateSource,
              feature.fields().indexOf("name")>=0 ? feature.attribute("name").toString() : positions.value(id).toMap().value("name").toString()};
      } catch (const QgsCsException &) {
        *error = QObject::tr("测井点无法转换到地震网格坐标系，请先完成配准"); return {};
      }
    }
  }
  return result;
}
