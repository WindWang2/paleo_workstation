// 层：功能
#include "faciesclassify.h"
#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/qgislayerservice.h"
#include "services/faciesclassificationservice.h"
#include "workflow/derivedassets.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
namespace paleo::crossplot {
FaciesClassifyWorkflow::FaciesClassifyWorkflow(PaleoTaskService *t,
                                               PaleoProjectStore *s,
                                               QgisLayerService *l, QObject *p)
    : QObject(p), m_tasks(t), m_store(s), m_layers(l) {}
void FaciesClassifyWorkflow::setCatalog(DataCatalog *c, const QString &dir) {
  clear();
  m_catalog = c;
  m_projectDir = dir;
}
void FaciesClassifyWorkflow::clear() {
  cancel();
  ++m_generation;
  m_task = nullptr;
  emit busyChanged(false);
  m_samples.reset();
  m_classification = {};
}
void FaciesClassifyWorkflow::setSamples(std::shared_ptr<const SampleSet> s) {
  clear();
  m_samples = std::move(s);
}
void FaciesClassifyWorkflow::cancel() {
  if (m_task)
    m_task->requestCancel();
}
PaleoTask *FaciesClassifyWorkflow::classify(const ClassificationOptions &o) {
  if (!m_tasks || !m_samples) {
    emit failed(tr("没有任务服务或交会样本"));
    return nullptr;
  }
  cancel();
  const auto generation = ++m_generation;
  auto samples = m_samples;
  auto previous = m_classification.labels;
  auto result = std::make_shared<Classification>();
  emit busyChanged(true);
  auto *task = m_tasks->start(
      tr("交会无监督相分类"), [samples, o, previous, result](PaleoTask *task) {
        cluster::Control ctl{
            [task] { return task->cancelRequested(); },
            [task](double p) { task->reportBytes(qint64(p * 1000), 1000); }};
        *result =
            FaciesClassificationService::classify(*samples, o, ctl, previous);
        return result->cancelled ? QString() : result->error;
      });
  m_task = task;
  connect(task, &PaleoTask::changed, this, [this, task, generation] {
    if (generation == m_generation)
      emit progressChanged(task->percent());
  });
  connect(task, &PaleoTask::finished, this, [this, task, result, generation] {
    if (generation != m_generation)
      return;
    m_task = nullptr;
    emit busyChanged(false);
    if (result->ok && task->state() == PaleoTask::State::Succeeded) {
      m_classification = *result;
      emit classificationReady(m_classification);
    } else
      emit failed(result->cancelled ? tr("分类已取消，未生成新成果")
                                    : result->error);
  });
  return task;
}
FaciesProduct FaciesClassifyWorkflow::write(const QString &horizon) {
  FaciesProduct out;
  auto fail = [&](const QString &e) {
    out.error = e;
    emit failed(e);
    return out;
  };
  if (m_task && m_task->running())
    return fail(tr("分类仍在运行"));
  if (!m_catalog || m_catalog->refusesWrites() || !m_store ||
      m_store->isReadOnly() || m_projectDir.isEmpty() || !m_samples ||
      !m_classification.ok)
    return fail(tr("工程不可写或没有分类成果"));
  const auto &s = *m_samples;
  const auto &r = m_classification;
  const bool raster = s.grid.spatial && s.grid.rows > 0;
  if (raster && (!m_layers || horizon.isEmpty()))
    return fail(tr("栅格写回需要有效层位和图层服务"));
  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  QString error;
  const auto stage = registrar.stage(
      raster ? QStringLiteral("facies_classification")
             : QStringLiteral("well_facies_intervals"),
      raster ? tr("%1 交会分类").arg(horizon) : tr("交会井层段分类"),
      raster ? QStringLiteral("facies.tif")
             : QStringLiteral("well-facies.json"),
      &error);
  if (!stage.isValid())
    return fail(error);
  auto written = m_store->enqueueWrite([&] {
    const bool ok = raster ? FaciesClassificationService::writeRaster(
                                 stage.absolutePath, s, r, &error)
                           : FaciesClassificationService::writeIntervals(
                                 stage.absolutePath, s, r, &error);
    return PaleoProjectStore::WriteResult{ok, error};
  });
  if (!written.ok) {
    QFile::remove(stage.absolutePath);
    return fail(written.error);
  }
  QVariantMap extra = r.provenance;
  extra.insert("horizon", horizon);
  extra.insert("intervalSupport", "inclusive_observed_depths_no_gap_bridging");
  if (!registrar.commit(stage, s.parentVersionIds,
                        QStringLiteral("crossplot://classification"), extra,
                        &error)) {
    QFile::remove(stage.absolutePath);
    return fail(error);
  }
  out.path = stage.absolutePath;
  out.assetId = stage.assetId;
  out.versionId = stage.versionId;
  out.counts = r.counts;
  if (raster) {
    QString suffix = horizon;
    suffix.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                   QStringLiteral("_"));
    out.layerId = QStringLiteral("predict.%1.crossplot").arg(suffix);
    LayerDeclaration declaration;
    declaration.layerId = out.layerId;
    declaration.horizon = horizon;
    declaration.type = QStringLiteral("raster");
    declaration.source = out.path;
    declaration.group = QStringLiteral("02_Prediction");
    declaration.title = tr("%1 交会分类（簇编号）").arg(horizon);
    if (!m_layers->declare(declaration, &error))
      return fail(tr("成果已登记，但图层声明失败：%1").arg(error));
  } else {
    QStringList wells;
    for (const auto &loc : s.locations)
      if (!loc.wellId.isEmpty() && !wells.contains(loc.wellId))
        wells << loc.wellId;
    for (const auto &well : wells) {
      EntityAssetLink link;
      link.entityType = QStringLiteral("well");
      link.entityId = well;
      link.assetId = out.assetId;
      link.role = QStringLiteral("interpretation");
      link.note = tr("交会分类井层段");
      if (!m_catalog->addLink(link, &error))
        return fail(tr("成果已登记，但井关联失败：%1").arg(error));
    }
  }
  out.ok = true;
  emit productReady(out);
  return out;
}
} // namespace paleo::crossplot
