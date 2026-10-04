// 层：功能
#pragma once
#include "../services/storagegovernance.h"
#include "../services/cataloghealth.h"
#include "assetops.h"
#include <QObject>
#include <QFuture>
#include <QPointer>

// No widgets. Live catalog stays on owner thread; workers receive values only.
class StorageGovernanceController : public QObject {
  Q_OBJECT
public:
  explicit StorageGovernanceController(QObject *parent = nullptr);
  ~StorageGovernanceController() override;
  void setCatalog(DataCatalog *catalog);
  bool busy() const { return m_busy; }
  bool cancellable() const { return m_cancellable; }
public slots:
  void scan();
  void cancel();
  void requestPreview(const QStringList &versions, const QStringList &orphans);
  void confirmPreview();
  void verifySha();
  void healthScan(const QStringList &recycleAssetIds);
signals:
  void reportReady(const paleo::storage::Report &report);
  void previewReady(const paleo::storage::Preview &preview);
  void progress(int done, int total, const QString &path);
  void stateChanged(bool busy, bool cancellable);
  void message(const QString &message);
  void cleanupFinished(const paleo::assetops::PurgeOutcome &outcome);
  void healthReady(const paleo::health::HealthReport &report, qint64 recycleBytes, bool complete);
  void shaReady(const QVector<paleo::health::HealthIssue> &issues, bool complete);
private:
  bool current(const paleo::storage::Snapshot &source) const;
  void setBusy(bool busy, bool cancellable = true);
  void commit(const paleo::storage::Preview &preview);
  QPointer<DataCatalog> m_catalog;
  QMetaObject::Connection m_changed;
  paleo::storage::Report m_report;
  paleo::storage::Preview m_preview;
  QFuture<void> m_work;
  bool m_busy = false;
  bool m_cancellable = true;
  quint64 m_generation = 0;
};
