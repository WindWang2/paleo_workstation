// 层：视图
#include "datapage.h"

#include "datalist.h"
#include "entitypanel.h"
#include "pageshared.h"

#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"

#include <QDynamicPropertyChangeEvent>

using namespace paleo::pagesinternal;

DataPage::DataPage(QWidget *parent)
  : QWidget(parent)
{
  setMinimumWidth(0);
  auto *lay = panelLayout(this);

  // W5 分家：列表侧（导入段 + 列表段）与实体视图段各自成件；壳按 ribbon
  // 布局重新安放（entityViewSection() 返回 EntityPanel 本体，可按名摘挂）。
  m_listPanel = new DataListPanel(this);
  m_entityPanel = new EntityPanel(this);
  lay->addWidget(m_listPanel, 1);
  lay->addSpacing(16); // spacing.md between groups
  lay->addWidget(m_entityPanel, 1);

  // 信号直转：子面板信号签名即本页对外信号（壳/测试接的是 DataPage）。
  connect(m_listPanel, &DataListPanel::importRequested, this,
          &DataPage::importRequested);
  connect(m_listPanel, &DataListPanel::assetActivated, this,
          &DataPage::assetActivated);
  connect(m_listPanel, &DataListPanel::assetWellActivated, this,
          &DataPage::assetWellActivated);
  connect(m_listPanel, &DataListPanel::seismicLineActivated, this,
          &DataPage::seismicLineActivated);
  connect(m_listPanel, &DataListPanel::wellSelected, this,
          &DataPage::wellSelected);
  connect(m_listPanel, &DataListPanel::surveyAreaActivated, this,
          &DataPage::surveyAreaActivated);
  // 列表内部建/撤挂接后的实体视图联动（原来走同一 refreshAssetTable）。
  connect(m_listPanel, &DataListPanel::entityRefreshRequested, m_entityPanel,
          &EntityPanel::refresh);
  // 列表内选中 → 壳统一走 selectAsset/selectAssetsForEntities（实体视图联动
  // 口径与旧同件实现一致）。
  connect(m_listPanel, &DataListPanel::assetFocusRequested, this,
          &DataPage::selectAsset);
  connect(m_listPanel, &DataListPanel::entitiesFocusRequested, this,
          &DataPage::selectAssetsForEntities);
}

bool DataPage::event(QEvent *event)
{
  // 壳经动态属性 "paleo.page.importsvc" 绑门面——属性变化时下发两个子面板。
  if (event->type() == QEvent::DynamicPropertyChange)
  {
    auto *dpe = static_cast<QDynamicPropertyChangeEvent *>(event);
    if (dpe->propertyName() == "paleo.page.importsvc")
    {
      PreviewDocService *doc = docService();
      if (m_listPanel)
        m_listPanel->setDocService(doc);
      if (m_entityPanel)
        m_entityPanel->setDocService(doc);
    }
  }
  return QWidget::event(event);
}

PreviewDocService *DataPage::docService() const
{
  return qobject_cast<PreviewDocService *>(
      property("paleo.page.importsvc").value<QObject *>());
}

void DataPage::refreshAssetTable()
{
  // p5a：catalog.changed() 接到本槽——实体视图一并重取（口径与旧同件实现
  // 一致：先实体后列表）。
  if (m_entityPanel)
    m_entityPanel->refresh();
  if (m_listPanel)
    m_listPanel->refreshAssetTable();
}

void DataPage::refreshEntityView()
{
  if (m_entityPanel)
    m_entityPanel->refresh();
}

void DataPage::applyListFilter()
{
  if (m_listPanel)
    m_listPanel->applyListFilter();
}

void DataPage::setUnresolvedFilter(bool on)
{
  if (m_listPanel)
    m_listPanel->setUnresolvedFilter(on);
}

void DataPage::selectAssetsForEntities(const QStringList &entityIds)
{
  // p5a：首个选中 id 驱动实体角色槽视图；空选择清回空态（地图取消点选）。
  setProperty("paleo.page.entityId",
              entityIds.isEmpty() ? QString() : entityIds.front());
  setProperty("paleo.page.assetId", QString());
  if (m_entityPanel)
  {
    m_entityPanel->setContext(
        entityIds.isEmpty() ? QString() : entityIds.front(), QString());
    m_entityPanel->refresh();
  }
  if (m_listPanel)
    m_listPanel->selectAssetsForEntities(entityIds);
}

void DataPage::selectAsset(const QString &assetId)
{
  setProperty("paleo.page.assetId", assetId);
  QString matchedEntity;
  if (PreviewDocService *svc = docService())
    if (DataCatalog *cat = svc->catalog())
    {
      if (!assetId.isEmpty())
        for (const EntityAssetLink &l : cat->linksForAsset(assetId))
          if (!l.entityId.isEmpty() && !l.unresolved)
          {
            matchedEntity = l.entityId;
            break;
          }
    }
  setProperty("paleo.page.entityId", matchedEntity);
  if (m_entityPanel)
  {
    m_entityPanel->setContext(matchedEntity, assetId);
    m_entityPanel->refresh();
  }
  if (m_listPanel)
    m_listPanel->selectAssetInViews(assetId);
}
