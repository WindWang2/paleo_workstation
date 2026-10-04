// 层：功能
#include "storagegovernancecontroller.h"
#include "../catalog/purgelease.h"
#include <QFutureWatcher>
#include <QFileInfo>
#include <QPromise>
#include <QtConcurrent>

StorageGovernanceController::StorageGovernanceController(QObject *parent) : QObject(parent) {}
StorageGovernanceController::~StorageGovernanceController() { m_work.cancel(); }
void StorageGovernanceController::setCatalog(DataCatalog *cat) {
  if (m_catalog == cat) return;
  cancel(); ++m_generation;
  disconnect(m_changed);
  m_catalog = cat; m_report = {}; m_preview = {};
  if (cat) m_changed = connect(cat, &DataCatalog::changed, this, [this] {
    ++m_generation; m_report.complete = false; m_preview.valid = false;
    if (m_cancellable) cancel();
  });
}
bool StorageGovernanceController::current(const paleo::storage::Snapshot &s) const {
  return m_catalog && m_catalog->isOpen() && m_catalog->catalogPath() == s.catalogPath
    && m_catalog->mutationSeq() == s.mutationSeq;
}
void StorageGovernanceController::setBusy(bool busy, bool cancellable) {
  m_busy = busy; m_cancellable = cancellable;
  emit stateChanged(busy, cancellable);
}
void StorageGovernanceController::cancel() {
  if (m_cancellable) m_work.cancel();
  m_preview.valid = false;
}
void StorageGovernanceController::scan() {
  if (m_busy || !m_catalog || !m_catalog->isOpen()) return;
  const auto source = paleo::storage::snapshot(m_catalog);
  const auto generation = m_generation;
  m_preview = {}; m_report = {};
  setBusy(true);
  auto *watcher = new QFutureWatcher<paleo::storage::Report>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, watcher](int n) {
    emit progress(n, watcher->progressMaximum(), watcher->progressText());
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, source, generation] {
    const bool cancelled = watcher->future().isCanceled();
    if (!cancelled && watcher->future().resultCount() && generation == m_generation && current(source))
      m_report = watcher->result();
    else { m_report = {}; m_report.source = source; }
    watcher->deleteLater(); setBusy(false);
    emit reportReady(m_report);
    emit message(m_report.complete ? tr("存储扫描完成；SHA 尚未复验。")
                                   : tr("扫描未完成或结果已过期；回收不可用，请重新扫描。"));
  });
  const auto future = QtConcurrent::run([source](QPromise<paleo::storage::Report> &promise) {
    const auto progress = [&promise](int n, int total, const QString &path) {
      // Check cancellation at every entry, publish bounded progress updates.
      if (n == 0 || n == total || n % 128 == 0) {
        promise.setProgressRange(0, qMax(n, total));
        promise.setProgressValueAndText(n, path);
      }
      return !promise.isCanceled();
    };
    promise.addResult(paleo::storage::scan(source, progress));
  });
  m_work = future; watcher->setFuture(future);
}
void StorageGovernanceController::requestPreview(const QStringList &versions, const QStringList &orphans) {
  if (m_busy) return;
  m_preview = paleo::storage::preview(m_report, versions, orphans);
  if (!current(m_preview.source)) {
    m_preview.valid = false; m_preview.blocked << tr("目录已变化，请重新扫描。");
  }
  emit previewReady(m_preview);
}
void StorageGovernanceController::confirmPreview() {
  if (m_busy || !m_preview.valid || !current(m_preview.source)) {
    emit message(tr("预览已过期，请重新扫描并预览。")); return;
  }
  const auto plan = m_preview;
  const auto generation = m_generation;
  m_preview.valid = false;
  setBusy(true);
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, watcher](int n) {
    emit progress(n, watcher->progressMaximum(), watcher->progressText());
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, plan, generation] {
    const bool cancelled = watcher->future().isCanceled();
    const QString error = !cancelled && watcher->future().resultCount() ? watcher->result() : tr("已取消，未执行任何回收动作。");
    watcher->deleteLater();
    if (!error.isEmpty() || generation != m_generation || !current(plan.source)) {
      setBusy(false); emit message(error.isEmpty() ? tr("目录已变化，未执行回收。") : error); return;
    }
    commit(plan);
  });
  const auto future = QtConcurrent::run([plan](QPromise<QString> &promise) {
    QString error;
    paleo::storage::validate(plan, [&promise](int n, int total, const QString &path) {
      promise.setProgressRange(0, qMax(n, total)); promise.setProgressValueAndText(n, path);
      return !promise.isCanceled();
    }, &error);
    promise.addResult(error);
  });
  m_work = future; watcher->setFuture(future);
}
void StorageGovernanceController::commit(const paleo::storage::Preview &p) {
  setBusy(true, false); // Atomic catalog commit begins; cancellation is no longer offered.
  QString error;
  QStringList adopted;

  // Orphans get explicit unknown catalog identities before passing the same
  // removal contract as assets. No file semantics are inferred or file unlinked here.
  DataCatalog::BatchSave batch(m_catalog);
  if (!p.versionIds.isEmpty() && !m_catalog->removeStaleVersions(p.versionIds, &error)) {
    batch.abort(); setBusy(false); emit message(error); return;
  }
  for (const auto &f : p.orphanFiles) {
    CatalogAsset a; a.id = m_catalog->nextAssetId(); a.type = QStringLiteral("unknown");
    a.displayName = f.relativePath;
    CatalogVersion v; v.id = m_catalog->nextVersionId(); v.assetId = a.id;
    // A temporary reference identity avoids repeating managed-path stat in the
    // owner loop. Worker validation already attested containment and identity;
    // this record is removed in the same batch, never exposed as an asset.
    v.stage = QStringLiteral("INTERMEDIATE"); v.managed = false; v.path = f.canonicalPath;
    v.fileName = QFileInfo(f.relativePath).fileName();
    v.extra.insert(QStringLiteral("governanceUnreferenced"), true);
    if (!m_catalog->addAsset(a, &error) || !m_catalog->addVersion(v, &error)) {
      // Each path was validated and catalog baseline checked; a readonly/save
      // failure is surfaced by flush and no filesystem cleanup is scheduled.
      batch.abort(); setBusy(false); emit message(error); return;
    }
    adopted << a.id;
  }
  if (!adopted.isEmpty() && !m_catalog->removeAssets(adopted, &error)) {
    batch.abort(); setBusy(false); emit message(error); return;
  }
  if (!batch.flush(&error)) { setBusy(false); emit message(error); return; }
  const auto lease = CatalogPurgeLease::acquire(m_catalog->catalogPath());
  if (!lease) { setBusy(false); emit message(tr("回收文件阶段未启动，请重新扫描残留文件。")); return; }
  emit message(tr("目录已提交，正在回收文件；此阶段不能取消。"));
  const auto retained = m_catalog->versions();
  QStringList paths;
  QVector<paleo::assetops::PurgeFileExpectation> expected;
  for (const auto &f : p.files) {
    const QString path = QDir(p.source.projectDir).filePath(f.relativePath);
    paths << path; expected.append({path, f.sizeBytes, f.modified});
  }
  auto *watcher = new QFutureWatcher<paleo::assetops::PurgeOutcome>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
    const auto outcome = watcher->result(); watcher->deleteLater();
    setBusy(false); m_report.complete = false;
    emit cleanupFinished(outcome);
  });
  const auto future = QtConcurrent::run([dir = p.source.projectDir, paths, retained, expected, lease] {
    return paleo::assetops::purgeManagedFiles(dir, paths, retained, expected);
  });
  m_work = future; watcher->setFuture(future);
}
void StorageGovernanceController::verifySha() {
  if (m_busy || !m_catalog) return;
  const auto source = paleo::storage::snapshot(m_catalog);
  setBusy(true);
  auto *watcher = new QFutureWatcher<QVector<paleo::health::HealthIssue>>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, watcher](int n) {
    emit progress(n, watcher->progressMaximum(), watcher->progressText());
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, source] {
    const bool complete = !watcher->future().isCanceled() && current(source);
    const auto issues = watcher->future().resultCount() ? watcher->result() : QVector<paleo::health::HealthIssue>();
    watcher->deleteLater(); setBusy(false); emit shaReady(issues, complete);
  });
  const auto future = QtConcurrent::run([source](QPromise<QVector<paleo::health::HealthIssue>> &promise) {
    promise.addResult(paleo::health::verifyExternalShas(source.versions,
      [&promise](int n, int total, const QString &path) {
        promise.setProgressRange(0, total); promise.setProgressValueAndText(n, path);
        return !promise.isCanceled();
      }, [&promise] { return promise.isCanceled(); }));
  });
  m_work = future; watcher->setFuture(future);
}

void StorageGovernanceController::healthScan(const QStringList &recycleAssetIds) {
  if (m_busy || !m_catalog || !m_catalog->isOpen()) return;
  const auto source = paleo::storage::snapshot(m_catalog);
  const auto health = paleo::health::healthSnapshot(m_catalog);
  setBusy(true);
  using Result = QPair<paleo::health::HealthReport, qint64>;
  auto *watcher = new QFutureWatcher<Result>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, watcher](int n) {
    emit progress(n, watcher->progressMaximum(), watcher->progressText());
  });
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, source] {
    const bool complete = !watcher->future().isCanceled() && current(source);
    const auto result = watcher->future().resultCount() ? watcher->result() : Result();
    watcher->deleteLater(); setBusy(false); emit healthReady(result.first, result.second, complete);
  });
  const auto future = QtConcurrent::run([source, health, recycleAssetIds](QPromise<Result> &promise) {
    const auto report = paleo::health::buildCatalogHealth(health, source.projectDir,
      [&promise](int n, int total, const QString &path) {
        promise.setProgressRange(0, total); promise.setProgressValueAndText(n, path);
        return !promise.isCanceled();
      });
    qint64 bytes = 0;
    const QSet<QString> removed(recycleAssetIds.begin(), recycleAssetIds.end());
    for (const auto &v : source.versions) {
      if (promise.isCanceled()) return;
      if (v.managed && removed.contains(v.assetId)) {
        const QString path = DataCatalog::resolvedVersionPath(source.projectDir, v);
        if (!path.isEmpty()) bytes += QFileInfo(path).size();
      }
    }
    promise.addResult(Result(report, bytes));
  });
  m_work = future; watcher->setFuture(future);
}
