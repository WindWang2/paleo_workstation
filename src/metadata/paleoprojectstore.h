#pragma once
#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QHash>
#include <QPair>
#include <QString>
#include <functional>
#include <atomic>

// §41.2 — sole write choke point for project.gpkg / metadata.sqlite / .qgz.
// All writes enqueue here; ordering contract per save: gpkg commit -> .qgz backup ->
// .qgz atomic write (temp+rename). .qgz failure after gpkg commit is recoverable
// (display state regenerable); gpkg failure aborts the sequence.
// Mechanism (not just discipline): callers route through enqueueWrite(); a write
// mutex serializes actual file mutation. Processing/GDAL outputs must go
// temp-then-merge (service boundary), never direct-to-gpkg inside task code.
class PaleoProjectStore : public QObject
{
  Q_OBJECT
  public:
    struct WriteResult { bool ok; QString error; };

    explicit PaleoProjectStore(QObject *parent = nullptr);

    void setProjectPaths(const QString &qgzPath, const QString &gpkgPath, const QString &metaSqlitePath);
    QString gpkgPath() const { return m_gpkgPath; }
    QString metaDbPath() const { return m_metaPath; }

    // Serialized write: fn runs while holding the write mutex. Thread-safe enqueue.
    WriteResult enqueueWrite(const std::function<WriteResult()> &fn);

    // Whole-save sequence per contract ordering. Backs up .qgz before rewriting.
    WriteResult saveAll(const std::function<WriteResult()> &gpkgCommit,
                        const std::function<WriteResult()> &writeQgz);

    // Per-layer busy registry shared with ToolAvailabilityService (§35).
    void markLayerBusy(const QString &layerId, const QString &taskId, const QString &reason);
    void markLayerFree(const QString &layerId);
    bool layerBusy(const QString &layerId, QString *reason = nullptr) const;

  signals:
    void writeCompleted(const QString &target);
    void writeFailed(const QString &target, const QString &error);

  private:
    QString m_qgzPath, m_gpkgPath, m_metaPath;
    mutable QMutex m_writeMutex;
    QHash<QString, QPair<QString, QString>> m_busy; // layerId -> (taskId, reason)
    mutable QMutex m_busyMutex;
};
