// 层：视图
#pragma once
#include <QDialog>
#include <QVariantMap>
class QLabel;
class QTableWidget;

// facies-ux 4：地震相 × 测井相预测一致性报告（只展示；数据由
// MappingWorkbench::faciesConsistency 生成）。非模态，可与画布并看。
class FaciesConsistencyDialog : public QDialog {
  Q_OBJECT
public:
  explicit FaciesConsistencyDialog(QWidget *parent = nullptr);
  void setReport(const QVariantMap &report);
signals:
  // 逐井表选中某井 → 画布定位（x, y 为测井相图层坐标）。
  void wellActivated(const QString &wellId, double x, double y);

private:
  QLabel *m_summary;
  QLabel *m_path;
  QTableWidget *m_wells;
  QTableWidget *m_matrix;
  QVariantMap m_report;
};
