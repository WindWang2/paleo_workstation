// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QHash>
#include <QPointer>
#include <memory>

class QgsVectorLayer;
class QgsGeometry;
class PaleoProjectStore;
class QgisConstraintEditSession;

// qgis/ — QgisEditingService wraps QgsVectorLayer edit sessions.
// Rules: an edit session marks the layer busy in PaleoProjectStore (tool
// gating shows "editing in progress"); commit goes through store.enqueueWrite
// (single-writer discipline); rollback frees the layer.
class QgisEditingService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisEditingService(PaleoProjectStore *store, QObject *parent = nullptr);
    ~QgisEditingService() override;
    void setUndoDepth(int depth);
    int undoDepth() const { return m_undoDepth; }
    QString availabilityError(const QgsVectorLayer *layer) const;
    static bool isConstraintLayer(const QgsVectorLayer *layer);

    bool beginEdit(QgsVectorLayer *layer, QString *error = nullptr);   // startEditing + markBusy("edit")
    bool commitEdit(QgsVectorLayer *layer, QString *error = nullptr);  // commitChanges via store queue
    bool rollbackEdit(QgsVectorLayer *layer);                          // rollBack + markFree
    bool isEditing(QgsVectorLayer *layer) const;

    // 拓扑提交门（docs/QGIS_NATIVE_ADOPTION.md）：几何经原生验证才允许入
    // edit buffer——QgsGeometryValidator（QgisInternal 引擎）出错文本如实
    // 返回，合法/几何为空时回空串。不静默 makeValid：自动修形的结果必须
    // 由人看见再入库，否则编辑结果与用户笔画脱节。
    static QString geometryCommitError(const QgsGeometry &geometry, const QString &what);

  signals:
    void editStarted(const QString &layerId);
    void editCommitted(const QString &layerId);
    void editRolledBack(const QString &layerId);
    void availabilityChanged();
    void editFailed(const QString &reason);

  private:
    QPointer<PaleoProjectStore> m_store;
    int m_undoDepth = 100;
    QHash<QgsVectorLayer *, std::shared_ptr<QgisConstraintEditSession>> m_constraintSessions;
    QHash<QgsVectorLayer *, QMetaObject::Connection> m_constraintLifetimeConnections;
};
