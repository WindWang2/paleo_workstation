// 层：视图
#pragma once
#include "../../domain/projectsetup.h"
#include <QDialog>
#include <QMap>
class ProjectSetupWorkflow;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTableWidget;
class QPlainTextEdit;
class ProjectSetupDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ProjectSetupDialog(ProjectSetupWorkflow *workflow, QWidget *parent = nullptr);
  QString projectPath() const;
  ProjectSetupSources sources() const;
  ProjectSetupPreview preview() const { return m_empty ? ProjectSetupPreview{} : m_preview; }
  void setSourceRoot(const QString &root);
  void setExistingProject(const QString &path, const ProjectSetupSources &sources);
private:
  QWidget *projectPage();
  QWidget *pointPage();
  QWidget *sourcesPage();
  QWidget *reviewPage();
  QWidget *pathRow(QLineEdit *edit, bool directory, const QString &filter = {});
  void discover();
  void scan();
  void showPreview(const ProjectSetupPreview &preview);
  void showStep(int step);
  void finish();
  ProjectSetupWorkflow *m_workflow;
  QLineEdit *m_name, *m_output, *m_root, *m_points, *m_wells, *m_seismic, *m_horizons, *m_references, *m_boundary, *m_crs;
  QMap<QString, QComboBox *> m_fields;
  QStackedWidget *m_pages;
  QTabBar *m_steps;
  QPushButton *m_previous, *m_next;
  QLabel *m_status;
  QTableWidget *m_pointPreview;
  QTableWidget *m_assignments;
  QPlainTextEdit *m_review;
  ProjectSetupPreview m_preview;
  QString m_discoveredRoot, m_lastPointFile;
  QString m_existingPath;
  QVariantMap m_initialMapping;
  quint64 m_revision = 0, m_scanRevision = 0;
  bool m_busy = false, m_empty = false, m_dirty = true;
};
