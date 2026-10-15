// 层：功能
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QMetaType>

// Business identities only. QGIS FIDs deliberately do not cross this boundary.
struct SelectionTarget
{
    QString projectId, horizon, entityId, assetId, versionId, layerId,
        featureUid, evidenceId, assertionId, interpretationId;
    quint64 revision = 0;
    bool operator==(const SelectionTarget &) const = default;
    bool isEmpty() const { return *this == SelectionTarget{}; }
};
Q_DECLARE_METATYPE(SelectionTarget)

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
    SelectionTarget target() const { return m_target; }
    quint64 targetGeneration() const { return m_targetGeneration; }
    QString targetOrigin() const { return m_targetOrigin; }

    void setSelection(const QStringList &ids, const QString &origin);
    void setActiveHorizon(const QString &horizon);
    void clear(const QString &origin);
    void setTarget(const SelectionTarget &target, const QString &origin);
    void clearTarget(const QString &origin);
    bool broadcasting() const { return m_broadcastDepth > 0; }

  signals:
    void selectionChanged(const QStringList &ids, const QString &origin);
    void activeHorizonChanged(const QString &horizon);
    void targetChanged(const SelectionTarget &target, const QString &origin,
                       quint64 generation);

  private:
    void settlePending(const char *caller);
    void broadcastHorizon(const QString &horizon);

    QStringList m_ids;
    QString m_origin, m_horizon;
    int m_broadcastDepth = 0;
    bool m_pending = false;
    QStringList m_pendingIds;
    QString m_pendingOrigin;
    SelectionTarget m_target, m_pendingTarget;
    QString m_targetOrigin, m_pendingTargetOrigin;
    quint64 m_targetGeneration = 0;
    bool m_targetPending = false;
    QString m_pendingHorizon;
    bool m_horizonPending = false;
};
