// 层：功能
#include "projectlayerrefresh.h"
#include "welltrajectorylayer.h"
#include "../catalog/datacatalog.h"
#include "../services/projectdata.h"
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
  auto *watcher = new QFutureWatcher<ProjectLayerData>(this);
  watcher->setObjectName("projectLayerRefreshWatcher");
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, sequence, directory] {
    const auto data = watcher->result(); watcher->deleteLater(); m_running = false;
    if (generation == m_generation && m_catalog && sequence == m_catalog->mutationSeq())
      emit prepared(directory, sequence, data);
    else if (generation == m_generation && m_catalog)
      m_pending = true; // changed() 取序号早于 recordOp，不发布旧快照
    if (m_pending) request(m_catalog, m_directory);
  });
  watcher->setFuture(QtConcurrent::run([snapshot, directory, stop] {
    return prepare(*snapshot, directory, stop);
  }));
}
void ProjectLayerRefreshWorkflow::cancel()
{
  ++m_generation; m_pending = false; m_catalog.clear(); m_directory.clear();
  if (m_stop) m_stop->store(true);
}
