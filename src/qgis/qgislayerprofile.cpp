// 层：QGIS 封装
#include "qgislayerprofile.h"

#include "layervocabulary.h"
#include "qgislayerservice.h"

#include <QDebug>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QVector>

#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgslayertreemodel.h>
#include <qgslayertreenode.h>
#include <qgslayoutitemmap.h>
#include <qgsmaplayer.h>
#include <qgsmapthemecollection.h>
#include <qgsproject.h>

namespace
{
  // paleoLayerId 自定义属性（QgisLayerService::instantiate 写入，见其 .cpp）。
  QString paleoLayerIdOf(const QgsMapLayer *layer)
  {
    return layer ? layer->customProperty(QStringLiteral("paleoLayerId")).toString()
                 : QString();
  }

  // 同名树组按组内结果同步勾选态：组内有任一受管（有声明）图层可见 → 勾选；
  // 全部不可见 → 取消；组内无受管图层 → 不动。isVisible() 会把未勾选的父组
  // 算作不可见，不同步会让并入档案的图层（如 predict 的当前层位约束层）
  // 被父组整体遮挡。
  void syncGroupCheckStates(QgsLayerTreeGroup *root,
                            const QHash<QString, LayerDeclaration> &declById)
  {
    const QList<QgsLayerTreeGroup *> groups = root->findGroups(true);
    for (QgsLayerTreeGroup *group : groups)
    {
      bool anyManaged = false;
      bool anyVisible = false;
      const QList<QgsLayerTreeLayer *> layers = group->findLayers();
      for (QgsLayerTreeLayer *nodeLayer : layers)
      {
        if (!declById.contains(paleoLayerIdOf(nodeLayer->layer())))
          continue; // 无声明（含 layer 已析构的悬空节点）：不参与组态
        anyManaged = true;
        if (nodeLayer->itemVisibilityChecked())
          anyVisible = true;
      }
      if (anyManaged)
        group->setItemVisibilityChecked(anyVisible);
    }
  }
} // namespace

QgisLayerProfileService::QgisLayerProfileService(QgsProject *project, QObject *parent)
    : QObject(parent), m_project(project)
{
  // 转发 QgsMapThemeCollection::mapThemesChanged（insert/update/remove 均触发）。
  if (QgsMapThemeCollection *collection = themeCollection())
  {
    connect(collection, &QgsMapThemeCollection::mapThemesChanged,
            this, &QgisLayerProfileService::mapThemesChanged);
  }
}

QgisLayerProfileService::~QgisLayerProfileService() = default;

void QgisLayerProfileService::setLayerTreeModel(QgsLayerTreeModel *model) { m_model = model; }

void QgisLayerProfileService::setLayerService(QgisLayerService *service) { m_layerService = service; }

QStringList QgisLayerProfileService::defaultProfileGroups(const QString &pageId)
{
    // 词表单一权威：页表在 layervocabulary.h（主线1），此处只透传。
    return PaleoLayerVocabulary::profileGroupsForPage(pageId);
}

void QgisLayerProfileService::setProfileGroupsOverride(const QString &pageId,
                                                       const QStringList &groups)
{
    m_groupOverrides.insert(pageId, groups);
}

QStringList QgisLayerProfileService::profileGroupsFor(const QString &pageId) const
{
    const auto it = m_groupOverrides.constFind(pageId);
    if (it != m_groupOverrides.constEnd())
        return it.value();
    return defaultProfileGroups(pageId);
}

QString QgisLayerProfileService::pageThemeName(const QString &pageId)
{
    return QStringLiteral("page:") + pageId;
}

QgsMapThemeCollection *QgisLayerProfileService::themeCollection() const
{
  return m_project ? m_project->mapThemeCollection() : nullptr;
}

bool QgisLayerProfileService::applyPageProfile(const QString &pageId)
{
  if (!m_project || !m_model)
    return false; // 未接线防御

  // 已知页 = 四个地图页 + data + 档案表 override 登记过的扩展页；其余拒绝。
  const bool knownPage =
      pageId == QStringLiteral("predict") || pageId == QStringLiteral("constraint") ||
      pageId == QStringLiteral("compose") || pageId == QStringLiteral("validate") ||
      pageId == QStringLiteral("data") || m_groupOverrides.contains(pageId);
  if (!knownPage)
    return false;

  // data 页无地图：只记录当前页，不摆树、不建主题、不发信号。
  if (pageId == QStringLiteral("data"))
  {
    m_currentPage = pageId;
    return true;
  }

  const QString themeName = pageThemeName(pageId);
  QgsMapThemeCollection *collection = themeCollection();
  if (!collection)
    return false;

  if (!collection->hasMapTheme(themeName))
  {
    // 主题不存在：先按档案表摆树，再把当前树态定格成 "page:<pageId>" 主题。
    if (!stageTreeVisibility(profileGroupsFor(pageId),
                             pageId == QStringLiteral("predict")))
      return false;
    const QgsMapThemeCollection::MapThemeRecord rec =
        QgsMapThemeCollection::createThemeFromCurrentState(m_project->layerTreeRoot(),
                                                           m_model);
    collection->insert(themeName, rec);
  }
  else if (!applyPrunedTheme(themeName))
  {
    return false;
  }

  m_currentPage = pageId;
  emit profileApplied(pageId, themeName);
  return true;
}

bool QgisLayerProfileService::applyCurrentPageProfile()
{
  if (m_currentPage.isEmpty())
    return false;
  if (m_currentPage == QStringLiteral("data"))
    return true; // data 重放：无操作，语义成功
  return applyPageProfile(m_currentPage);
}

bool QgisLayerProfileService::stageTreeVisibility(const QStringList &groups,
                                                  bool mergeActiveHorizonConstraints)
{
  QgsLayerTree *root = m_project->layerTreeRoot();
  if (!root)
    return false;

  // 声明回查表：paleoLayerId → declaration。layerService 为 null 时无从回查
  //（所有图层可见性不动）；清单读失败是读失败，不是"无声明"——失败上报。
  QHash<QString, LayerDeclaration> declById;
  if (m_layerService)
  {
    QVector<LayerDeclaration> decls;
    if (!m_layerService->tryDeclared(&decls, nullptr))
      return false;
    for (const LayerDeclaration &d : decls)
      declById.insert(d.layerId, d);
  }

  const QString activeHorizon =
      m_layerService ? m_layerService->activeHorizon() : QString();

  const QList<QgsLayerTreeLayer *> layers = root->findLayers();
  for (QgsLayerTreeLayer *nodeLayer : layers)
  {
    QgsMapLayer *layer = nodeLayer->layer();
    if (!layer)
      continue; // 未解析的占位引用：不动
    const QString paleoId = paleoLayerIdOf(layer);
    if (paleoId.isEmpty())
      continue; // 用户手加的临时图层：可见性不动
    const auto it = declById.constFind(paleoId);
    if (it == declById.constEnd())
      continue; // 无声明：可见性不动

    const LayerDeclaration &decl = it.value();
    // 旧名→canonical 在此吸收（主线1）：旧 .qgz/project.sqlite 里的
    // "01_Prediction" 等历史组名经 profileContains 折算，不再表外隐藏。
    bool visible = PaleoLayerVocabulary::profileContains(groups, decl.group);
    const bool isSharedData = (decl.horizon.isEmpty() &&
        PaleoLayerVocabulary::groupRoot(PaleoLayerVocabulary::canonicalize(decl.group)) == QLatin1String("00_Data"));
    if (isSharedData)
    {
      visible = true; // 井位与测区范围等基础共享数据层，作为所有编图页的基准参考叠加层始终保持可见
    }
    else if (!visible && mergeActiveHorizonConstraints
        && PaleoLayerVocabulary::canonicalize(decl.group) == PaleoLayerVocabulary::kConstraintsGroup
        && m_layerService && decl.horizon == activeHorizon)
    {
      visible = true; // predict 档案并入当前层位约束图层
    }
    nodeLayer->setItemVisibilityChecked(visible);
  }

  syncGroupCheckStates(root, declById);
  return true;
}

void QgisLayerProfileService::ensureSharedDataVisible()
{
  if (!m_project)
    return;
  QgsLayerTree *root = m_project->layerTreeRoot();
  if (!root)
    return;

  QHash<QString, LayerDeclaration> declById;
  if (m_layerService)
  {
    QVector<LayerDeclaration> decls;
    if (m_layerService->tryDeclared(&decls, nullptr))
    {
      for (const auto &d : decls)
        declById.insert(d.layerId, d);
    }
  }

  bool anyChanged = false;
  const QList<QgsLayerTreeLayer *> layers = root->findLayers();
  for (QgsLayerTreeLayer *nodeLayer : layers)
  {
    QgsMapLayer *layer = nodeLayer->layer();
    if (!layer)
      continue;
    const QString paleoId = paleoLayerIdOf(layer);
    bool isSharedData = (paleoId == QLatin1String("wells") || paleoId == QLatin1String("survey.area"));
    if (!isSharedData && !paleoId.isEmpty())
    {
      const auto it = declById.constFind(paleoId);
      if (it != declById.constEnd())
      {
        const LayerDeclaration &d = it.value();
        if (d.horizon.isEmpty() &&
            PaleoLayerVocabulary::groupRoot(PaleoLayerVocabulary::canonicalize(d.group)) == QLatin1String("00_Data"))
        {
          isSharedData = true;
        }
      }
    }

    if (isSharedData && !nodeLayer->itemVisibilityChecked())
    {
      nodeLayer->setItemVisibilityChecked(true);
      anyChanged = true;
    }
  }

  if (anyChanged)
  {
    syncGroupCheckStates(root, declById);
    if (QgsMapThemeCollection *col = themeCollection(); col && !m_currentPage.isEmpty() && m_model)
    {
      const QString themeName = pageThemeName(m_currentPage);
      if (col->hasMapTheme(themeName))
      {
        const QgsMapThemeCollection::MapThemeRecord updatedRec =
            QgsMapThemeCollection::createThemeFromCurrentState(root, m_model);
        col->update(themeName, updatedRec);
      }
    }
  }
}

bool QgisLayerProfileService::applyPrunedTheme(const QString &name)
{
  QgsMapThemeCollection *collection = themeCollection();
  if (!collection || !m_project || !m_model || !collection->hasMapTheme(name))
    return false;

  // 防御性修剪：记录里的弱图层指针已析构、或图层 id 不在当前工程注册表
  //（层位切换释放实例）——先剔除并 update 回写，再应用。
  const QgsMapThemeCollection::MapThemeRecord rec = collection->mapThemeState(name);
  const QMap<QString, QgsMapLayer *> live = m_project->mapLayers();

  QgsMapThemeCollection::MapThemeRecord pruned;
  pruned.setHasExpandedStateInfo(rec.hasExpandedStateInfo());
  pruned.setExpandedGroupNodes(rec.expandedGroupNodes());
  pruned.setHasCheckedStateInfo(rec.hasCheckedStateInfo());
  pruned.setCheckedGroupNodes(rec.checkedGroupNodes());
  bool changed = false;
  const QList<QgsMapThemeCollection::MapThemeLayerRecord> records = rec.layerRecords();
  for (const QgsMapThemeCollection::MapThemeLayerRecord &layerRecord : records)
  {
    QgsMapLayer *layer = layerRecord.layer(); // 弱指针：层已析构 → null，不 dereference
    if (layer && live.contains(layer->id()))
      pruned.addLayerRecord(layerRecord);
    else
      changed = true;
  }
  if (changed)
    collection->update(name, pruned);

  // page:* 主题：无声明图层（用户手加的临时图层）可见性守恒——头注释契约
  //（stageTreeVisibility 首建路径跳过它们；重放路径同样不得动）。
  // QgsMapThemeCollection::applyTheme 会把不在主题记录内的图层一律置隐藏，
  // 这里先快照、应用后恢复。有效可见性 = 自身勾选 ∧ 祖先组勾选，两级都按
  // 快照还原（祖先组勾选只影响该组内无声明层的有效可见性——声明层自身
  // 勾选仍由主题记录裁定，组被勾回不会把声明层带出来）。
  const bool isPageTheme = name.startsWith(QLatin1String("page:"));
  QHash<QString, bool> undeclaredChecked;    // layerId → 自身勾选
  QSet<QString> undeclaredWasVisible;        // 有效可见的 layerId
  if (isPageTheme)
  {
    QSet<QString> declaredIds;
    if (m_layerService)
    {
      QVector<LayerDeclaration> decls;
      // 读失败 ≠ 无声明：失败时 declaredIds 留空，带 paleoLayerId 的层随之
      // 全部落进守恒集（快照/恢复其勾选，主题对它们空转）——重放退化为
      // 保持现状。这是有意的 fail-safe：清单不可读时绝不让主题按记录批量
      // 遮层。
      if (m_layerService->tryDeclared(&decls, nullptr))
        for (const LayerDeclaration &d : decls)
          declaredIds.insert(d.layerId);
    }
    else
    {
      // 无图层服务（测试裸挂/早期构造）：无法区分声明层，全部进守恒集，
      // page:* 主题可见性记录整体不生效——留日志便于诊断空转。
      qWarning() << "QgisLayerProfileService: page 主题" << name
                 << "重放时图层服务为空，全部图层按无声明守恒（主题记录不生效）";
    }
    const QList<QgsLayerTreeLayer *> nodes = m_project->layerTreeRoot()->findLayers();
    for (QgsLayerTreeLayer *node : nodes)
    {
      QgsMapLayer *layer = node->layer();
      if (!layer)
        continue;
      const QString paleoId = paleoLayerIdOf(layer);
      if (!paleoId.isEmpty() && declaredIds.contains(paleoId))
        continue; // 受管声明层：主题照常裁定
      undeclaredChecked.insert(layer->id(), node->itemVisibilityChecked());
      bool effective = node->itemVisibilityChecked();
      for (QgsLayerTreeNode *p = node->parent(); p; p = p->parent())
        effective = effective && p->itemVisibilityChecked();
      if (effective)
        undeclaredWasVisible.insert(layer->id());
    }
  }

  // 修剪后为空仍可应用：全隐藏语义。
  collection->applyTheme(name, m_project->layerTreeRoot(), m_model);

  if (isPageTheme && !undeclaredChecked.isEmpty())
  {
    QgsLayerTree *root = m_project->layerTreeRoot();
    for (auto it = undeclaredChecked.cbegin(); it != undeclaredChecked.cend(); ++it)
    {
      QgsLayerTreeLayer *node = root ? root->findLayer(it.key()) : nullptr;
      if (!node)
        continue; // applyTheme 不删节点；防御
      node->setItemVisibilityChecked(it.value());
      if (undeclaredWasVisible.contains(it.key()))
        for (QgsLayerTreeNode *p = node->parent(); p; p = p->parent())
          p->setItemVisibilityChecked(true);
    }
  }

  // 页面档案（page:*）必须保证井位与测区范围等基础共享数据层（00_Data / Zone::SharedData）可见：
  // 避免因存量主题未记录或历史快照未勾选导致井位与测区范围互斥或丢失。
  if (name.startsWith(QLatin1String("page:")))
  {
    ensureSharedDataVisible();
  }

  return true;
}

bool QgisLayerProfileService::captureCurrentAsTheme(const QString &name)
{
  if (name.isEmpty() || !m_project || !m_model)
    return false;
  QgsMapThemeCollection *collection = themeCollection();
  if (!collection)
    return false;

  if (name.startsWith(QLatin1String("page:")))
  {
    ensureSharedDataVisible();
  }

  const QgsMapThemeCollection::MapThemeRecord rec =
      QgsMapThemeCollection::createThemeFromCurrentState(m_project->layerTreeRoot(),
                                                         m_model);
  if (collection->hasMapTheme(name))
    collection->update(name, rec); // 覆盖保存语义
  else
    collection->insert(name, rec);
  return true;
}

bool QgisLayerProfileService::applyTheme(const QString &name)
{
  return applyPrunedTheme(name);
}

bool QgisLayerProfileService::removeMapTheme(const QString &name)
{
  QgsMapThemeCollection *collection = themeCollection();
  if (!collection || !collection->hasMapTheme(name))
    return false;
  collection->removeMapTheme(name);
  return true;
}

bool QgisLayerProfileService::renameTheme(const QString &oldName, const QString &newName)
{
  QgsMapThemeCollection *collection = themeCollection();
  if (!collection || newName.isEmpty() || newName == oldName)
    return false;
  if (!collection->hasMapTheme(oldName) || collection->hasMapTheme(newName))
    return false; // 旧名不存在 / 新名已占用（不覆盖既有主题）
  // page:* 页面档案按 pageId 重建，且 .qgz 里版面地图项钉的
  // followVisibilityPresetName 会随改名悬空——服务层直接拒绝（此前只有
  // 头注释约定，新调用面忘滤即静默坏档）。
  if (oldName.startsWith(QStringLiteral("page:")) ||
      newName.startsWith(QStringLiteral("page:")))
    return false;

  // 原生 renameMapTheme（QGIS ≥3.14）：语义单调用 + 专用 mapThemeRenamed
  // 信号（实现内部仍走 insert/remove，但由上游保证一致性顺序与失败回滚）。
  return collection->renameMapTheme(oldName, newName);
}

bool QgisLayerProfileService::hasTheme(const QString &name) const
{
  QgsMapThemeCollection *collection = themeCollection();
  return collection && collection->hasMapTheme(name);
}

QStringList QgisLayerProfileService::themes() const
{
  QgsMapThemeCollection *collection = themeCollection();
  return collection ? collection->mapThemes() : QStringList();
}

void QgisLayerProfileService::setLayoutMapTheme(QgsLayoutItemMap *mapItem,
                                                const QString &themeName)
{
  if (!mapItem)
    return;
  if (themeName.isEmpty())
  {
    mapItem->setFollowVisibilityPreset(false);
    return;
  }
  mapItem->setFollowVisibilityPreset(true);
  mapItem->setFollowVisibilityPresetName(themeName);
}
