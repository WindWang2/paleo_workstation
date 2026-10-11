// 层：QGIS 封装
#include "qgislayerservice.h"
#include "factorstylewriter.h"
#include "qgiserrors_internal.h"

#include "qgisprojectservice.h"
#include "qgiseditingservice.h"
#include "mappingartifactwriter.h"
#include "projectmapreference.h"

#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

namespace
{
using paleo::qgis_detail::setError;

  // The project service is the authority when present; a null service falls back
  // to the QgsProject singleton so tests/early boot can run without it.
  QgsProject *svcProject(QgisProjectService *svc)
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
  // m_instances caches raw QgsMapLayer* owned by the project. QgsProject
  // clear()/read() and any removeMapLayer() path delete layers without asking —
  // every removal signal drops the corresponding cache entries so the hash can
  // never go dangling (instantiate() would otherwise hand out freed pointers).
  QgsProject *proj = svcProject(m_projectSvc);
  if (proj)
  {
    connect(proj, &QgsProject::cleared, this, [this] { m_instances.clear(); });
    connect(proj, &QgsProject::layersRemoved, this,
            [this](const QStringList &removedIds) {
              for (auto it = m_instances.begin(); it != m_instances.end();)
              {
                // layersRemoved fires before the layers are deleted — id() is
                // still readable here; destroyed() covers the rest.
                if (!it.value() || removedIds.contains(it.value()->id()))
                  it = m_instances.erase(it);
                else
                  ++it;
              }
            });
  }
  // openProject()/createProject() swap the layer set wholesale (read() clears
  // first, but be explicit): drop every entry not still registered, then adopt
  // the layers read() restored from the .qgz (see adoptProjectLayers).
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this] {
              purgeDanglingInstances();
              adoptProjectLayers();
            });
}

bool QgisLayerService::declare(const LayerDeclaration &decl, QString *error)
{
  if (decl.layerId.isEmpty())
  {
    setError(error, QStringLiteral("cannot declare a layer with an empty layerId"));
    return false;
  }
  QgsMapLayer *previous = m_instances.value(decl.layerId).data();
  const bool replace = previous && (decl.type == QLatin1String("mbtiles")
      ? previous->customProperty("paleoBasemapPath").toString() != QFileInfo(QDir(QFileInfo(svcProject(m_projectSvc)->fileName()).absolutePath()).filePath(decl.source)).absoluteFilePath()
      : previous->source() != decl.source);
  if (replace)
  {
    if (auto *vector = qobject_cast<QgsVectorLayer *>(previous))
    {
      if (vector->isEditable())
      {
        setError(error, tr("请先保存或取消该图层的编辑，再替换图件"));
        return false;
      }
    }
  }
  if (!m_manifest->upsert(decl, error))
    return false;
  if (replace)
  {
    if (auto *project = svcProject(m_projectSvc))
      project->removeMapLayer(previous->id());
    // previous 此后可能已被 QgsProject 删除——不得再解引用。重建失败不回滚
    // 声明（清单已是新 source），但不能把失败文案塞进「成功」返回的 error，
    // 记 warning 留痕。
    QString reErr;
    if (!instantiate(decl.layerId, &reErr))
      qWarning("QgisLayerService::declare: re-instantiating '%s' after source change failed: %s",
               qPrintable(decl.layerId), qPrintable(reErr));
  }
  emit layerDeclared(decl.layerId);
  return true;
}

QgsMapLayer *QgisLayerService::instantiate(const QString &layerId, QString *error)
{
  if (QgsMapLayer *existing = m_instances.value(layerId).data())
  {
    // Paranoia guard: compare pointer identity against the project's live set
    // without dereferencing — a layer destroyed between signal deliveries must
    // never be handed back as live.
    QgsProject *proj = svcProject(m_projectSvc);
    const auto live = proj ? proj->mapLayers().values() : QList<QgsMapLayer *>();
    if (live.contains(existing))
      return existing;
    m_instances.remove(layerId); // stale entry — fall through and re-create
  }
  else
  {
    m_instances.remove(layerId); // purge null QPointer entry
  }

  // A manifest read failure is a READ failure, not "no declaration" — report
  // the store error so callers don't misdiagnose a corrupt/unreadable manifest
  // as an undeclared layer.
  QVector<LayerDeclaration> decls;
  QString readError;
  if (!m_manifest->readAll(&decls, &readError))
  {
    setError(error, readError.isEmpty()
                        ? QStringLiteral("failed to read layer manifest")
                        : QStringLiteral("failed to read layer manifest: %1").arg(readError));
    return nullptr;
  }
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

  QgsProject *proj = svcProject(m_projectSvc);
  if (!proj)
  {
    setError(error, QStringLiteral("no QgsProject available to host '%1'").arg(layerId));
    return nullptr;
  }

  std::unique_ptr<QgsMapLayer> layer;
  if (decl->type == QLatin1String("mbtiles"))
    layer.reset(paleo::mapreference::offlineBasemap(
        QDir(QFileInfo(proj->fileName()).absolutePath()).filePath(decl->source), decl->title));
  else if (decl->type.compare(QStringLiteral("raster"), Qt::CaseInsensitive) == 0)
    layer.reset(new QgsRasterLayer(decl->source, decl->layerId, QStringLiteral("gdal")));
  else
    layer.reset(new QgsVectorLayer(decl->source, decl->layerId, QStringLiteral("ogr")));

  if (!layer || !layer->isValid())
  {
    const QString detail = layer ? layer->error().message() : tr("离线底图不存在或无法读取");
    setError(error, QStringLiteral("failed to instantiate layer '%1': %2")
                        .arg(layerId, detail.isEmpty() ? QStringLiteral("provider rejected source '%1'").arg(decl->source) : detail));
    return nullptr;
  }

  if (decl->type != QLatin1String("mbtiles")) MappingArtifactWriter::restoreRasterCrs(layer.get());

  // paleoLayerId/显示名在 addMapLayer 之前落——legendLayersAdded 在注册期
  // 即发（图层树布局器此刻归位，须已能认出声明归属）。
  if (!decl->title.isEmpty())
    layer->setName(decl->title); // 显示名优先 title，机器名仍在 paleoLayerId
  layer->setCustomProperty(QStringLiteral("paleoLayerId"), decl->layerId);

  if (decl->layerId.startsWith(QStringLiteral("contours.")) ||
      (decl->layerId.startsWith(QStringLiteral("cartographic.")) && decl->layerId.endsWith(QStringLiteral(".contours"))))
    FactorStyleWriter::applyContours(qobject_cast<QgsVectorLayer *>(layer.get()));

  // QgsProject takes ownership; keep only the raw pointer in the instance map.
  QgsMapLayer *added = proj->addMapLayer(layer.get());
  if (!added)
  {
    setError(error, QStringLiteral("QgsProject refused layer '%1'").arg(layerId));
    return nullptr;
  }
  layer.release();
  // 主线5：创建时间元数据——首次实例化时刻落图层自定义属性（QGIS 随 .qgz
  // 持久化；复用实例不刷新时间）。属性面板业务字段「创建时间」读此值。
  if (!added->customProperty(QStringLiteral("paleoCreatedAt")).isValid())
    added->setCustomProperty(
        QStringLiteral("paleoCreatedAt"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

  trackInstance(layerId, added);
  emit layerInstantiated(layerId);
  return added;
}

bool QgisLayerService::removeDeclaration(const QString &layerId, QString *error)
{
  // 与 declare() 替换路径同口径：编辑中的层不可静默销毁——未提交的编辑
  // 缓冲会直接丢失，且编辑会话的 busy 标记无人释放（Issue #27 同族）。
  if (auto *project = svcProject(m_projectSvc))
    for (auto *layer : project->mapLayers())
      if (layer->customProperty("paleoLayerId").toString() == layerId)
        if (auto *vector = qobject_cast<QgsVectorLayer *>(layer); vector && vector->isEditable())
        {
          setError(error, tr("请先保存或取消该图层的编辑，再移除图件"));
          return false;
        }
  if (!m_manifest->remove(layerId, error)) return false;
  if (auto *project = svcProject(m_projectSvc))
    for (auto *layer : project->mapLayers())
      if (layer->customProperty("paleoLayerId").toString() == layerId)
        project->removeMapLayer(layer);
  emit layerDeclared(layerId);
  return true;
}

void QgisLayerService::refreshBasemaps()
{
  if (!m_projectSvc || m_projectSvc->projectPath().isEmpty()) return;
  const auto &config = m_projectSvc->mapConfiguration();
  const bool enabled = config.basemapEnabled && config.georeference &&
      m_projectSvc->project()->crs().type() != Qgis::CrsType::Engineering;
  for (const auto &id : {QStringLiteral("basemap.hillshade"), QStringLiteral("basemap.topo")}) {
    const auto source = id.endsWith("topo") ? config.basemapTopo : config.basemapHillshade;
    // 配准缺席时也卸下 .qgz 已保存的底图，避免把投影数据混进局部坐标。
    if (!enabled || source.isEmpty()) {
      if (!m_manifest->isReadOnly()) removeDeclaration(id);
      else if (auto *project = svcProject(m_projectSvc))
        for (auto *layer : project->mapLayers())
          if (layer->customProperty("paleoLayerId").toString() == id) project->removeMapLayer(layer);
      continue;
    }
    LayerDeclaration declaration;
    declaration.layerId = id;
    declaration.type = QStringLiteral("mbtiles");
    declaration.source = source;
    declaration.group = QStringLiteral("01_Base");
    declaration.title = id.endsWith("topo") ? tr("离线地形底图") : tr("离线地形阴影");
    QString error;
    if (!m_manifest->isReadOnly() && !declare(declaration, &error))
      qWarning() << "Offline basemap declaration:" << error;
    if (!instantiate(id, &error)) qWarning() << "Offline basemap:" << error;
  }
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
  // by any horizon and survive the switch. A failed read must not look like
  // "this horizon declares nothing" and drop live layers.
  QVector<LayerDeclaration> decls;
  if (!m_manifest->readAll(&decls, nullptr))
    return;

  QStringList toRelease;
  for (const LayerDeclaration &d : decls)
    if (d.horizon == horizon && isInstantiated(d.layerId))
      toRelease.append(d.layerId);

  QgsProject *proj = svcProject(m_projectSvc);
  for (const QString &id : toRelease)
  {
    QgsMapLayer *l = m_instances.take(id).data();
    if (proj && l)
    {
      if (auto *vl = qobject_cast<QgsVectorLayer *>(l))
      {
        if (vl->isEditable())
        {
          // 经服务回滚：busy 标记随会话释放（Issue #27 残留——直接 rollBack
          // 会让该层位的「editing in progress」门控永久滞留）。未注入服务的
          // 裸用路径维持旧行为。
          if (m_editSvc)
            m_editSvc->rollbackEdit(vl);
          else
            vl->rollBack();
        }
      }
      proj->removeMapLayer(l); // project-owned: removal deletes the layer
    }
  }
  emit horizonReleased(horizon);
}

bool QgisLayerService::tryDeclared(QVector<LayerDeclaration> *out, QString *error) const
{
  if (!m_manifest)
  {
    setError(error, QStringLiteral("layer service has no manifest"));
    return false;
  }
  return m_manifest->readAll(out, error);
}

void QgisLayerService::trackInstance(const QString &layerId, QgsMapLayer *layer)
{
  m_instances.insert(layerId, layer);
  // The project owns the layer — if it is destroyed by ANY path (clear(),
  // removeMapLayer(), an external consumer), the cache entry dies with it.
  if (layer)
  {
    // #81：只摘「仍指向这个已销毁对象（或已置空）」的条目。declare() 替换
    // 路径会在同一 layerId 下先删旧层、再登记新层；旧层的 destroyed 若晚于
    // 新层登记（延迟删除/外部持有者），按 layerId 无条件 remove 会把新层
    // 从缓存里摘掉——之后 layer() 返回空、paleoAssetId 盖章静默落空。
    connect(layer, &QObject::destroyed, this, [this, layerId](QObject *dead) {
      const auto it = m_instances.find(layerId);
      if (it != m_instances.end() && (it.value().isNull() || it.value().data() == dead))
        m_instances.erase(it);
    });
  }
}

void QgisLayerService::purgeDanglingInstances()
{
  QgsProject *proj = svcProject(m_projectSvc);
  const QList<QgsMapLayer *> live = proj ? proj->mapLayers().values()
                                         : QList<QgsMapLayer *>();
  for (auto it = m_instances.begin(); it != m_instances.end();)
  {
    if (!it.value() || !live.contains(it.value().data()))
      it = m_instances.erase(it);
    else
      ++it;
  }
  // .qgz 已恢复的实例归同一个服务缓存；再次打开不得为同一声明再造一层。
  for (auto *layer : live) {
    const auto id = layer->customProperty("paleoLayerId").toString();
    if (!id.isEmpty() && layer->isValid() && !m_instances.contains(id)) {
      if (layer->providerType() == QLatin1String("gdal")) MappingArtifactWriter::restoreRasterCrs(layer);
      trackInstance(id, layer);
    }
  }
}

void QgisLayerService::adoptProjectLayers()
{
  // #287：QgsProject::write() 会把实例化过的图层原样存进 .qgz，read() 后
  // 它们带着 paleoLayerId 自定义属性回来，但 cleared 钩子早已清空
  // m_instances——不收编的话：切层位时 instantiate 缓存未命中会对同一数据
  // 源重复 addMapLayer（图层树/画布出现双份）；恢复的副本对 layer()、
  // isEditingAnyLayer()、releaseHorizon 和编辑忙闸全部不可见，在它上面的
  // 编辑不受保护也不回滚。createProject 时 mapLayers 为空，收编是 no-op。
  QgsProject *proj = svcProject(m_projectSvc);
  if (!proj)
    return;
  const auto layers = proj->mapLayers();
  for (auto it = layers.cbegin(); it != layers.cend(); ++it)
  {
    const QString paleoId =
        it.value()->customProperty(QStringLiteral("paleoLayerId")).toString();
    if (!paleoId.isEmpty())
      trackInstance(paleoId, it.value());
  }
}

QgsMapLayer *QgisLayerService::layer(const QString &layerId) const
{
  return m_instances.value(layerId).data();
}

bool QgisLayerService::isInstantiated(const QString &layerId) const
{
  return m_instances.value(layerId) != nullptr;
}

bool QgisLayerService::isEditingAnyLayer(QString *layerName) const
{
  for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it)
  {
    if (!it.value())
      continue;
    if (auto *vl = qobject_cast<QgsVectorLayer *>(it.value().data()))
    {
      if (vl->isEditable())
      {
        if (layerName)
          *layerName = vl->name().isEmpty() ? it.key() : vl->name();
        return true;
      }
    }
  }
  return false;
}

void QgisLayerService::setActiveHorizon(const QString &horizon)
{
  QVector<LayerDeclaration> decls;
  if (!m_manifest->readAll(&decls, nullptr))
    return; // keep every instantiated layer; an empty read is not authoritative

  QHash<QString, QString> horizonOf;
  horizonOf.reserve(decls.size());
  for (const LayerDeclaration &d : decls)
    horizonOf.insert(d.layerId, d.horizon);

  QSet<QString> otherHorizons;
  QStringList orphanIds; // instantiated but declaration has since been removed
  for (auto it = m_instances.cbegin(); it != m_instances.cend(); ++it)
  {
    if (!it.value())
      continue;
    const auto hit = horizonOf.constFind(it.key());
    if (hit == horizonOf.constEnd())
      orphanIds.append(it.key());
    else if (!hit.value().isEmpty() && hit.value() != horizon)
      otherHorizons.insert(hit.value());
    // else: target horizon or horizon-agnostic layer stays instantiated
  }

  QgsProject *proj = svcProject(m_projectSvc);
  for (const QString &id : orphanIds)
  {
    QgsMapLayer *l = m_instances.take(id).data();
    if (proj && l)
    {
      if (auto *vl = qobject_cast<QgsVectorLayer *>(l))
      {
        // 经服务回滚：busy 标记随会话释放（Issue #27——与 releaseHorizon
        // 同口径，直接 rollBack 会让「editing in progress」门控永久滞留）。
        if (vl->isEditable())
        {
          if (m_editSvc)
            m_editSvc->rollbackEdit(vl);
          else
            vl->rollBack();
        }
      }
      proj->removeMapLayer(l);
    }
  }
  for (const QString &h : otherHorizons)
    releaseHorizon(h);

  m_activeHorizon = horizon;
  instantiateHorizon(horizon);
}
