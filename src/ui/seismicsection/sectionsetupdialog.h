// 层：视图
#pragma once
#include <QDialog>
#include <QStringList>
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
  // 吸附成线/空间重排的井序落盘：ids 按给定序置顶并勾选，其余井保持当前
  // 相对序、置未勾选（sectionsetupdialog_order.cpp）。
  void applyWellOrder(const QStringList &ids);
  // 当前勾选井 id（列表序）。
  QStringList checkedWellIds() const;
signals:
  void buildRequested(const QStringList &ids);
  void drawRequested();
  // 清除地图上的剖面连线（rubber band + 壳侧路线状态）。
  void clearRequested();
  // 「按空间关系重排」：mode = "nearest" | "pca"；ids = 当前勾选井（列表序）。
  void spatialReorderRequested(const QString &mode, const QStringList &ids);
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
