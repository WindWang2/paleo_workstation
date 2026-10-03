// 层：QGIS 封装
#include "qgislayerservice.h"

#include "qgisprojectservice.h"
#include "qgiseditingservice.h"
#include "layervocabulary.h"
#include "mappingartifactwriter.h"

#include <QSet>
#include <QtGlobal>

#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
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

  // 归位路径：声明图层按「地层大组（decl.horizon 非空）⊃ 工作流子组
  //（decl.group，"/" 分隔子路径）」摆树——实例化即归组，不再根上平铺。
  // horizon 为空（层位无关层，如地震/井位）退为根上工作流组；两段皆空
  // 才落根（与原 addMapLayer 默认行为等价）。
  QgsLayerTreeGroup *groupForPath(QgsLayerTreeGroup *root, const QStringList &segments)
  {
    QgsLayerTreeGroup *cur = root;
    for (const QString &seg : segments)
    {
      if (seg.isEmpty())
        continue;
      // 只认直接子节点（findGroup 递归——用户在别支下自建的同名组会误归位）。
      QgsLayerTreeGroup *hit = nullptr;
      for (QgsLayerTreeNode *child : cur->children())
        if (QgsLayerTree::isGroup(child) && child->name() == seg)
          hit = QgsLayerTree::toGroup(child);
      cur = hit ? hit : cur->addGroup(seg);
    }
    return cur;
  }

  // 释放/摘除后收空壳：只剪我们自己摆出来的组——树组内已无后代图层
  //（findLayers 递归）才摘；用户往组里拖的手工层会阻止剪枝。
  void pruneEmptyRootGroup(QgsProject *proj, const QString &groupName)
  {
    if (!proj || groupName.isEmpty())
      return;
    QgsLayerTree *root = proj->layerTreeRoot();
    if (!root)
      return;
    for (QgsLayerTreeNode *child : root->children())
    {
      if (QgsLayerTree::isGroup(child) && child->name() == groupName
          && QgsLayerTree::toGroup(child)->findLayers().isEmpty())
      {
        root->removeChildNode(child);
        return;
      }
    }
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
  QgsProject *proj = resolveProject(m_projectSvc);
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
  // first, but be explicit): drop every entry not still registered.
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this] { purgeDanglingInstances(); });
}

bool QgisLayerService::declare(const LayerDeclaration &decl, QString *error)
{
  if (decl.layerId.isEmpty())
  {
    setError(error, QStringLiteral("cannot declare a layer with an empty layerId"));
    return false;
  }
  QgsMapLayer *previous = m_instances.value(decl.layerId).data();
  const bool replace = previous && previous->source() != decl.source;
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
    if (auto *project = resolveProject(m_projectSvc))
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
    QgsProject *proj = resolveProject(m_projectSvc);
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

  MappingArtifactWriter::restoreRasterCrs(layer.get());

  // QgsProject takes ownership; keep only the raw pointer in the instance map.
  // addToLegend=false：树节点不由工程默认摆（根顶平铺），随即按声明归组。
  QgsMapLayer *added = proj->addMapLayer(layer.get(), false);
  if (!added)
  {
    setError(error, QStringLiteral("QgsProject refused layer '%1'").arg(layerId));
    return nullptr;
  }
  layer.release();

  // 归组：地层大组 ⊃ 工作流子组（旧组名经 canonicalize 折算——树上只见
  // canonical 组名；组内 insertLayer(0) 顶置，与原根顶落位同序语义）。
  QStringList groupPath;
  if (!decl->horizon.isEmpty())
    groupPath << decl->horizon;
  const QString canonGroup = PaleoLayerVocabulary::canonicalize(decl->group);
  groupPath << canonGroup.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  groupForPath(proj->layerTreeRoot(), groupPath)->insertLayer(0, added);
  if (!decl->title.isEmpty())
    added->setName(decl->title); // 显示名优先 title，机器名仍在 paleoLayerId
  added->setCustomProperty(QStringLiteral("paleoLayerId"), decl->layerId);
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

  QgsProject *proj = resolveProject(m_projectSvc);
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
  // 自动摆出来的地层大组：释放后整组无后代图层即摘掉（手工拖进来的层
  // 会阻止剪枝）——否则切换几次层位，树里攒一串空历史层位组。
  pruneEmptyRootGroup(proj, horizon);
  emit horizonReleased(horizon);
}

void QgisLayerService::reconcileTreeGrouping()
{
  QgsProject *proj = resolveProject(m_projectSvc);
  QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
  if (!root || !m_manifest)
    return;
  QVector<LayerDeclaration> decls;
  if (!m_manifest->readAll(&decls, nullptr))
    return;
  QHash<QString, QStringList> pathById;
  pathById.reserve(decls.size());
  for (const LayerDeclaration &d : decls)
  {
    QStringList path;
    if (!d.horizon.isEmpty())
      path << d.horizon;
    path << PaleoLayerVocabulary::canonicalize(d.group)
                .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!path.isEmpty())
      pathById.insert(d.layerId, path);
  }

  // 只搬树根直挂的声明图层：嵌在用户自建组里的位置是用户排版，不重排；
  // 无 paleoLayerId 的手工层本来就不归清单管。
  const QList<QgsLayerTreeNode *> kids = root->children();
  for (QgsLayerTreeNode *n : kids)
  {
    if (!QgsLayerTree::isLayer(n))
      continue;
    auto *ln = QgsLayerTree::toLayer(n);
    QgsMapLayer *l = ln ? ln->layer() : nullptr;
    if (!l)
      continue;
    const auto it = pathById.constFind(
        l->customProperty(QStringLiteral("paleoLayerId")).toString());
    if (it == pathById.constEnd())
      continue;
    if (root->takeChild(ln)) // take 不删节点——插入目标组接管
      groupForPath(root, it.value())->insertChildNode(0, ln);
  }
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
  QgsProject *proj = resolveProject(m_projectSvc);
  const QList<QgsMapLayer *> live = proj ? proj->mapLayers().values()
                                         : QList<QgsMapLayer *>();
  for (auto it = m_instances.begin(); it != m_instances.end();)
  {
    if (!it.value() || !live.contains(it.value().data()))
      it = m_instances.erase(it);
    else
      ++it;
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

  QgsProject *proj = resolveProject(m_projectSvc);
  for (const QString &id : orphanIds)
  {
    QgsMapLayer *l = m_instances.take(id).data();
    if (proj && l)
    {
      if (auto *vl = qobject_cast<QgsVectorLayer *>(l))
      {
        if (vl->isEditable())
          vl->rollBack();
      }
      proj->removeMapLayer(l);
    }
  }
  for (const QString &h : otherHorizons)
    releaseHorizon(h);

  m_activeHorizon = horizon;
  instantiateHorizon(horizon);
}
