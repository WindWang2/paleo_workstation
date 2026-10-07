// 层：视图
#pragma once
#include <QDialog>
#include "qgis/qgisprojectservice.h"
#include <QVector>
class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QgsProjectionSelectionWidget;

// 收集参数并发出保存意图，持久化和坐标操作由工程服务负责。
class ProjectMapSettingsDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ProjectMapSettingsDialog(const PaleoProjectFile &configuration, QWidget *parent = nullptr);
  PaleoProjectFile configuration() const;
  void showError(const QString &error);
signals:
  void saveRequested();
private:
  PaleoProjectFile m_original;
  QCheckBox *m_registered = nullptr;
  QCheckBox *m_basemapEnabled = nullptr;
  QgsProjectionSelectionWidget *m_crs = nullptr;
  QVector<QDoubleSpinBox *> m_parameters;
  QLineEdit *m_topo = nullptr;
  QLineEdit *m_hillshade = nullptr;
  QLineEdit *m_provenance = nullptr;
  QLabel *m_error = nullptr;
};
