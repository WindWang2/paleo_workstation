// 层：功能
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>

// linkage/ — SelectionContext is the single selection broadcast hub (§41.3).
// All selection producers (canvas pick, well panel, tree) call setSelection();
// consumers receive selectionChanged(). Re-entrancy guard coalesces echoes:
// a slot that re-sets selection during a broadcast doesn't re-enter — its
// payload merges and fires once at settle.
class SelectionContext : public QObject
{
  Q_OBJECT
  public:
    explicit SelectionContext(QObject *parent = nullptr);

    QStringList selectedIds() const { return m_ids; }
    QString origin() const { return m_origin; }    // who set it: "canvas" | "wellpanel" | "tree" | ...
    QString activeHorizon() const { return m_horizon; }

    void setSelection(const QStringList &ids, const QString &origin);
    void setActiveHorizon(const QString &horizon);
    void clear(const QString &origin);
    bool broadcasting() const { return m_broadcastDepth > 0; }

  signals:
    void selectionChanged(const QStringList &ids, const QString &origin);
    void activeHorizonChanged(const QString &horizon);

  private:
    void settlePending(const char *caller);

    QStringList m_ids;
    QString m_origin, m_horizon;
    int m_broadcastDepth = 0;
    bool m_pending = false;
    QStringList m_pendingIds;
    QString m_pendingOrigin;
};
