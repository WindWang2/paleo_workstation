#include "qgiseditingservice.h"

#include "../metadata/paleoprojectstore.h"

#include <qgsvectorlayer.h>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }
} // namespace

// qgis/ — QgisEditingService wraps QgsVectorLayer edit sessions.
// Rules: an edit session marks the layer busy in PaleoProjectStore (tool
// gating shows "editing in progress"); commit goes through store.enqueueWrite
// (single-writer discipline, §41.2); rollback frees the layer.

QgisEditingService::QgisEditingService(PaleoProjectStore *store, QObject *parent)
  : QObject(parent)
  , m_store(store)
{
}

bool QgisEditingService::beginEdit(QgsVectorLayer *layer, QString *error)
{
  if (!layer)
  {
    setError(error, tr("cannot begin an edit session on a null layer"));
    return false;
  }
  if (layer->isEditable())
  {
    setError(error, tr("layer '%1' already has an active edit session").arg(layer->id()));
    return false;
  }
  if (!layer->startEditing())
  {
    setError(error, tr("startEditing failed for layer '%1'").arg(layer->id()));
    return false;
  }

  m_store->markLayerBusy(layer->id(), QStringLiteral("edit"), tr("editing in progress"));
  emit editStarted(layer->id());
  return true;
}

bool QgisEditingService::commitEdit(QgsVectorLayer *layer, QString *error)
{
  if (!layer)
  {
    setError(error, tr("cannot commit an edit session on a null layer"));
    return false;
  }

  // §41.2 single-writer discipline: the provider flush runs inside the store's
  // serialized write queue, never directly.
  const PaleoProjectStore::WriteResult res = m_store->enqueueWrite(
    [layer]() -> PaleoProjectStore::WriteResult
    {
      if (!layer->isEditable())
        return {false, QObject::tr("layer '%1' has no active edit session").arg(layer->id())};
      if (!layer->commitChanges())
        return {false, QObject::tr("commitChanges failed for layer '%1'").arg(layer->id())};
      return {true, QString()};
    });

  // Freed on success AND on failure: a stale busy mark would gate the layer
  // out of every tool permanently ("editing in progress" must not stick).
  m_store->markLayerFree(layer->id());

  if (!res.ok)
  {
    setError(error, res.error);
    return false;
  }
  emit editCommitted(layer->id());
  return true;
}

bool QgisEditingService::rollbackEdit(QgsVectorLayer *layer)
{
  if (!layer)
    return false;

  const bool ok = layer->rollBack();
  m_store->markLayerFree(layer->id()); // freed regardless of rollBack outcome
  if (ok)
    emit editRolledBack(layer->id());
  return ok;
}

bool QgisEditingService::isEditing(QgsVectorLayer *layer) const
{
  return layer && layer->isEditable();
}
