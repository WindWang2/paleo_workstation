#include "qgislayerservice.h"

#include "qgisprojectservice.h"

#include <QSet>

#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  // The project service is the authority when present; a null service falls back
  // to the QgsProject singleton so tests/early boot can run without it.
  QgsProject *resolveProject(QgisProjectService *svc)
  {
    if (svc)
      return svc->project();
    return QgsProject::instance();
  }
} // namespace

QgisLayerService::QgisLayerService(QgisProjectService *projectSvc, LayerManifest *manifest, QObject *parent)
  : QObject(parent)
  , m_projectSvc(projectSvc)
  , m_manifest(manifest)
{
}

bool QgisLayerService::declare(const LayerDeclaration &decl, QString *error)
{
  if (decl.layerId.isEmpty())
  {
    setError(error, QStringLiteral("cannot declare a layer with an empty layerId"));
    return false;
  }
  return m_manifest->upsert(decl, error);
}

QgsMapLayer *QgisLayerService::instantiate(const QString &layerId, QString *error)
{
  if (QgsMapLayer *existing = m_instances.value(layerId))
    return existing;

  const QVector<LayerDeclaration> decls = m_manifest->all();
  const LayerDeclaration *decl = nullptr;
  for (const LayerDeclaration &d : decls)
  {
    if (d.layerId == layerId)
    {
      decl = &d;
      break;
    }
  }
  if (!decl)
  {
    setError(error, QStringLiteral("no layer declaration for '%1'").arg(layerId));
    return nullptr;
  }

  QgsProject *proj = resolveProject(m_projectSvc);
  if (!proj)
  {
    setError(error, QStringLiteral("no QgsProject available to host '%1'").arg(layerId));
    return nullptr;
  }

  std::unique_ptr<QgsMapLayer> layer;
  if (decl->type.compare(QStringLiteral("raster"), Qt::CaseInsensitive) == 0)
    layer.reset(new QgsRasterLayer(decl->source, decl->layerId, QStringLiteral("gdal")));
  else
    layer.reset(new QgsVectorLayer(decl->source, decl->layerId, QStringLiteral("ogr")));

  if (!layer->isValid())
  {
    const QString detail = layer->error().message();
    setError(error, QStringLiteral("failed to instantiate layer '%1': %2")
                        .arg(layerId, detail.isEmpty() ? QStringLiteral("provider rejected source '%1'").arg(decl->source) : detail));
    return nullptr;
  }

  // QgsProject takes ownership; keep only the raw pointer in the instance map.
  QgsMapLayer *added = proj->addMapLayer(layer.get());
  if (!added)
  {
    setError(error, QStringLiteral("QgsProject refused layer '%1'").arg(layerId));
    return nullptr;
  }
  layer.release();

  m_instances.insert(layerId, added);
  emit layerInstantiated(layerId);
  return added;
}

int QgisLayerService::instantiateHorizon(const QString &horizon)
{
  int count = 0;
  const QVector<LayerDeclaration> decls = m_manifest->forHorizon(horizon);
  for (const LayerDeclaration &d : decls)
    if (instantiate(d.layerId, nullptr))
      ++count;
  return count;
}

void QgisLayerService::releaseHorizon(const QString &horizon)
{
  // Exact horizon match only: horizon-agnostic ('') declarations are not owned
  // by any horizon and survive the switch.
  QStringList toRelease;
  const QVector<LayerDeclaration> decls = m_manifest->all();
  for (const LayerDeclaration &d : decls)
    if (d.horizon == horizon && m_instances.contains(d.layerId))
      toRelease.append(d.layerId);

  QgsProject *proj = resolveProject(m_projectSvc);
  for (const QString &id : toRelease)
  {
    QgsMapLayer *l = m_instances.take(id);
    if (proj && l)
      proj->removeMapLayer(l); // project-owned: removal deletes the layer
  }
  emit horizonReleased(horizon);
}

QgsMapLayer *QgisLayerService::layer(const QString &layerId) const
{
  return m_instances.value(layerId);
}

bool QgisLayerService::isInstantiated(const QString &layerId) const
{
  return m_instances.contains(layerId);
}

void QgisLayerService::setActiveHorizon(const QString &horizon)
{
  const QVector<LayerDeclaration> decls = m_manifest->all();
  QHash<QString, QString> horizonOf;
  horizonOf.reserve(decls.size());
  for (const LayerDeclaration &d : decls)
    horizonOf.insert(d.layerId, d.horizon);

  QSet<QString> otherHorizons;
  QStringList orphanIds; // instantiated but declaration has since been removed
  for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it)
  {
    const auto hit = horizonOf.constFind(it.key());
    if (hit == horizonOf.constEnd())
      orphanIds.append(it.key());
    else if (!hit.value().isEmpty() && hit.value() != horizon)
      otherHorizons.insert(hit.value());
    // else: target horizon or horizon-agnostic layer stays instantiated
  }

  QgsProject *proj = resolveProject(m_projectSvc);
  for (const QString &id : orphanIds)
  {
    QgsMapLayer *l = m_instances.take(id);
    if (proj && l)
      proj->removeMapLayer(l);
  }
  for (const QString &h : otherHorizons)
    releaseHorizon(h);

  m_activeHorizon = horizon;
  instantiateHorizon(horizon);
}
