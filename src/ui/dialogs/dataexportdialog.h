// 层：视图
#pragma once
#include <QDialog>
#include <QPointer>
#include "../../domain/exportrequest.h"
class DataCatalog;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QTableWidget;
class QLabel;
class QPushButton;
class QTimer;
class PaleoTask;
class PaleoTaskService;
class DataExportDialog : public QDialog {
  Q_OBJECT
public:
  DataExportDialog(DataCatalog *catalog, const QStringList &assetIds, PaleoTaskService *tasks, QWidget *parent = nullptr, const QStringList &selectedVersions = {});
  ~DataExportDialog() override;
  paleo::exporting::DataRequest request() const;
private:
  void refreshVersions();
  void schedulePreview();
  void updateAvailability();
  // L5.4：行内取不到版本 id 的资产显示名（RAW scope 下没有 RAW 版本的行）。
  QStringList rowsMissingVersion() const;
  // L5.4：汇总文案出口——缺 RAW 版本的提示常驻，不被预览/检查文案覆盖。
  void applySummary(const QString &base);
  DataCatalog *m_catalog;
  PaleoTaskService *m_tasks;
  QPointer<PaleoTask> m_previewTask;
  QTimer *m_previewTimer;
  QLabel *m_summary;
  QPushButton *m_save;
  quint64 m_generation = 0;
  bool m_ready = false;
  QStringList m_assetIds, m_selectedVersions;
  QComboBox *m_scope, *m_format;
  QLineEdit *m_directory;
  QCheckBox *m_references;
  QTableWidget *m_versions;
};
