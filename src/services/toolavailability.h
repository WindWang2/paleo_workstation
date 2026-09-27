// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QHash>

class PaleoProjectStore;

// §35 + eng review — tool gating with per-layer busy map.
// Global gate (e.g. "no project open") AND per-layer gate (task/edit in progress).
// Every denial carries a tr()'d reason string shown in tooltip (§42.13).
class ToolAvailabilityService : public QObject
{
  Q_OBJECT
  public:
    explicit ToolAvailabilityService(PaleoProjectStore *store, QObject *parent = nullptr);

    void setGlobalGate(bool allowed, const QString &reason);  // e.g. project closed
    bool check(const QString &layerId, QString *reasonOut = nullptr) const;
    // check returns false when: global gate closed, or layerId busy (task/edit).

    // Convenience for tools: bind to store's busy registry (shared state).
    void noteTaskOnLayer(const QString &layerId, const QString &taskId, const QString &reason);
    void clearTaskOnLayer(const QString &layerId);

  signals:
    void availabilityChanged(const QString &layerId);

  private:
    PaleoProjectStore *m_store;
    bool m_globalAllowed = true;
    QString m_globalReason;
};
