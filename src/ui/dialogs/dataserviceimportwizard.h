// 层：视图
#pragma once
#include "../../domain/projectsetup.h"
#include <QWizard>
class ProjectSetupWorkflow;
class QLineEdit;
class QComboBox;
class QLabel;
class PaleoCrsSelector;

class DataServiceImportWizard : public QWizard
{
  Q_OBJECT
public:
  explicit DataServiceImportWizard(ProjectSetupWorkflow *workflow, QWidget *parent = nullptr);
  void accept() override;
  QString projectPath() const;
  ProjectSetupSources sources() const;
  ProjectSetupPreview preview() const;
  void setSourceRoot(const QString &root);
  void showCreationError(const QString &error);
private:
  QWizardPage *projectPage();
  QWizardPage *settingsPage();
  QWizardPage *reviewPage();
  QWidget *pathRow(QLineEdit *edit, bool directory);
  void refreshReview();
  void finish();
  ProjectSetupWorkflow *m_workflow;
  QLineEdit *m_name, *m_output, *m_root, *m_points, *m_wellField, *m_xField, *m_yField;
  QComboBox *m_crsUnit, *m_threads, *m_pending;
  PaleoCrsSelector *m_crs;
  QLabel *m_status, *m_review;
};
