// 层：功能
// 相栅格转面：同步路径 + 后台任务（JobRunner 三段式）。从 mappingworkbench.cpp
// 析出——校验/登记语义与原同步实现逐条一致，只把 processing 段挪到 worker。
#include "mappingworkbench.h"
#include "../domain/singlefactorrequest.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/mappingfeatureidentity.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include <QDir>
#include <QFile>
#include <QUuid>
#include <qgscoordinatereferencesystem.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
} // namespace

bool MappingWorkbench::preparePolygonize(const QString &id, PolygonizeJob *job,
                                         QString *error) {
  const auto d = declaration(id);
  if (!ready(d.horizon, error))
    return false;
  if (d.type != "raster") {
    fail(error, tr("栅格转面需要选择相栅格"));
    return false;
  }
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return false;
  const auto parent = ensureInputVersion(id, error);
  if (parent.isEmpty())
    return false;
  const auto sourceVersion = m_catalog->versionById(parent);
  const auto sourceKind = sourceVersion.extra.value("kind").toString();
  const auto valueSource = sourceVersion.extra.value("value_source").toString();
  if (id.startsWith("factor.") ||
      sourceKind == QLatin1String("single_factor_raster") ||
      paleo::singlefactor::rejectsQuantitativeUse(sourceKind, valueSource)) {
    fail(error, paleo::singlefactor::rejectsQuantitativeUse(sourceKind, valueSource)
                    ? tr("解释性制图工作场不能参与分相或转面")
                    : tr("连续单因素须先按阈值分相，再转为相面"));
    return false;
  }
  if (!QDir().mkpath(m_dir + "/artifacts/staging")) {
    fail(error, tr("无法创建暂存目录：%1").arg(m_dir + "/artifacts/staging"));
    return false;
  }
  job->sourceLayerId = id;
  job->horizon = d.horizon;
  job->title = d.title;
  // worker 上独立建层：只交 URI（图层对象属主线程，不得跨线程传）。
  job->rasterUri = layer->source();
  job->crsWkt = layer->crs().toWkt();
  job->outputPath = m_dir + "/artifacts/staging/" +
                    QUuid::createUuid().toString(QUuid::WithoutBraces) + ".gpkg";
  job->parent = parent;
  job->dir = m_dir;
  job->facies = sourceVersion.extra.value("facies", facies(d.horizon)).toList();
  job->mock = sourceVersion.extra.value("mock");
  job->catalog = m_catalog;
  return true;
}

bool MappingWorkbench::computePolygonize(PolygonizeJob &job,
                                         const std::function<bool()> &cancelled,
                                         const std::function<void(double)> &progress,
                                         QgisProcessingService *processing) {
  if (!processing) {
    job.error = tr("处理服务不可用");
    return false;
  }
  // 相栅格转面默认开平滑+聚合：逐像元聚类的椒盐噪点若直接多边形化会碎成
  // 噪声图斑（用户要求——「一定要做平滑和聚合，不然太细了」）。MIN_FRACTION
  // 按有效区占比吸收小斑块进最大邻斑——大栅格上固定像元数兜不住的碎斑
  // 都能并掉（2026-10-11 用户反馈相面小斑块太多）。
  QgsRasterLayer raster(job.rasterUri, QStringLiteral("facies"),
                        QStringLiteral("gdal"));
  if (!raster.isValid()) {
    job.error = tr("无法读取相栅格：%1").arg(job.rasterUri);
    return false;
  }
  if (!raster.crs().isValid() && !job.crsWkt.isEmpty())
    raster.setCrs(QgsCoordinateReferenceSystem::fromWkt(job.crsWkt));
  QgisProcessingService::ProcessingHooks hooks;
  hooks.cancelled = cancelled;
  hooks.progress = [progress](double p) {
    if (progress)
      progress(p * 0.9); // 末 10% 留给相属性同步
  };
  QString runErr;
  const auto output = processing->run(
      "paleo:paleo_facies_polygonize",
      {{"INPUT", QVariant::fromValue(static_cast<QgsMapLayer *>(&raster))},
       {"OUTPUT", job.outputPath},
       {"SMOOTH", 1},
       {"MIN_CELLS", 4},
       {"MIN_FRACTION", 0.0025},
       {"MIN_AREA", 0.0},
       {"SIMPLIFY", 0.0}},
      &runErr, hooks);
  if (output.isEmpty()) {
    job.error = runErr.isEmpty() ? tr("相栅格转面未返回结果") : runErr;
    return false;
  }
  if (cancelled && cancelled())
    return false;
  {
    QgsVectorLayer outputLayer(job.outputPath + "|layername=facies_polygons",
                               "facies", "ogr");
    QString syncErr;
    if (!MappingFeatureIdentity::ensure(&outputLayer, &syncErr)) {
      job.error = syncErr; return false;
    }
    if (!MappingArtifactWriter::syncFaciesAttributes(&outputLayer, job.facies,
                                                     &syncErr)) {
      job.error = syncErr;
      return false;
    }
  }
  job.fingerprint =
      DerivedFileFingerprint::read(job.outputPath, cancelled, &job.error);
  if (job.fingerprint.sha256.isEmpty())
    return false;
  if (progress)
    progress(100.0);
  return true;
}

bool MappingWorkbench::commitPolygonize(PolygonizeJob &job, QString *error) {
  // 会话守卫：任务期间切换工程/重绑目录 → 旧结果不得写进新工程。
  if (!m_catalog || m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，转面结果已丢弃"));
    return false;
  }
  const auto id =
      record(job.outputPath, job.horizon, "facies_polygons",
             tr("%1 相面").arg(job.title), "vector", {job.parent},
             {{"facies", QVariant(job.facies)},
              {"computational_parent_versions", QStringList{job.parent}},
              {"method", "paleo:paleo_facies_polygonize"},
              {"mock", job.mock},
              {"source_layer", job.sourceLayerId}},
             error, "|layername=facies_polygons", {}, &job.fingerprint);
  job.productId = id;
  return !id.isEmpty();
}

namespace {
void removeStaging(const QString &path) {
  if (path.isEmpty())
    return;
  for (const auto &suffix : {"", "-wal", "-shm", "-journal"})
    QFile::remove(path + QString::fromLatin1(suffix));
}
} // namespace

QString MappingWorkbench::polygonize(const QString &id, QString *error) {
  PolygonizeJob job;
  if (!preparePolygonize(id, &job, error))
    return {};
  if (!computePolygonize(job, {}, {}, m_processing)) {
    removeStaging(job.outputPath);
    fail(error, job.error);
    return {};
  }
  if (!commitPolygonize(job, error)) {
    removeStaging(job.outputPath);
    return {};
  }
  return job.productId;
}

bool MappingWorkbench::polygonizeBusy() const {
  return m_polygonizeRunner && m_polygonizeRunner->busy();
}

void MappingWorkbench::cancelPolygonize() {
  if (m_polygonizeRunner)
    m_polygonizeRunner->requestCancel();
}

bool MappingWorkbench::startPolygonize(const QString &id, QString *error) {
  if (!m_taskSvc) {
    // 无任务服务（测试壳）：同步跑，收尾语义一致。
    return !polygonize(id, error).isEmpty();
  }
  using paleo::jobs::JobRunner;
  if (!m_polygonizeRunner) {
    m_polygonizeRunner = std::make_unique<JobRunner<PolygonizeJob>>(this);
  }
  m_polygonizeRunner->setTaskService(m_taskSvc);
  if (m_polygonizeRunner->busy()) {
    fail(error, tr("已有相栅格转面任务在进行，请等待完成或在任务中心取消"));
    return false;
  }
  auto job = std::make_shared<PolygonizeJob>();
  if (!preparePolygonize(id, job.get(), error))
    return false;

  JobRunner<PolygonizeJob>::Callbacks cb;
  QgisProcessingService *processing = m_processing;
  cb.compute = [processing](PolygonizeJob &j, const paleo::jobs::CancelFn &cancel,
                            const paleo::jobs::ProgressFn &report) {
    return computePolygonize(
        j, cancel,
        [&report](double p) {
          if (report)
            report(p, p < 90.0 ? QStringLiteral("build")
                               : QStringLiteral("publish"));
        },
        processing);
  };
  cb.commit = [this](PolygonizeJob &j, QString *err) {
    if (!j.error.isEmpty()) { // compute 失败态也进 commit：如实上报
      if (err)
        *err = j.error;
      emit polygonizeFinished(j.sourceLayerId, {}, j.error, false);
      return false;
    }
    QString commitErr;
    const bool ok = commitPolygonize(j, &commitErr);
    if (!ok) {
      removeStaging(j.outputPath);
      j.error = commitErr;
      if (err)
        *err = commitErr;
    }
    emit polygonizeFinished(j.sourceLayerId, j.productId, j.error, false);
    return ok;
  };
  cb.cleanup = [](PolygonizeJob &j) {
    if (j.productId.isEmpty())
      removeStaging(j.outputPath);
  };
  cb.onDropped = [this](PolygonizeJob &j, paleo::jobs::DropReason reason) {
    emit polygonizeFinished(j.sourceLayerId, {},
                            paleo::jobs::dropReasonText(reason), true);
  };
  const QString title = tr("相栅格转相面 · %1").arg(
      job->title.isEmpty() ? job->sourceLayerId : job->title);
  PaleoTask *task = m_polygonizeRunner->start(title, job, cb, QString(),
                                              /*quiet=*/false);
  if (!task) {
    removeStaging(job->outputPath);
    fail(error, tr("转面任务未能启动"));
    return false;
  }
  return true;
}

MappingWorkbench::PolygonizeJob::~PolygonizeJob() { removeStaging(outputPath); }
