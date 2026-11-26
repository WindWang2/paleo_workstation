// 层：功能
#include "mappingworkbench.h"
#include "analysisresultsync.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/seismicclusterpatches.h"
#include "../services/futuretask.h"
#include <QtConcurrent>
#include <QDir>
#include <QScopedValueRollback>
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

void MappingWorkbench::previewClusterScope(const QString &id, int cluster, QObject *receiver, ClusterScopeReady completed) {
  const auto version = versionForLayer(id);
  if (!isClusterSurface(version)) { completed(-1, {}, tr("请选择已保存的聚类面或其赋相版本")); return; }
  struct Scope { qint64 count; QList<qint64> ids; QString error; };
  const auto path = DataCatalog::resolvedVersionPath(m_dir, version);
  auto *watcher = new QFutureWatcher<Scope>(receiver);
  connect(watcher, &QFutureWatcherBase::finished, receiver, [watcher, completed] {
    const auto result = watcher->result();
    watcher->deleteLater();
    completed(result.count, result.ids, result.error);
  });
  watcher->setFuture(QtConcurrent::run(paleo::tasks::pool(m_taskSvc), [path, cluster] {
    Scope result{};
    result.count = SeismicClusterPatches::clusterSelection(path, cluster, result.ids, &result.error);
    return result;
  }));
}

QString MappingWorkbench::assignClusterFacies(const QString &id, qint64 patchId, bool wholeCluster,
                                             int faciesCode, QString *error, qint64 featureId) {
  const QVariantMap assignment{{"patch_id", patchId}, {"feature_id", featureId},
                               {"whole_cluster", wholeCluster}, {"facies_code", faciesCode}};
  return saveClusterAssignments(id, {assignment}, {}, error);
}

QString MappingWorkbench::saveClusterAssignments(const QString &id, const QVariantList &assignments,
                                                const QVariantList &expectedSchema, QString *error) {
  const auto version = versionForLayer(id);
  const auto h = version.extra.value("horizon").toString();
  const auto unit = paleo::strat::fromMap(version.extra.value("unitSelection").toMap());
  if (!isClusterSurface(version) || !ready(h, error, unit.isValid() ? &unit : nullptr)) {
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
  const auto schema = raw ? facies(h) : version.extra.value("facies").toList();
  if (!expectedSchema.isEmpty() && schema != expectedSchema) {
    if (error) *error = tr("微相分类已经改变，请重新核对草稿");
    return {};
  }
  if (raw && schemaVersion(h).isEmpty() && !saveFacies(h, schema, error)) return {};
  const auto schemaId = raw ? schemaVersion(h) : version.extra.value("schema_version").toString();
  const auto staging = QDir(m_dir).filePath("artifacts/staging");
  if (!QDir().mkpath(staging)) {
    if (error) *error = tr("无法创建聚类面暂存目录");
    return {};
  }
  const auto path = QDir(staging).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".gpkg");
  int changed = 0;
  if (!SeismicClusterPatches::writeManualAssignments(source, path, schema, assignments, &changed, error)) return {};
  QStringList parents{version.id};
  if (!schemaId.isEmpty()) parents << schemaId;
  QVariantMap extra{{"method", "manual-seismic-cluster-assignment"},
      {"result_kind", "manual_cluster_interpretation"}, {"result_scope", "area"},
      {"assignments", assignments}, {"changed_patches", changed}, {"facies", schema},
      {"schema_version", schemaId}, {"computational_parent_versions", parents},
      {"cluster_patches_version", raw ? version.id : version.extra.value("cluster_patches_version").toString()},
      {"cluster_raster_version", version.extra.value("cluster_raster_version")},
      {"unitSelection", version.extra.value("unitSelection")}};
  if (version.extra.contains("cluster_facies_mapping"))
    extra.insert("cluster_facies_mapping", version.extra.value("cluster_facies_mapping"));
  if (assignments.size() == 1) {
    const auto row = assignments.first().toMap();
    extra.insert("patch_id", row.value("patch_id"));
    extra.insert("source_feature_id", row.value("feature_id"));
    extra.insert("assignment_scope", row.value("whole_cluster").toBool() ? "cluster" : "patch");
    extra.insert("facies_code", row.value("facies_code"));
  }
  // Multiple synchronous layer-tree and product notifications can present the same version.
  // Preserve the view for the entire registration, just like an editing working copy.
  QScopedValueRollback<bool> preserveViewport(m_preserveEditingView, true);
  const auto output = record(path, h, "edited_facies", tr("%1 · 聚类面人工解释").arg(h),
                             "vector", parents, extra, error, "|layername=cluster_patches");
  if (!output.isEmpty()) {
    const auto syncError = syncAnalysisResults(m_catalog, m_dir);
    if (!syncError.isEmpty()) emit errorOccurred(syncError);
  }
  return output;
}
