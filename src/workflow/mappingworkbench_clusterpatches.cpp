// 层：功能
#include "mappingworkbench.h"
#include "analysisresultsync.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/seismicclusterpatches.h"
#include <QDir>
#include <QUuid>

namespace {
bool isClusterSurface(const CatalogVersion &version) {
  const auto kind = version.extra.value("kind").toString();
  return kind == QLatin1String("seismic_cluster_patches") ||
      (!version.extra.value("cluster_patches_version").toString().isEmpty() &&
       (kind == QLatin1String("facies_polygons") || kind == QLatin1String("edited_facies")));
}
}

QVariantList MappingWorkbench::clusterPatchChoices(const QString &id, QString *error, qint64 firstFid, int limit) const {
  const auto version = versionForLayer(id);
  if (!isClusterSurface(version)) {
    if (error) *error = tr("请选择已保存的聚类面或其赋相版本");
    return {};
  }
  return SeismicClusterPatches::choices(DataCatalog::resolvedVersionPath(m_dir, version), error, firstFid, limit);
}

qint64 MappingWorkbench::clusterSelection(const QString &id, int cluster, QList<qint64> &highlight, QString *error) const {
  const auto version = versionForLayer(id);
  if (!isClusterSurface(version)) {
    if (error) *error = tr("请选择已保存的聚类面或其赋相版本");
    return -1;
  }
  return SeismicClusterPatches::clusterSelection(DataCatalog::resolvedVersionPath(m_dir, version), cluster, highlight, error);
}

QString MappingWorkbench::assignClusterFacies(const QString &id, qint64 patchId, bool wholeCluster,
                                             int faciesCode, QString *error, qint64 featureId) {
  const auto version = versionForLayer(id);
  const auto h = version.extra.value("horizon").toString();
  if (!isClusterSurface(version) || !ready(h, error)) {
    if (error && !isClusterSurface(version)) *error = tr("请选择已保存的聚类面或其赋相版本");
    return {};
  }
  if (m_catalog->refusesWrites() || !m_project || m_project->isReadOnly() || !m_project->allowsProjectWritesAt(m_dir)) {
    if (error) *error = tr("当前工程位置不可写");
    return {};
  }
  const auto source = DataCatalog::resolvedVersionPath(m_dir, version);
  if (version.sha256.isEmpty() || DataCatalog::sha256FileHex(source) != version.sha256) {
    if (error) *error = tr("聚类面文件与登记版本不一致，请重新生成");
    return {};
  }
  const bool raw = version.extra.value("kind") == QLatin1String("seismic_cluster_patches");
  if (raw && schemaVersion(h).isEmpty() && !saveFacies(h, facies(h), error)) return {};
  const auto schema = raw ? facies(h) : version.extra.value("facies").toList();
  const auto schemaId = raw ? schemaVersion(h) : version.extra.value("schema_version").toString();
  const auto staging = QDir(m_dir).filePath("artifacts/staging");
  if (!QDir().mkpath(staging)) {
    if (error) *error = tr("无法创建聚类面暂存目录");
    return {};
  }
  const auto path = QDir(staging).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".gpkg");
  int changed = 0;
  if (!SeismicClusterPatches::writeManualAssignment(source, path, schema, patchId,
                                                   wholeCluster, faciesCode, &changed, error, featureId)) return {};
  QStringList parents{version.id};
  if (!schemaId.isEmpty()) parents << schemaId;
  const QVariantMap extra{{"method", "manual-seismic-cluster-assignment"},
      {"result_kind", "manual_cluster_interpretation"}, {"result_scope", "area"},
      {"patch_id", patchId}, {"source_feature_id", featureId}, {"assignment_scope", wholeCluster ? "cluster" : "patch"},
      {"facies_code", faciesCode}, {"changed_patches", changed}, {"facies", schema},
      {"schema_version", schemaId}, {"computational_parent_versions", parents},
      {"cluster_patches_version", raw ? version.id : version.extra.value("cluster_patches_version").toString()},
      {"cluster_raster_version", version.extra.value("cluster_raster_version")},
      {"unitSelection", version.extra.value("unitSelection")}};
  const auto output = record(path, h, "edited_facies", tr("%1 · 聚类面人工解释").arg(h),
                             "vector", parents, extra, error, "|layername=cluster_patches");
  if (!output.isEmpty()) {
    const auto syncError = syncAnalysisResults(m_catalog, m_dir);
    if (!syncError.isEmpty()) emit errorOccurred(syncError);
  }
  return output;
}
