// 层：视图
#include "datapage.h"

#include "datalist.h"
#include "entitypanel.h"
#include "pageshared.h"

#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "dataopspalette.h"
#include "dataopsviews.h"

#include <QDynamicPropertyChangeEvent>
#include <QShortcut>

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

  wireDataOps();
}

// P3 dataops 接线（wave/data-page-operations）：共享命令栈/存储、命令面板
//（Ctrl+K）、快捷键表（?）、状态反馈信号、实体 CRUD 意图、多选批量概要。
void DataPage::wireDataOps()
{
  if (m_dataopsWired)
    return;
  m_dataopsWired = true;
  using namespace paleo::dataops;

  // 共享栈：列表侧拥有（stores + DataOpsUndoStack + OperationsHistory），
  // 实体侧写操作进同一条栈。
  m_entityPanel->setSharedOps(m_listPanel->opsContext(), m_listPanel->opStack(),
                              m_listPanel->operationsHistory());

  // 状态反馈（D5.4/D8）与外部导入意图（D3.2）直转。
  connect(m_listPanel, &DataListPanel::statusMessage, this, &DataPage::statusMessage);
  connect(m_listPanel, &DataListPanel::externalImportRequested, this,
          &DataPage::externalImportRequested);
  connect(m_entityPanel, &EntityPanel::statusMessage, this, &DataPage::statusMessage);
  // 实体 CRUD 联动（实体侧改完请列表/实体视图重取）。
  connect(m_entityPanel, &EntityPanel::entityRefreshRequested, this,
          &DataPage::refreshAssetTable);
  // 树内 F2/菜单实体意图 → 实体面板执行（同一套对话框/命令栈）。
  connect(m_listPanel, &DataListPanel::entityRenameRequested, m_entityPanel,
          &EntityPanel::beginRenameEntity);
  connect(m_listPanel, &DataListPanel::entityDeleteRequested, m_entityPanel,
          &EntityPanel::beginDeleteEntity);
  // D6.3 快捷键表（? 键在列表侧，对话框在这里开）。
  connect(m_listPanel, &DataListPanel::shortcutsDialogRequested, this,
          &DataPage::openShortcutsDialog);

  // D1.2/D4.9：多选 → 实体面板批量概要（>1 资产时）。
  connect(m_listPanel, &DataListPanel::selectionCountChanged, this, [this](int assets, int) {
    if (assets > 1)
      m_entityPanel->setMultiContext({}, m_listPanel->currentAssetSelection().values());
  });

  // D6.1 Ctrl+K 命令面板（数据页内）。
  auto *paletteSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
  paletteSc->setObjectName(QStringLiteral("scCommandPalette"));
  connect(paletteSc, &QShortcut::activated, this, &DataPage::openCommandPalette);
}

void DataPage::openCommandPalette()
{
  using namespace paleo::dataops;
  DataCommandPalette palette(this);
  QVector<DataCommandPalette::Source> sources;
  // 1) 动作/过滤器命令（D6.2 注册表）。
  const CommandRegistry &reg = m_listPanel->commandRegistry();
  for (const CommandEntry &e : reg.all())
  {
    DataCommandPalette::Source s;
    s.kind = QStringLiteral("command");
    s.id = e.id;
    s.title = e.title;
    s.subtitle = e.category + (e.shortcut.isEmpty()
                                   ? QString()
                                   : QStringLiteral(" · ") + e.shortcut);
    s.weight = e.weight;
    s.trigger = e.trigger; // 可空（纯登记面）
    sources.append(s);
  }
  // 2) 资产 + 实体（模糊搜定位）。
  if (PreviewDocService *svc = docService())
    if (DataCatalog *cat = svc->catalog())
    {
      for (const CatalogAsset &a : cat->assets())
      {
        DataCommandPalette::Source s;
        s.kind = QStringLiteral("asset");
        s.id = a.id;
        s.title = a.displayName;
        s.subtitle = tr("资产 · %1").arg(a.type);
        sources.append(s);
      }
      const paleo::dataops::EntityOverrideStore *ov =
          m_listPanel->opsContext().entityOverrides;
      for (const CatalogEntity &e : cat->entities())
      {
        DataCommandPalette::Source s;
        s.kind = QStringLiteral("entity");
        s.id = e.id;
        s.title = ov ? ov->displayName(e) : e.name;
        s.subtitle = tr("实体 · %1").arg(e.entityType);
        sources.append(s);
      }
    }
  palette.setSources(std::move(sources));
  connect(&palette, &DataCommandPalette::assetChosen, this, [this](const QString &id) {
    selectAsset(id);
    emit assetActivated(id);
  });
  connect(&palette, &DataCommandPalette::entityChosen, this, [this](const QString &id) {
    selectAssetsForEntities({id});
  });
  palette.exec();
}

void DataPage::openShortcutsDialog()
{
  using namespace paleo::dataops;
  // 快捷键表 = 列表侧注册表 + 内建 Qt 行为说明。
  CommandRegistry reg = m_listPanel->commandRegistry();
  CommandEntry e;
  e.id = QStringLiteral("qt.selectAll");
  e.title = tr("全选（内建）");
  e.category = tr("选择");
  e.shortcut = QStringLiteral("Ctrl+A");
  reg.registerCommand(e);
  ShortcutsDialog dlg(this);
  dlg.loadRegistry(reg);
  dlg.exec();
}

bool DataPage::vimModeEnabled() const
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  return s.value(QStringLiteral("dataops/vimMode")).toBool();
}

void DataPage::setVimModeEnabled(bool on)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("dataops/vimMode"), on);
  if (auto *nav = findChild<paleo::dataops::DataNavTree *>(QStringLiteral("dataTree")))
    nav->setVimMode(on);
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
      // 门面变化（新工程/重开）→ 实体侧共享 ops 重新下发（ctx 指针随列表侧
      // refreshAssetTable 重载，这里保证面板构造后就有栈可用）。
      m_entityPanel->setSharedOps(m_listPanel->opsContext(), m_listPanel->opStack(),
                                  m_listPanel->operationsHistory());
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

// AUTOMOC：dataopspalette.h 的 Q_OBJECT 类（命令面板/快捷键表）。
#include "moc_dataopspalette.cpp"
