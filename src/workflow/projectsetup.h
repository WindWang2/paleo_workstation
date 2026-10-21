// 层：功能
#pragma once
#include "../domain/projectsetup.h"
#include <QObject>
#include <QPointer>
#include <atomic>
#include <memory>
class FolderImportWorkflow;
class QgisProjectService;
class ProjectSetupWorkflow : public QObject
{
  Q_OBJECT
public:
  ProjectSetupWorkflow(QgisProjectService *project, FolderImportWorkflow *imports, QObject *parent = nullptr);
  ~ProjectSetupWorkflow() override;
  static ProjectSetupSources discover(const QString &root);
  static bool containsProject(const QString &root);
  void scan(const ProjectSetupSources &sources);
  bool create(const QString &path, const ProjectSetupSources &sources, const ProjectSetupPreview &preview, QString *error);
  bool applySources(const ProjectSetupSources &sources, const ProjectSetupPreview &preview, QString *error);
  ProjectSetupSources currentSources() const;
signals:
  void previewReady(const ProjectSetupPreview &preview);
  void finished(const QString &summary, bool success);
private:
  QPointer<QgisProjectService> m_project;
  QPointer<FolderImportWorkflow> m_imports;
  std::shared_ptr<std::atomic_bool> m_stop;
  quint64 m_generation = 0;
};
