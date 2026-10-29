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
  bool validateCurrentPage() override;
private:
  QWizardPage *projectPage();
  QWizardPage *settingsPage();
  QWizardPage *reviewPage();
  QWidget *pathRow(QLineEdit *edit, bool directory);
  void refreshReview();
  void finish();
  bool validateProjectInputs();
  bool validateCoordinateInputs();
  void showPageError(int page, const QString &error);
  ProjectSetupWorkflow *m_workflow;
  QLineEdit *m_name = nullptr, *m_output = nullptr, *m_root = nullptr, *m_points = nullptr;
  QLineEdit *m_wellField = nullptr, *m_xField = nullptr, *m_yField = nullptr;
  QComboBox *m_crsUnit = nullptr, *m_threads = nullptr, *m_pending = nullptr;
  PaleoCrsSelector *m_crs = nullptr;
  QLabel *m_status = nullptr, *m_coordinateStatus = nullptr, *m_reviewStatus = nullptr, *m_review = nullptr;
};
