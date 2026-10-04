// 层：QGIS 封装
#include "qgiseditingservice.h"
#include "qgisconstrainteditsession.h"
#include <QUndoStack>
#include <QFileInfo>
#include <algorithm>

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
  if (m_store)
    connect(m_store, &PaleoProjectStore::readOnlyChanged, this, &QgisEditingService::availabilityChanged);
}

QgisEditingService::~QgisEditingService()
{
  const auto sessions = m_constraintSessions;
  for (auto it = sessions.cbegin(); it != sessions.cend(); ++it)
    rollbackEdit(it.key());
}

void QgisEditingService::setUndoDepth(int depth)
{
  m_undoDepth = std::clamp(depth, 1, 10000);
}

bool QgisEditingService::isConstraintLayer(const QgsVectorLayer *layer)
{
  return layer && layer->customProperty(QStringLiteral("paleoLayerId")).toString()
      .startsWith(QLatin1String("constraints."));
}

QString QgisEditingService::availabilityError(const QgsVectorLayer *layer) const
{
  if (!m_store || m_store->isReadOnly())
    return tr("工程处于只读模式或 store 不可用");
  if (!layer || !layer->isValid() || layer->readOnly() || !layer->supportsEditing())
    return tr("图层为只读或编辑资产不可用");
  if (isConstraintLayer(layer))
  {
    const QString path = QFileInfo(layer->source().section(QLatin1Char('|'), 0, 0)).canonicalFilePath();
    const QString storePath = QFileInfo(m_store->gpkgPath()).canonicalFilePath();
    if (storePath.isEmpty() || (!m_constraintSessions.contains(const_cast<QgsVectorLayer *>(layer)) && path != storePath))
      return tr("约束编辑资产缺失或未连接到工程 store");
  }
  QString busy;
  if (!layer->isEditable() && m_store->layerBusy(busyKey(layer), &busy))
    return busy;
  return {};
}

bool QgisEditingService::beginEdit(QgsVectorLayer *layer, QString *error)
{
  if (!layer)
  {
    setError(error, tr("cannot begin an edit session on a null layer"));
    return false;
  }
  const QString unavailable = availabilityError(layer);
  if (!unavailable.isEmpty())
  {
    setError(error, unavailable);
    return false;
  }
  if (layer->isEditable())
  {
    setError(error, tr("layer '%1' already has an active edit session").arg(layer->id()));
    return false;
  }
  QString busy;
  if (m_store->layerBusy(busyKey(layer), &busy))
  {
    setError(error, busy);
    return false;
  }
  layer->undoStack()->setUndoLimit(m_undoDepth);
  if (!isConstraintLayer(layer) && !layer->startEditing())
  {
    setError(error, tr("startEditing failed for layer '%1'").arg(layer->id()));
    return false;
  }

  if (isConstraintLayer(layer))
  {
    auto session = std::make_shared<QgisConstraintEditSession>(layer, m_store,
        [this](const QString &reason) { emit editFailed(reason); });
    QString reason;
    if (!session->initialize(&reason))
    {
      layer->rollBack();
      setError(error, reason);
      return false;
    }
    m_constraintSessions.insert(layer, session);
    const QString key = busyKey(layer);
    const QString id = layer->id();
    m_constraintLifetimeConnections.insert(layer, connect(layer, &QObject::destroyed, this, [this, layer, key, id] {
      m_constraintLifetimeConnections.remove(layer);
      const bool hadSession = m_constraintSessions.remove(layer) > 0;
      if (hadSession && m_store)
      {
        m_store->markLayerFree(key);
        emit editRolledBack(id);
      }
    }));
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

  if (!m_store)
  {
    setError(error, tr("编辑 store 已不可用"));
    return false;
  }
  if (auto session = m_constraintSessions.value(layer))
  {
    QString reason;
    if (!session->finish(true, &reason))
    {
      setError(error, reason);
      return false;
    }
    disconnect(m_constraintLifetimeConnections.take(layer));
    m_constraintSessions.remove(layer);
    m_store->markLayerFree(busyKey(layer));
    emit editCommitted(layer->id());
    return true;
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

  // Failed commits retain the session and its busy mark for retry/rollback.
  if (res.ok)
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

  QString error;
  auto session = m_constraintSessions.value(layer);
  const bool ok = session ? session->finish(false, &error) : layer->rollBack();
  if (ok)
  {
    disconnect(m_constraintLifetimeConnections.take(layer));
    m_constraintSessions.remove(layer);
    if (m_store)
      m_store->markLayerFree(busyKey(layer));
  }
  else if (!error.isEmpty())
    emit editFailed(error);
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
