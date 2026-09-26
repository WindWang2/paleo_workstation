#pragma once
#include <QObject>
#include <QString>

class QgsVectorLayer;
class QgsGeometry;
class PaleoProjectStore;

// qgis/ — QgisEditingService wraps QgsVectorLayer edit sessions.
// Rules: an edit session marks the layer busy in PaleoProjectStore (tool
// gating shows "editing in progress"); commit goes through store.enqueueWrite
// (single-writer discipline); rollback frees the layer.
class QgisEditingService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisEditingService(PaleoProjectStore *store, QObject *parent = nullptr);

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

  private:
    PaleoProjectStore *m_store;
};
