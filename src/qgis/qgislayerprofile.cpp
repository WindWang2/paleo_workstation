#include "qgislayerprofile.h"

#include "qgislayerservice.h"
#include "qgisprojectservice.h"

#include <qgslayertree.h>
#include <qgslayoutitemmap.h>
#include <qgsmaplayer.h>
#include <qgsmapthemecollection.h>
#include <qgsproject.h>

namespace
{
  // 组匹配：声明组等于档案组或以其为前缀（如 "04_SingleFactor/Contours"
  // 属于 04_SingleFactor）。
  bool groupMatches(const QString &declGroup, const QString &profileGroup)
  {
    return declGroup == profileGroup ||
           declGroup.startsWith(profileGroup + QLatin1Char('/'));
  }
} // namespace

QStringList QgisLayerProfileService::profileGroups(const QString &pageId)
{
  if (pageId == QLatin1String("predict"))
    return {QStringLiteral("01_Base"), QStringLiteral("02_Prediction")};
  if (pageId == QLatin1String("constraint"))
    return {QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
            QStringLiteral("04_SingleFactor")};
  if (pageId == QLatin1String("compose"))
    return {QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
            QStringLiteral("04_SingleFactor"), QStringLiteral("05_PaleoMap"),
            QStringLiteral("06_Reference")};
  if (pageId == QLatin1String("validate"))
    return {QStringLiteral("01_Base"), QStringLiteral("07_Validation")};
  return {};
}

const QStringList &QgisLayerProfileService::knownPageIds()
{
  static const QStringList ids = {QStringLiteral("predict"),
                                  QStringLiteral("constraint"),
                                  QStringLiteral("compose"),
                                  QStringLiteral("validate")};
  return ids;
}

QString QgisLayerProfileService::themeNameForPage(const QString &pageId)
{
  return QStringLiteral("paleo.page.%1").arg(pageId);
}

QgisLayerProfileService::QgisLayerProfileService(QgisLayerService *layers,
                                                 QgisProjectService *projectSvc,
                                                 QObject *parent)
  : QObject(parent)
  , m_layers(layers)
  , m_projectSvc(projectSvc)
{
}

QStringList QgisLayerProfileService::applyPageProfile(const QString &pageId)
{
  const QStringList groups = profileGroups(pageId);
  if (groups.isEmpty() || !m_layers)
    return {};

  QVector<LayerDeclaration> declared;
  if (!m_layers->tryDeclared(&declared))
    return {};

  const QString horizon = m_layers->activeHorizon();
  QStringList ids;
  for (const LayerDeclaration &d : declared)
  {
    const bool inProfile =
        std::any_of(groups.cbegin(), groups.cend(),
                    [&d](const QString &g) { return groupMatches(d.group, g); });
    if (!inProfile)
      continue;
    // 层位过滤：激活层位非空时只取该层位图层 + 层位无关（01_Base 等）图层。
    if (!horizon.isEmpty() && !d.horizon.isEmpty() && d.horizon != horizon)
      continue;
    QString err;
    if (m_layers->instantiate(d.layerId, &err))
      ids << d.layerId;
  }

  // 真实 QgsMapThemeCollection 落档 + 图层树勾选态对齐（m1 前无图层树
  // model，applyTheme(model) 不可用——按主题成员直接拨节点可见性）。
  if (m_projectSvc)
  {
    if (QgsProject *project = m_projectSvc->project())
    {
      QgsMapThemeCollection::MapThemeRecord record;
      for (const QString &id : ids)
      {
        if (QgsMapLayer *layer = m_layers->layer(id))
          record.addLayerRecord(QgsMapThemeCollection::MapThemeLayerRecord(layer));
      }
      project->mapThemeCollection()->insert(themeNameForPage(pageId), record);

      if (QgsLayerTree *root = project->layerTreeRoot())
      {
        const QList<QgsLayerTreeLayer *> nodes = root->findLayers();
        for (QgsLayerTreeLayer *node : nodes)
        {
          // 声明 layerId 与 QgsMapLayer::id() 的桥：QgisLayerService 实例
          // 化时按 layerId 挂 project（node->layerId() 即声明 id 的映射），
          // 这里以「主题成员集合」为准拨勾选态。
          const QString nodeLayerId = node->layerId();
          bool inTheme = false;
          for (const QString &id : ids)
          {
            if (QgsMapLayer *layer = m_layers->layer(id))
            {
              if (layer->id() == nodeLayerId)
              {
                inTheme = true;
                break;
              }
            }
          }
          node->setItemVisibilityChecked(inTheme);
        }
      }
    }
  }

  emit profileApplied(pageId, ids);
  return ids;
}

bool QgisLayerProfileService::setLayoutMapTheme(QgsLayoutItemMap *map,
                                                const QString &pageId)
{
  if (!map || profileGroups(pageId).isEmpty())
    return false;
  map->setFollowVisibilityPresetName(themeNameForPage(pageId));
  return true;
}
