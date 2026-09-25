#pragma once
#include <QObject>
#include <QString>

class QgsVectorLayer;
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

  signals:
    void editStarted(const QString &layerId);
    void editCommitted(const QString &layerId);
    void editRolledBack(const QString &layerId);

  private:
    PaleoProjectStore *m_store;
};
