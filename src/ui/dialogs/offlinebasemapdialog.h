// 层：视图
#pragma once
#include <QDialog>
class QLineEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QgisProjectService;
class QgisLayerService;
class PaleoTaskService;
class OfflineBasemapDialog : public QDialog {
  Q_OBJECT
public:
  explicit OfflineBasemapDialog(bool registered, QWidget *parent = nullptr);
  QString path() const;
  bool hillshade() const;
  bool display() const;
  void setBusy(bool busy);
  void showError(const QString &error);
signals:
  void importRequested();
private:
  QLineEdit *m_path;
  QComboBox *m_role;
  QCheckBox *m_display;
  QLabel *m_error;
  QPushButton *m_import;
  QProgressBar *m_progress;
};
void openOfflineBasemapImport(QgisProjectService *project, QgisLayerService *layers,
                              PaleoTaskService *tasks, QWidget *parent);
