#include "qgiseditingservice.h"

#include "../metadata/paleoprojectstore.h"

#include <qgsgeometry.h>
#include <qgsgeometryvalidator.h>
#include <qgsvectorlayer.h>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  // Validation and tool gating key layers by manifest id. QgsMapLayer::id()
  // is a generated "name_uuid" and never matches. Layers instantiated by
  // QgisLayerService stamp paleoLayerId; bare test layers keep layer->id().
  QString busyKey(const QgsVectorLayer *layer)
  {
    const QString stamped = layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    return stamped.isEmpty() ? layer->id() : stamped;
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

  m_store->markLayerBusy(busyKey(layer), QStringLiteral("edit"), tr("editing in progress"));
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
  m_store->markLayerFree(busyKey(layer));

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
  m_store->markLayerFree(busyKey(layer)); // freed regardless of rollBack outcome
  if (ok)
    emit editRolledBack(layer->id());
  return ok;
}

bool QgisEditingService::isEditing(QgsVectorLayer *layer) const
{
  return layer && layer->isEditable();
}

QString QgisEditingService::geometryCommitError( const QgsGeometry &geometry,
                                                 const QString &what )
{
  if ( geometry.isNull() )
    return tr( "%1 is empty" ).arg( what );
  // 原生验证器（QgisInternal 引擎，与 QGIS app 的「检查几何有效性」同源）。
  // 空几何（如点要素未成形前的空 QgsGeometry）在 native 语义里不是非法——
  // isNull 已在上面如实体指认；这里只管拓扑违例。
  QVector<QgsGeometry::Error> errors;
  QgsGeometryValidator::validateGeometry( geometry, errors );
  if ( errors.isEmpty() )
    return QString();
  const QgsGeometry::Error &first = errors.constFirst();
  QString where;
  const QgsPointXY w = first.where();
  if ( !std::isnan( w.x() ) )
    where = tr( " at (%1, %2)" ).arg( w.x(), 0, 'f', 2 ).arg( w.y(), 0, 'f', 2 );
  return tr( "%1 is invalid: %2%3" ).arg( what, first.what(), where );
}
