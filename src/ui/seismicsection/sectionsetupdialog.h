// 层：视图
#pragma once
#include <QDialog>
#include <QVariantList>
class QListWidget;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QTableWidget;
class QLineEdit;
class SectionSetupDialog : public QDialog {
  Q_OBJECT
public:
  explicit SectionSetupDialog(QWidget *parent = nullptr);
  void setWells(const QVariantList &wells);
  void setSavedSections(const QVariantList &sections);
  void setMessage(const QString &text);
signals:
  void buildRequested(const QStringList &ids);
  void drawRequested();
  // 清除地图上的剖面连线（rubber band + 壳侧路线状态）。
  void clearRequested();
  void calibrationRequested(const QString &id, bool constant, double velocity,
                            double shift);
  void saveRequested(const QString &name);
  void restoreRequested(const QString &versionId);

private:
  void showWell();
  QListWidget *m_wells;
  QComboBox *m_mode, *m_saved;
  QDoubleSpinBox *m_velocity, *m_shift;
  QLabel *m_status, *m_message;
  QTableWidget *m_samples;
  QLineEdit *m_name;
};
