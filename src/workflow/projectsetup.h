// 层：功能
#pragma once
#include "../domain/projectsetup.h"
#include <QObject>
#include <QPointer>
#include <atomic>
#include <memory>
class FolderImportWorkflow;
class QgisProjectService;
class QgisLayerService;
class PaleoTaskService;
class ProjectSetupWorkflow : public QObject
{
  Q_OBJECT
public:
  ProjectSetupWorkflow(QgisProjectService *project, FolderImportWorkflow *imports, QObject *parent = nullptr);
  ~ProjectSetupWorkflow() override;
  static ProjectSetupSources discover(const QString &root);
  static bool containsProject(const QString &root);
  static bool checkProjectLocation(const QString &path, QString *error);
  void scan(const ProjectSetupSources &sources);
  bool create(const QString &path, const ProjectSetupSources &sources, const ProjectSetupPreview &preview, QString *error);
  bool applySources(const ProjectSetupSources &sources, const ProjectSetupPreview &input, QString *error);
  ProjectSetupSources currentSources() const;
  void setMapImportServices(QgisLayerService *layers, PaleoTaskService *tasks);
signals:
  void previewReady(const ProjectSetupPreview &preview);
  void finished(const QString &summary, bool success);
private:
  void importMaps(const ProjectSetupSources &sources, const ProjectSetupPreview &preview,
                  quint64 session, const QString &summary, bool success, int group = 0);
  QPointer<QgisProjectService> m_project;
  QPointer<FolderImportWorkflow> m_imports;
  QPointer<QgisLayerService> m_mapLayers;
  QPointer<PaleoTaskService> m_mapTasks;
  std::shared_ptr<std::atomic_bool> m_stop;
  quint64 m_generation = 0;
};
