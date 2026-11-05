// 层：功能
#include "projectlayerrefresh.h"
#include "analysisresultsync.h"
#include "domain/mappinghorizons.h"
#include "../catalog/datacatalog.h"
#include "../services/futuretask.h"
#include "../services/projectdata.h"
#include "welltrajectorylayer.h"
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QtConcurrent>

namespace {
struct PreparedRefresh {
  ProjectLayerData data;
  QVector<CatalogOp> journal;
};
}

ProjectLayerRefreshWorkflow::ProjectLayerRefreshWorkflow(QObject *parent) : QObject(parent) {}
ProjectLayerData ProjectLayerRefreshWorkflow::prepare(DataCatalog &snapshot, const QString &directory,
                                                      const std::shared_ptr<std::atomic_bool> &stop)
{
  ProjectLayerData data;
  QTemporaryDir temporary;
  QString error;
  const QString path = temporary.filePath("wells.geojson");
  data.wellsPrepared = temporary.isValid() && snapshot.writeWellsGeoJson(path, &error);
  QFile file(path);
  if (data.wellsPrepared && QFile::exists(path)) {
    data.wellsPrepared = file.open(QIODevice::ReadOnly);
    if (data.wellsPrepared) data.wellsBytes = file.readAll();
  }
  if (!error.isEmpty()) data.warnings << error;
  if (stop && stop->load()) return data;
  ProjectDataFacade facade;
  facade.setCatalog(&snapshot, directory);
  error.clear();
  data.trajectoryBytes = paleo::wellTrajectoriesGeoJson(&facade, &snapshot, &error);
  if (!error.isEmpty()) data.warnings << error;
  if (!error.isEmpty())
  {
    // #203：轨迹计算失败（返回空 + error）≠ 确认无轨迹。此时不发布空集合、
    // 不改写已持久化的 well_trajectories.geojson——一次读错误就静默毁掉
    // 既有图层数据。保留原状，由 warnings 如实披露失败原因。
    data.trajectoriesPrepared = false;
    return data;
  }
  if (data.trajectoryBytes.isEmpty() && QFile::exists(directory + "/artifacts/layers/well_trajectories.geojson")) {
    QJsonObject root{{"type", "FeatureCollection"}, {"features", QJsonArray()},
      {"crs", QJsonObject{{"type", "name"}, {"properties", QJsonObject{{"name", DataCatalog::localGridCrsWkt()}}}}}};
    data.trajectoryBytes = QJsonDocument(root).toJson();
  }
  data.trajectoriesPrepared = true;
  return data;
}
void ProjectLayerRefreshWorkflow::request(DataCatalog *catalog, const QString &directory)
{
  if (!catalog || directory.isEmpty() || !catalog->isOpen()) return;
  m_catalog = catalog; m_directory = directory; ++m_generation;
  if (m_running) { m_pending = true; return; }
  m_running = true; m_pending = false;
  const quint64 generation = m_generation, sequence = catalog->mutationSeq();
  auto snapshot = std::shared_ptr<DataCatalog>(catalog->createStagingCopy(QString()));
  m_stop = std::make_shared<std::atomic_bool>(false);
  const auto stop = m_stop;
  const auto boundaries = mappingHorizons();
  const QPointer<DataCatalog> target=catalog;
  auto *watcher = new QFutureWatcher<PreparedRefresh>(this);
  watcher->setObjectName("projectLayerRefreshWatcher");
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation, sequence, directory, target, stop] {
            const bool cancelled = watcher->future().isCanceled() || stop->load();
            auto result = !cancelled && watcher->future().resultCount()
                                  ? watcher->result()
                                  : PreparedRefresh{};
            watcher->deleteLater();
            const bool current=!cancelled && target && target==m_catalog && target->isOpen() &&
              directory==m_directory && sequence==target->mutationSeq() &&
              generation==m_generation;
            if(current) {
              bool changed=false;
              if(!result.journal.isEmpty()) {
                QString error;
                const QSignalBlocker block(target);
                if(!target->applyJournal(result.journal,&error))result.data.warnings<<error;
                else changed=true;
              }
              // Catalog status changes are committed before consumers see the
              // projection. Block reentrant refreshes until the entire journal lands.
              const auto publishedSequence=target->mutationSeq();
              m_running=false;m_pending=false;
              emit prepared(directory,publishedSequence,result.data);
              if(changed)QMetaObject::invokeMethod(target,"changed",Qt::QueuedConnection);
            } else {
              m_running=false;
              // changed() can arrive before recordOp advances the sequence.
              // A rejected/cancelled result never publishes its old journal.
              if(m_catalog && (m_pending || (!cancelled && generation==m_generation)))
                request(m_catalog,m_directory);
            }
          });
  const auto future = QtConcurrent::run(
      paleo::tasks::pool(m_tasks), [snapshot, directory, stop, boundaries] {
        PreparedRefresh result;result.data = prepare(*snapshot, directory, stop);
        if (!stop->load()) {
          const auto frameworkError = ensureProjectAnalysisScheme(snapshot.get(), directory, boundaries);
          if (!frameworkError.isEmpty()) result.data.warnings << frameworkError;
          const auto error = syncAnalysisResults(snapshot.get(), directory, [stop] { return stop->load(); });
          if (!error.isEmpty()) result.data.warnings << error;
          else if(!stop->load())result.journal=snapshot->journal();
        }
        return result;
      });
  auto *task = paleo::tasks::track(m_tasks, watcher, future,
                                   tr("准备井位与井轨迹"), true);
  if (task)
    connect(task, &PaleoTask::cancelAsked, watcher,
            [stop] { stop->store(true); });
  watcher->setFuture(future);
}
void ProjectLayerRefreshWorkflow::cancel()
{
  ++m_generation; m_pending = false; m_catalog.clear(); m_directory.clear();
  if (m_stop) m_stop->store(true);
}

ProjectLayerRefreshWorkflow::~ProjectLayerRefreshWorkflow() { cancel(); }
