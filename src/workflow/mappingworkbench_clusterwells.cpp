// 层：功能
#include "mappingworkbench.h"
#include "analysisresultsync.h"
#include "domain/faciestaxonomy.h"
#include "mockseismicinputs.h"
#include "seismicwellintervals.h"
#include "qgis/clusterassignmentgrid.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/seismicclusterpatches.h"
#include "qgis/seismicwellcalibration.h"
#include "services/algorithmspec.h"
#include "services/analysisunitcontext.h"
#include "services/futuretask.h"
#include <QDir>
#include <QFile>
#include <QtConcurrent>

struct ClusterWellPreview {
  QString patchLayer, horizon, projectDir, schemaId, error;
  QStringList selectedVersions, usedVersions;
  QVariantList sources, schema;
  QVariantMap summary;
  CatalogVersion raster, patches;
  QVector<DerivedFileFingerprint> inputs;
  DerivedPreparedSet outputs;
  std::atomic_bool cancelled{false};
  bool ready = false, published = false;
  ~ClusterWellPreview() {
    if (published) return;
    for (const auto &member : outputs.members) {
      const auto path = member.staging.absolutePath;
      QFile::remove(path);
      QDir().rmdir(QFileInfo(path).absolutePath());
    }
  }
};

namespace {
AlgorithmSpec assignmentSpec() {
  AlgorithmSpec spec;
  spec.complete = true;
  spec.id = QStringLiteral("paleo:seismic_cluster_well_assignment");
  spec.outputs = {{"raster","raster"},{"patches","vector"}};
  return spec;
}

bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}

QVariantList selectedSources(const QVariantList &sources,const QStringList &selection) {
  if (selection.isEmpty()) return sources;
  QVariantList found;
  for (const auto &s : sources)
    if (selection.contains(s.toMap().value("version_id").toString())) found.append(s);
  return found;
}

bool verifyFile(ClusterWellPreview &job, const QString &path, const QString &digest) {
  auto stamp = DerivedFileFingerprint::read(path,[&]{return job.cancelled.load();},&job.error);
  if (stamp.sha256.isEmpty() || stamp.sha256 != digest)
    return fail(&job.error,QObject::tr("来源文件缺失或已改变，请重新选择成果"));
  job.inputs.append(stamp);
  return true;
}

void prepareOutputs(ClusterWellPreview &job,const ClusterTransformSnapshot &context) {
  const auto cancelled = [&job]{return job.cancelled.load();};
  const auto rasterPath = DataCatalog::resolvedVersionPath(job.projectDir,job.raster);
  const auto patchPath = DataCatalog::resolvedVersionPath(job.projectDir,job.patches);
  if (!verifyFile(job,rasterPath,job.raster.sha256) || !verifyFile(job,patchPath,job.patches.sha256)) return;
  // Retain cheap TOCTOU stamps. Each provider verifies the actual bytes once
  // (the JSON adapter hashes the same bytes it parses).
  QStringList selectedVersions;
  for (const auto &s : job.sources) {
    const auto item=s.toMap();
    selectedVersions.append(item.value("version_id").toString());
    const QFileInfo info(item.value("path").toString());
    DerivedFileFingerprint stamp;
    stamp.path=info.absoluteFilePath(); stamp.size=info.size(); stamp.modified=info.lastModified();
    stamp.sha256=item.value("sha256").toString();
    if (!stamp.matches(stamp.path)) { job.error=QObject::tr("井预测文件缺失或摘要无效"); return; }
    job.inputs.append(stamp);
  }
  auto grid = readClusterAssignmentGrid(rasterPath,cancelled);
  if (!grid.error.isEmpty()) { job.error = grid.error; return; }
  QVariantList excluded;
  const auto readIntervals=[&](QVariantMap &source, QString *error) {
    return readSeismicWellInterval(source,job.schema,cancelled,error);
  };
  const auto wells = readSeismicCalibrationWells(job.sources,job.schema,grid.crsWkt,*context,
                                                  &job.usedVersions,&job.error,cancelled,&excluded,readIntervals,
                                                  !job.selectedVersions.isEmpty());
  if (!job.error.isEmpty()) return;
  auto assigned = labelSeismicClusters(std::move(grid),wells,cancelled);
  if (!assigned.error.isEmpty()) { job.error = assigned.error; return; }
  if (!SeismicClusterPatches::locateWells(patchPath,assigned.supportingWells,&job.error,cancelled)) return;
  assigned.supportingWells.append(excluded);
  job.summary = {{"cluster_facies_mapping",assigned.clusterMapping},{"supporting_wells",assigned.supportingWells},
                 {"voting_wells",assigned.parameters.value("voting_wells")},{"facies",job.schema},
                 {"selected_well_versions",selectedVersions},
                 {"well_selection_mode",job.selectedVersions.isEmpty()?"latest_matching":"explicit"}};
  if (!job.summary.value("voting_wells").toInt()) return;
  auto &raster = job.outputs.members[0];
  auto &patches = job.outputs.members[1];
  if (!MappingArtifactWriter::raster(raster.staging.absolutePath,assigned.cells,assigned.columns,
                                     assigned.rows,assigned.extent,assigned.crsWkt,&job.error)) return;
  if (!SeismicClusterPatches::writeWellAssignment(patchPath,patches.staging.absolutePath,job.schema,
                              assigned.clusterMapping,assigned.supportingWells,&job.error,cancelled)) return;
  for (auto &member : job.outputs.members) {
    member.fingerprint = DerivedFileFingerprint::read(member.staging.absolutePath,cancelled,&job.error);
    if (member.fingerprint.sha256.isEmpty()) return;
  }
}

DerivedPreparedMember member(const DerivedStaging &target, bool raster, const QString &horizon) {
  const QString kind = raster ? QStringLiteral("seismic_prediction") : QStringLiteral("facies_polygons");
  DerivedPreparedMember m;
  m.portId = raster ? "raster" : "patches"; m.staging = target;
  m.extra = {{"kind",kind},{"layer_id",QStringLiteral("product.")+target.versionId},
             {"layer_type",raster?"raster":"vector"},{"group","02_Prediction"},
             {"result_kind",raster?"well_assigned_cluster_raster":"well_assigned_cluster_patches"},
             {"source_suffix",raster?"":"|layername=cluster_patches"},
             {"display_mode","micro_facies"},
             {"title",raster?QObject::tr("%1 · 井赋相栅格").arg(horizon):QObject::tr("%1 · 井赋相面").arg(horizon)}};
  return m;
}
}

QVariantList MappingWorkbench::clusterWellSources(const QString &patchLayerId,QString *error) {
  if(error) error->clear();
  const auto patch = versionForLayer(patchLayerId);
  if (patch.extra.value("kind") != "seismic_cluster_patches") {
    fail(error,tr("请选择已保存的原始聚类面")); return {};
  }
  const auto h = patch.extra.value("horizon").toString();
  if (!ready(h,error)) return {};
  const auto raw = m_catalog->versionById(patch.extra.value("cluster_raster_version").toString());
  if (raw.extra.value("kind") != "seismic_clusters" || raw.extra.value("stale").toBool()) {
    fail(error,tr("原始簇图已过期或缺失")); return {};
  }
  const auto unit = analysisResultUnitSelection(m_catalog,raw);
  if(unit.isValid() && !paleo::strat::analysisUnitGeometryCurrentFor(m_dir,unit,
          paleo::strat::AnalysisGeometryDependency::SeismicSurface,0,error)) return {};
  return mockSeismicCalibrationSources(m_catalog,m_dir,h,unit);
}

bool MappingWorkbench::previewClusterWells(const QString &id,const QStringList &versions,QString *error) {
  cancelClusterWellPreview();
  auto job = std::make_shared<ClusterWellPreview>();
  const auto all = clusterWellSources(id,&job->error);
  if(!job->error.isEmpty()) return fail(error,job->error);
  if(!m_catalog || m_catalog->refusesWrites() || !m_project || m_project->isReadOnly() || !m_project->allowsProjectWritesAt(m_dir))
    return fail(error,tr("当前工程位置不可写"));
  job->sources = selectedSources(all,versions);
  if(!versions.isEmpty() && job->sources.size()!=versions.size()) return fail(error,tr("所选井预测已过期或不属于当前层段"));
  job->patchLayer=id; job->projectDir=m_dir; job->selectedVersions=versions;
  job->patches=versionForLayer(id);
  job->raster=m_catalog->versionById(job->patches.extra.value("cluster_raster_version").toString());
  job->horizon=job->patches.extra.value("horizon").toString();
  job->schemaId=schemaVersion(job->horizon);
  if(!FaciesTaxonomy::normalize(facies(job->horizon), &job->schema, error)) return false;
  DerivedAssetRegistrar registrar(m_catalog,m_dir);
  const auto raster=registrar.stage("seismic_prediction",job->horizon+tr(" 井赋相栅格"),"facies.tif",error);
  if(!raster.isValid()) return false;
  job->outputs.members.append(member(raster,true,job->horizon));
  const auto patches=registrar.stage("facies_polygons",job->horizon+tr(" 井赋相面"),"facies.gpkg",error);
  if(!patches.isValid()) return false;
  job->outputs.members.append(member(patches,false,job->horizon));
  job->outputs.id=patches.versionId; job->outputs.algorithmId=assignmentSpec().id;
  const auto context=clusterTransformContext(m_project);
  auto *watcher=new QFutureWatcher<void>(this);
  connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,job] {
    watcher->deleteLater();
    if(job!=m_clusterPreview || job->cancelled.load()) return;
    if(!job->error.isEmpty()) { m_clusterPreview.reset(); emit errorOccurred(job->error); return; }
    job->ready=true;
    emit clusterWellPreviewReady(job->patchLayer,job->summary);
  });
  m_clusterPreview=job;
  watcher->setFuture(QtConcurrent::run(paleo::tasks::pool(m_taskSvc),[job,context]{prepareOutputs(*job,context);}));
  return true;
}

void MappingWorkbench::cancelClusterWellPreview() {
  if(m_clusterPreview) m_clusterPreview->cancelled.store(true);
  m_clusterPreview.reset();
}

bool MappingWorkbench::applyClusterWellPreview(const QString &id,QString *error) {
  auto job=m_clusterPreview;
  if(!job || !job->ready || job->patchLayer!=id || job->projectDir!=m_dir ||
     job->patches.id!=versionForLayer(id).id || !job->summary.value("voting_wells").toInt())
    return fail(error,tr("请先预览至少一口落入聚类区的有效井"));
  if(!m_catalog || m_catalog->refusesWrites() || !m_project || m_project->isReadOnly() || !m_project->allowsProjectWritesAt(m_dir))
    return fail(error,tr("当前工程位置不可写"));
  QString refreshError;
  const auto all=clusterWellSources(id,&refreshError);
  if(!refreshError.isEmpty()) return fail(error,refreshError);
  if(schemaVersion(job->horizon)!=job->schemaId || selectedSources(all,job->selectedVersions)!=job->sources)
    return fail(error,tr("井预测、井位或相分类已变化，请重新预览"));
  for(const auto &input : job->inputs)
    if(!input.matches(input.path)) return fail(error,tr("井预测或聚类文件已变化，请重新预览"));
  // A partner can supply the first well result before a project taxonomy
  // has ever been saved. Freeze a real taxonomy version on the Save action.
  if(job->schemaId.isEmpty()) {
    if(!saveFacies(job->horizon,job->schema,error)) return false;
    job->schemaId=schemaVersion(job->horizon);
    if(job->schemaId.isEmpty()) return fail(error,tr("无法登记井赋相使用的分类版本"));
  }
  auto parents=job->usedVersions;
  parents.append(job->patches.id); parents.append(job->raster.id);
  if(!job->schemaId.isEmpty()) parents.append(job->schemaId);
  QVariantMap extra=job->summary;
  extra.insert("unitSelection",job->raster.extra.value("unitSelection"));
  extra.insert("mock",job->raster.extra.value("mock").toBool());
  extra.insert("synthetic_input",job->raster.extra.value("synthetic_input").toBool());
  extra.insert("mapping_product",true); extra.insert("schema_version",job->schemaId);
  extra.insert("horizon",job->horizon); extra.insert("method",assignmentSpec().id);
  extra.insert("cluster_raster_version",job->raster.id); extra.insert("cluster_patches_version",job->patches.id);
  extra.insert("wells",job->summary.value("supporting_wells"));
  extra.insert("computational_parent_versions",parents);
  DerivedOutputSetReceipt receipt;
  DerivedAssetRegistrar registrar(m_catalog,m_dir);
  if(!registrar.commitSet(assignmentSpec(),job->outputs,parents,assignmentSpec().id,extra,receipt))
    return fail(error,receipt.error);
  job->published=true;
  m_clusterPreview.reset();
  synchronizeCatalogLayers();
  emit changed();
  emit productReady(job->horizon,QStringLiteral("product.")+job->outputs.id);
  const auto syncError=syncAnalysisResults(m_catalog,m_dir);
  if(!syncError.isEmpty()) emit errorOccurred(syncError);
  if(!m_project->projectPath().isEmpty() && !m_project->writeProject())
    emit errorOccurred(tr("井赋相成果已保存，工程视图保存失败"));
  return true;
}
