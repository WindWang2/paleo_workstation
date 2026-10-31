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
#include <QTemporaryDir>
#include <QtConcurrent>

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
  auto *watcher = new QFutureWatcher<ProjectLayerData>(this);
  watcher->setObjectName("projectLayerRefreshWatcher");
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, generation, sequence, directory] {
            const bool cancelled = watcher->future().isCanceled();
            const auto data = !cancelled && watcher->future().resultCount()
                                  ? watcher->result()
                                  : ProjectLayerData{};
            watcher->deleteLater();
            m_running = false;
            if (!cancelled && generation == m_generation && m_catalog &&
                sequence == m_catalog->mutationSeq())
              emit prepared(directory, sequence, data);
            else if (!cancelled && generation == m_generation && m_catalog)
              m_pending = true; // changed() 取序号早于 recordOp，不发布旧快照
            if (m_pending)
            {
              // #415：并入的请求拿到的仍是当前目录最新快照（同目录、序号未变）
              // ——直接交付在飞结果，不再整轮 staging 拷贝 + 重解析。
              if (!cancelled && m_catalog && m_catalog->isOpen() &&
                  directory == m_directory && sequence == m_catalog->mutationSeq())
              {
                m_pending = false;
                emit prepared(directory, sequence, data);
              }
              else
                request(m_catalog, m_directory);
            }
          });
  const auto future = QtConcurrent::run(
      paleo::tasks::pool(m_tasks), [snapshot, directory, stop, boundaries] {
        auto data = prepare(*snapshot, directory, stop);
        if (!stop->load()) {
          const auto frameworkError = ensureProjectAnalysisScheme(snapshot.get(), directory, boundaries);
          if (!frameworkError.isEmpty()) data.warnings << frameworkError;
          const auto error = syncAnalysisResults(snapshot.get(), directory, [stop] { return stop->load(); });
          if (!error.isEmpty()) data.warnings << error;
        }
        return data;
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
