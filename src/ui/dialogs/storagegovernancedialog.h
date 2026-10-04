// 层：视图
#pragma once
#include <QDialog>
#include "../../services/storagegovernance.h"
class QLabel;
class QPushButton;
class QProgressBar;
class QTableView;
class QTabWidget;

class StorageGovernanceDialog : public QDialog {
  Q_OBJECT
public:
  explicit StorageGovernanceDialog(QWidget *parent = nullptr);
  void setReport(const paleo::storage::Report &report);
  void setPreview(const paleo::storage::Preview &preview);
  void setBusy(bool busy, bool cancellable);
  void setProgress(int done, int total, const QString &path);
  void setMessage(const QString &message);
signals:
  void scanRequested();
  void cancelRequested();
  void previewRequested(const QStringList &versions, const QStringList &orphans);
  void confirmRequested();
  void verifyShaRequested();
private:
  QTableView *m_entities, *m_types, *m_orphans, *m_stale;
  QTabWidget *m_tabs;
  QLabel *m_summary, *m_status;
  QPushButton *m_scan, *m_preview, *m_cancel, *m_sha;
  QProgressBar *m_progress;
};
