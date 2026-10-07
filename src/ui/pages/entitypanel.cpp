// 层：视图
#include "entitypanel.h"
#include "datalistops.h"
#include "derivationgraph.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../services/paleotaskservice.h" // F3：GeoJSON 统计任务池路径
#include "../../catalog/datacatalog.h"
#include "../../catalog/entityview.h"
#include "dataopspanelextra.h"
#include "dataopspanelops.h"
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include "../notifications/paleonotify.h"
#include <QDialog>  // 原经 <QMessageBox> 传递引入
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>
#include "../paleotheme.h"

using namespace paleo::pagesinternal;

namespace
{
  // p5a 实体视图占位格（「缺失」/「—」）：缺源可见但样式克制（upstream
  // missing-source 原则——空角色如实显示为缺失槽位，灰字、不可交互）。

  struct WellLogRow
  {
    EntityAssetLink link;
    bool unresolved = false;
  };

  void clearCellWidgets(QTableWidget *table)
  {
    if (!table)
      return;
    for (int r = 0; r < table->rowCount(); ++r)
      for (int c = 0; c < table->columnCount(); ++c)
        if (QWidget *w = table->cellWidget(r, c))
        {
          table->removeCellWidget(r, c);
          w->setParent(nullptr);
          w->deleteLater();
        }
  }
} // namespace

EntityPanel::EntityPanel(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("entityViewSection")); // 壳按名摘挂到右 dock
  setMinimumWidth(0);
  auto *entityLay = new QVBoxLayout(this);
  entityLay->setContentsMargins(0, 0, 0, 0);
  entityLay->setSpacing(PaleoTheme::tokens().spacingSm);
  entityLay->setAlignment(Qt::AlignTop);
  // ---- p5a：实体角色槽与资产属性视图 ----
  entityLay->addWidget(caption(tr("数据属性与设置"), this));
  auto *viewEmpty = new QLabel(this);
  viewEmpty->setObjectName(QStringLiteral("entityViewEmptyLabel"));
  viewEmpty->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(viewEmpty, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  entityLay->addWidget(viewEmpty);

  auto *viewContent = new QWidget(this);
  viewContent->setObjectName(QStringLiteral("entityViewContent"));
  auto *vcl = new QVBoxLayout(viewContent);
  vcl->setContentsMargins(0, 0, 0, 0);
  vcl->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *entityHeader = new QLabel(viewContent);
  entityHeader->setObjectName(QStringLiteral("entityViewHeader"));
  // 同值标签：实体名（往往是长文件名）不顶宽面板，宽度内换行。
  entityHeader->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  entityHeader->setWordWrap(true);
  // 数据属性标题使用中性色，交互蓝只用于主操作与选中状态。
  PaleoTheme::applyThemedStyleSheet(entityHeader, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QLabel { "
               "  background: %1; "
               "  color: %2; "
               "  font-weight: 600; "
               "  font-size: {typography.title}pt; "
               "  padding: {spacing.sm}px {spacing.md}px; "
               "  border-radius: {rounded.sm}px; "
               "  border: 1px solid %3; "
               "}"))
        .arg(t.surfaceAltRaised.name().toUpper(),
             t.text.name().toUpper(),
             t.border.name().toUpper());
  });
  vcl->addWidget(entityHeader);
  m_mappingReference = new QPushButton(tr("在编图中联动参考"), viewContent);
  m_mappingReference->setObjectName(QStringLiteral("dataMappingReference"));
  m_mappingReference->setEnabled(false);
  m_mappingReference->setToolTip(tr("请先选择已保存的编图或单因素图件版本"));
  vcl->addWidget(m_mappingReference);
  connect(m_mappingReference, &QPushButton::clicked, this, [this] {
    if (!m_mappingReferenceVersion.isEmpty())
      emit mappingReferenceRequested(m_mappingReferenceVersion);
  });

  auto *scroll = new QScrollArea(viewContent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *scrollContainer = new QWidget(scroll);
  auto *sl = new QVBoxLayout(scrollContainer);
  sl->setContentsMargins(0, 0, 0, 0);
  sl->setSpacing(PaleoTheme::tokens().spacingSm);

  const auto addRow = [](CollapsibleSection *sec, QFormLayout *fl, const QString &label, const char *valName) -> QLabel * {
    auto *lbl = new QLabel(label, sec->container());
    PaleoTheme::applyThemedStyleSheet(lbl, [] {
      return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt;"))
          .arg(PaleoTheme::tokens().textMuted.name().toUpper());
    });
    auto *val = new QLabel(QStringLiteral("—"), sec->container());
    val->setObjectName(QLatin1String(valName));
    PaleoTheme::applyThemedStyleSheet(val, [] {
      return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt; font-weight: 500;"))
          .arg(PaleoTheme::tokens().text.name().toUpper());
    });
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 内容永不下推面板宽度：值标签可压到 0（Ignored），长文本在面板现有
    // 宽度内换行/裁切，而不是把「数据属性」dock 撑宽。
    val->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    fl->addRow(lbl, val);
    return val;
  };

  // 1. 基本信息
  auto *secBasic = new CollapsibleSection(tr("基本信息"), scrollContainer);
  secBasic->setObjectName(QStringLiteral("secBasic"));
  auto *formBasic = new QFormLayout();
  secBasic->containerLayout()->addLayout(formBasic);
  formBasic->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs);
  formBasic->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *nameVal = addRow(secBasic, formBasic, tr("名称:"), "propName");
  nameVal->setWordWrap(true);
  addRow(secBasic, formBasic, tr("类型:"), "propType");
  addRow(secBasic, formBasic, tr("格式:"), "propFormat");
  auto *pathVal = addRow(secBasic, formBasic, tr("路径:"), "propPath");
  pathVal->setWordWrap(true);
  addRow(secBasic, formBasic, tr("当前版本:"), "propVersion");
  addRow(secBasic, formBasic, tr("状态:"), "propStatus");
  sl->addWidget(secBasic);

  // 2. 空间与几何
  auto *secSpatial = new CollapsibleSection(tr("空间与几何"), scrollContainer);
  secSpatial->setObjectName(QStringLiteral("secSpatial"));
  auto *formSpatial = new QFormLayout();
  secSpatial->containerLayout()->addLayout(formSpatial);
  formSpatial->setContentsMargins(PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingXs);
  formSpatial->setSpacing(PaleoTheme::tokens().spacingXs);
  addRow(secSpatial, formSpatial, tr("坐标系:"), "propCrs");
  auto *coordVal = addRow(secSpatial, formSpatial, tr("坐标/范围:"), "propCoord");
  coordVal->setWordWrap(true);
  addRow(secSpatial, formSpatial, tr("深度/时间:"), "propZRange");
  addRow(secSpatial, formSpatial, tr("采样/规格:"), "propGrid");
  sl->addWidget(secSpatial);

  // 3. 业务角色与关联
  auto *secRoles = new CollapsibleSection(tr("业务角色与关联"), scrollContainer);
  secRoles->setObjectName(QStringLiteral("secRoles"));
  auto *rl = secRoles->containerLayout();
  auto *roleSummary = new QLabel(secRoles->container());
  roleSummary->setObjectName(QStringLiteral("propRoleSummary"));
  roleSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  roleSummary->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(roleSummary, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt; margin-bottom: {spacing.xs}px;"))
        .arg(PaleoTheme::tokens().textMuted.name().toUpper());
  });
  rl->addWidget(roleSummary);
  auto *roleTable = new QTableWidget(0, 4, secRoles->container());
  roleTable->setObjectName(QStringLiteral("entityRoleTable"));
  roleTable->setAccessibleName(tr("实体角色槽"));
  roleTable->setHorizontalHeaderLabels(
      {tr("角色"), tr("主关联"), tr("其他成员"), tr("未决")});
  roleTable->verticalHeader()->setVisible(false);
  roleTable->horizontalHeader()->setStretchLastSection(true);
  roleTable->setMinimumHeight(160);
  rl->addWidget(roleTable);
  sl->addWidget(secRoles);

  // 4. 属性明细 / 特征
  auto *secDetails = new CollapsibleSection(tr("属性明细 / 特征"), scrollContainer);
  secDetails->setObjectName(QStringLiteral("secDetails"));
  auto *dl = secDetails->containerLayout();
  auto *detailsText = new QLabel(secDetails->container());
  detailsText->setObjectName(QStringLiteral("propDetailsText"));
  detailsText->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  PaleoTheme::applyThemedStyleSheet(detailsText, [] {
    return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt;"))
        .arg(PaleoTheme::tokens().text.name().toUpper());
  });
  detailsText->setWordWrap(true);
  dl->addWidget(detailsText);
  sl->addWidget(secDetails);

  // 5. 下游派生产物与版本
  auto *secDerived = new CollapsibleSection(tr("下游派生产物与版本"), scrollContainer);
  secDerived->setObjectName(QStringLiteral("secDerived"));
  auto *derLay = secDerived->containerLayout();
  auto *derivedTable = new QTableWidget(0, 3, secDerived->container());
  derivedTable->setObjectName(QStringLiteral("derivedProductsTable"));
  derivedTable->setAccessibleName(tr("下游派生产物"));
  derivedTable->setHorizontalHeaderLabels({tr("产物"), tr("版本"), tr("状态")});
  derivedTable->verticalHeader()->setVisible(false);
  derivedTable->horizontalHeader()->setStretchLastSection(true);
  derivedTable->setMinimumHeight(100);
  derLay->addWidget(derivedTable);
  auto *missing = new QLabel(secDerived->container());
  missing->setObjectName(QStringLiteral("missingSourcesLabel"));
  missing->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  missing->setWordWrap(true);
  missing->hide(); // 悬空血缘诊断只在 missingSources 非空时出现
  derLay->addWidget(missing);
  sl->addWidget(secDerived);

  sl->addStretch(1);
  scroll->setWidget(scrollContainer);
  vcl->addWidget(scroll, 1);
  entityLay->addWidget(viewContent, 1);

  buildD4Ui(); // P3 D4：CRUD 条/版本时间线/拓扑图/统计段

  entityLay->addStretch(); // Absorb unused height while the content is hidden.
  refresh(); // 初始空态（未选实体）：指引行，不留白板
}

// ===========================================================================
// P3 D4：实体/属性面板增强（wave/data-page-operations）
// ===========================================================================

void EntityPanel::buildD4Ui()
{
  using namespace paleo::dataops;
  auto *content = findChild<QWidget *>(QStringLiteral("entityViewContent"));
  if (!content)
    return;
  auto *vcl = static_cast<QVBoxLayout *>(content->layout());

  // ---- D4.6 实体 CRUD 工具条（表头之下）----
  auto *crud = new QWidget(content);
  crud->setObjectName(QStringLiteral("entityCrudBar"));
  auto *cl = new QHBoxLayout(crud);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(PaleoTheme::tokens().spacingXs);
  const struct
  {
    const char *name;
    const char *text;
    const char *tip;
  } kBtns[] = {
    {"entityCreateButton", QT_TR_NOOP("新建实体"),
     QT_TR_NOOP("新建井/辅助资料/地震工区实体")},
    {"entityRenameButton", QT_TR_NOOP("重命名"),
     QT_TR_NOOP("重命名当前实体（F2）——写视图层改写表，可撤销")},
    {"entityDeleteButton", QT_TR_NOOP("删除"),
     QT_TR_NOOP("删除当前实体（软删可恢复；资产处置可选）")},
    {"entityTopologyButton", QT_TR_NOOP("拓扑"),
     QT_TR_NOOP("实体↔资产关联拓扑图")},
    {"entityHistoryButton", QT_TR_NOOP("历史"),
     QT_TR_NOOP("本面板会话内的最近操作")},
  };
  const auto handlers = std::tuple{
      [this] { beginCreateEntity(); },
      [this] { if (!m_entityId.isEmpty()) beginRenameEntity(m_entityId); },
      [this] { if (!m_entityId.isEmpty()) beginDeleteEntity(m_entityId); },
      [this] { emit statusMessage(tr("拓扑图在「关联拓扑」段实时刷新")); },
      [this] {
        OperationsHistoryDialog dlg(this);
        dlg.setEntries(m_history ? m_history->entries() : QStringList());
        dlg.exec();
      },
  };
  int bi = 0;
  for (const auto &b : kBtns)
  {
    auto *btn = new QToolButton(crud);
    btn->setObjectName(QLatin1String(b.name));
    btn->setText(tr(b.text));
    btn->setToolTip(tr(b.tip));
    btn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
    // D6.7 焦点指示强化：可聚焦 + 强焦点策略（焦点环走全局 QSS）。
    btn->setFocusPolicy(Qt::StrongFocus);
    const auto h = handlers;
    const int idx = bi++;
    // std::tuple 元素按序接线（编译期分发）。
    [btn, &h, idx, this](auto seq) {
      constexpr int I = decltype(seq)::value;
      if (I == idx)
        connect(btn, &QToolButton::clicked, this, std::get<I>(h));
    }(std::integral_constant<int, 0>{});
    [btn, &h, idx, this](auto seq) {
      constexpr int I = decltype(seq)::value;
      if (I == idx)
        connect(btn, &QToolButton::clicked, this, std::get<I>(h));
    }(std::integral_constant<int, 1>{});
    [btn, &h, idx, this](auto seq) {
      constexpr int I = decltype(seq)::value;
      if (I == idx)
        connect(btn, &QToolButton::clicked, this, std::get<I>(h));
    }(std::integral_constant<int, 2>{});
    [btn, &h, idx, this](auto seq) {
      constexpr int I = decltype(seq)::value;
      if (I == idx)
        connect(btn, &QToolButton::clicked, this, std::get<I>(h));
    }(std::integral_constant<int, 3>{});
    [btn, &h, idx, this](auto seq) {
      constexpr int I = decltype(seq)::value;
      if (I == idx)
        connect(btn, &QToolButton::clicked, this, std::get<I>(h));
    }(std::integral_constant<int, 4>{});
    cl->addWidget(btn);
  }
  cl->addStretch(1);
  // 插在实体头之后（vcl 第 0 项是 entityHeader，第 1 项是 scroll）。
  vcl->insertWidget(1, crud);

  // ---- D4.3 版本时间线 + D4.5 拓扑图：进 scroll 容器（secDerived 之后）----
  auto *derived = findChild<QTableWidget *>(QStringLiteral("derivedProductsTable"));
  if (derived)
  {
    QWidget *host = derived->parentWidget();
    while (host && host->objectName() != QLatin1String("secDerived") &&
           host != content)
      host = host->parentWidget();
    QVBoxLayout *sl = host && host != content
                          ? static_cast<QVBoxLayout *>(host->layout())
                          : nullptr;
    if (sl)
    {
      auto *secVersion = new CollapsibleSection(tr("版本时间线"), host);
      secVersion->setObjectName(QStringLiteral("secVersion"));
      m_timeline = new VersionTimeline(secVersion->container());
      secVersion->containerLayout()->addWidget(m_timeline);
      sl->addWidget(secVersion);
      // D4.4 版本 diff：清单级（大小/SHA/字段差异）。
      connect(m_timeline, &VersionTimeline::diffRequested, this,
              [this](const QString &va, const QString &vb) {
                if (!m_doc || !m_doc->catalog())
                  return;
                DataCatalog *cat = m_doc->catalog();
                const CatalogVersion a = cat->versionById(va);
                const CatalogVersion b = cat->versionById(vb);
                if (a.id.isEmpty() || b.id.isEmpty())
                  return;
                VersionDiffDialog dlg(this);
                dlg.showDiff(diffVersions(a, b, m_doc->absolutePathForVersion(a),
                                          m_doc->absolutePathForVersion(b)));
                dlg.exec();
              });

      auto *secTopo = new CollapsibleSection(tr("关联拓扑"), host);
      secTopo->setObjectName(QStringLiteral("secTopo"));
      m_topology = new TopologyGraph(secTopo->container());
      m_topology->setMaximumHeight(220);
      secTopo->containerLayout()->addWidget(m_topology);
      // D4.5 点击节点 → 定位（资产 → assetFocus；实体 → entitiesFocus 由壳接）。
      connect(m_topology, &TopologyGraph::nodeClicked, this,
              [this](const QString &id, bool isEntity) {
                if (isEntity)
                  emit statusMessage(tr("拓扑：实体 %1（地图联动经壳）").arg(id));
                else
                  emit statusMessage(tr("拓扑：资产 %1").arg(id));
              });
      sl->addWidget(secTopo);

      auto *secLineage = new CollapsibleSection(tr("衍生血缘"), host);
      secLineage->setObjectName(QStringLiteral("secDerivation"));
      m_derivation = new DerivationPanel(secLineage->container());
      secLineage->containerLayout()->addWidget(m_derivation);
      sl->addWidget(secLineage);
      connect(m_derivation, &DerivationPanel::queryChanged, this, &EntityPanel::refreshDerivation);
      connect(m_derivation, &DerivationPanel::nodeClicked, this, [this](const QString &id) {
        m_graphSelected = id;
        m_derivation->graphView()->highlight(id, paleo::derivation::Service::selectionClosure(
            m_doc ? m_doc->catalog() : nullptr, m_derivationData, id));
        m_preserveGraphOnce = true;
        const QPointer<EntityPanel> self(this);
        emit versionActivated(id);
        if (self) self->m_preserveGraphOnce = false; // 定位槽可销毁宿主；无接收者也不得残留。
      });
      connect(m_timeline, &VersionTimeline::versionActivated, this, &EntityPanel::versionActivated);

      // D4.8 统计段：当前上下文的资产计数摘要。
      auto *secStats = new CollapsibleSection(tr("统计"), host);
      secStats->setObjectName(QStringLiteral("secStats"));
      auto *statsLabel = new QLabel(secStats->container());
      statsLabel->setObjectName(QStringLiteral("propStatsText"));
      statsLabel->setWordWrap(true);
      PaleoTheme::applyThemedStyleSheet(statsLabel, [] {
        return PaleoTheme::metricStyleSheet(QStringLiteral("color: %1; font-size: {typography.label}pt;"))
            .arg(PaleoTheme::tokens().text.name().toUpper());
      });
      secStats->containerLayout()->addWidget(statsLabel);
      sl->addWidget(secStats);
    }
  }

  // D4.7 角色表双击 → 编辑挂接角色（资产上下文）。
  if (auto *roleTable = findChild<QTableWidget *>(QStringLiteral("entityRoleTable")))
    connect(roleTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
      if (m_assetId.isEmpty() || !m_ctx.valid())
        return;
      DataCatalog *cat = m_ctx.cat;
      const QVector<EntityAssetLink> links = cat->linksForAsset(m_assetId);
      if (row < 0 || row >= links.size())
        return;
      const EntityAssetLink &l = links.at(row);
      if (l.unresolved)
        return;
      QStringList vocab;
      for (const RoleDef &d : cat->roleRegistry().forEntity(l.entityType))
        vocab << d.role;
      if (!vocab.contains(l.role))
        vocab << l.role;
      vocab.sort();
      RoleEditDialog dlg(this);
      dlg.loadRole(l.role, vocab);
      if (dlg.exec() != QDialog::Accepted || dlg.newRole() == l.role)
        return;
      // D5.5：addLink 无删除对偶 → 不可撤销确认。
      if (!PaleoNotify::ask(
              this, tr("角色变更（不可撤销）"),
              tr("将为「%1」新增角色关联 %2（原 %3 关联保留）。\n"
                 "此操作不可撤销。继续？")
                  .arg(m_assetId, dlg.newRole(), l.role),
              PaleoNotify::AskButtons::YesNo, PaleoNotify::AskDefault::Platform,
              PaleoNotify::AskIcon::Warning))
        return;
      EntityAssetLink nl = l;
      nl.role = dlg.newRole();
      nl.isPrimary = false;
      QString err;
      if (cat->addLink(nl, &err))
      {
        if (m_history)
          m_history->push(tr("角色变更 %1：%2→%3")
                              .arg(m_assetId, l.role, dlg.newRole()));
        refresh();
        emit entityRefreshRequested();
        emit statusMessage(tr("已新增角色关联 %1（不可撤销）").arg(dlg.newRole()));
      }
      else
        emit statusMessage(tr("角色变更失败：%1").arg(err));
    });
}

void EntityPanel::setSharedOps(const paleo::dataops::DataOpsContext &ctx,
                               paleo::dataops::DataOpsUndoStack *stack,
                               paleo::dataops::OperationsHistory *history)
{
  m_ctx = ctx;
  m_stack = stack;
  m_history = history;
}

void EntityPanel::setMultiContext(const QStringList &entityIds,
                                  const QStringList &assetIds)
{
  m_multiEntityIds = entityIds;
  m_multiAssetIds = assetIds;
  refresh();
}

// D4.9 多选批量概要。
void EntityPanel::refreshMultiSummary()
{
  auto *content = findChild<QWidget *>(QStringLiteral("entityViewContent"));
  auto *empty = findChild<QLabel *>(QStringLiteral("entityViewEmptyLabel"));
  auto *header = findChild<QLabel *>(QStringLiteral("entityViewHeader"));
  if (!content || !empty || !header)
    return;
  empty->setVisible(false);
  content->setVisible(true);
  header->setText(tr("已选 %1 个资产 / %2 个实体（批量概要）")
                      .arg(m_multiAssetIds.size())
                      .arg(m_multiEntityIds.size()));
  DataCatalog *cat = m_doc ? m_doc->catalog() : nullptr;
  QMap<QString, int> byType, byStatus;
  QSet<QString> commonEntities;
  bool first = true;
  QStringList allTags;
  int totalVersions = 0;
  const paleo::dataops::EntityOverrideStore *ov = m_ctx.entityOverrides;
  for (const QString &id : m_multiAssetIds)
  {
    const CatalogAsset a = cat ? cat->assetById(id) : CatalogAsset();
    if (a.id.isEmpty())
      continue;
    byType[a.type] += 1;
    const CatalogVersion v = cat->currentVersion(id);
    byStatus[v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage] += 1;
    totalVersions += cat->versionsForAsset(id).size();
    QSet<QString> ents;
    for (const EntityAssetLink &l : cat->linksForAsset(id))
      if (!l.unresolved && !l.entityId.isEmpty())
        ents.insert(l.entityId);
    commonEntities = first ? ents : (commonEntities & ents);
    first = false;
  }
  if (ov)
    for (const QString &e : m_multiEntityIds)
    {
      const CatalogEntity ent = cat ? cat->entityById(e) : CatalogEntity();
      if (!ent.id.isEmpty())
        byType[tr("实体:") + ent.entityType] += 1;
    }
  QStringList typeParts, statusParts;
  for (auto it = byType.constBegin(); it != byType.constEnd(); ++it)
    typeParts << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value());
  for (auto it = byStatus.constBegin(); it != byStatus.constEnd(); ++it)
    statusParts << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value());
  QStringList entsShown;
  if (cat)
    for (const QString &e : commonEntities)
    {
      const CatalogEntity ent = cat->entityById(e);
      entsShown << (ov ? ov->displayName(ent) : ent.name);
    }
  if (auto *stats = findChild<QLabel *>(QStringLiteral("propStatsText")))
    stats->setText(tr("批量概要：%1").arg(typeParts.join(QStringLiteral("、"))));
  if (auto *details = findChild<QLabel *>(QStringLiteral("propDetailsText")))
    details->setText(
        tr("类型分布：%1\n状态分布：%2\n版本总数：%3\n共同实体：%4")
            .arg(typeParts.join(QStringLiteral("、")),
                 statusParts.join(QStringLiteral("、")))
            .arg(totalVersions)
            .arg(entsShown.isEmpty() ? tr("无") : entsShown.join(QStringLiteral("、"))));
  if (auto *roleTable = findChild<QTableWidget *>(QStringLiteral("entityRoleTable")))
  {
    clearCellWidgets(roleTable);
    roleTable->setRowCount(0);
  }
  if (auto *derived = findChild<QTableWidget *>(QStringLiteral("derivedProductsTable")))
    derived->setRowCount(0);
  if (m_timeline)
    m_timeline->loadVersions({});
}

// ---- D4.6 实体 CRUD ---------------------------------------------------------
void EntityPanel::beginCreateEntity()
{
  using namespace paleo::dataops;
  if (!m_ctx.valid())
  {
    emit statusMessage(tr("工程未打开——无法新建实体"));
    return;
  }
  EntityCreateDialog dlg(this);
  // 重名校验（D4.1 同口径：井名唯一性按 catalog 现有名）。
  const QString type = dlg.chosenType();
  Q_UNUSED(type);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString name = dlg.chosenName();
  if (name.isEmpty())
  {
    emit statusMessage(tr("实体名不能为空"));
    return;
  }
  for (const CatalogEntity &e : m_ctx.cat->entities(dlg.chosenType()))
  {
    const QString existing = m_ctx.entityOverrides
                                 ? m_ctx.entityOverrides->displayName(e)
                                 : e.name;
    if (existing.compare(name, Qt::CaseInsensitive) == 0)
    {
      PaleoNotify::warning(this, tr("重名"),
                           tr("已存在同名实体「%1」").arg(name));
      return;
    }
  }
  CatalogEntity e;
  e.entityType = dlg.chosenType();
  e.name = name;
  e.hasSurface = e.entityType == QLatin1String("well") ||
                 e.entityType == QLatin1String("planned"); // 计划井带井口坐标
  e.surfaceX = dlg.chosenX();
  e.surfaceY = dlg.chosenY();
  e.id = m_ctx.cat->nextEntityId(e.entityType == QLatin1String("well")
                                     ? QStringLiteral("well")
                                 : e.entityType == QLatin1String("planned")
                                     ? QStringLiteral("planned")
                                     : QStringLiteral("aux"));
  if (m_stack)
  {
    m_stack->push(new EntityCreateCmd(m_ctx, e));
    if (m_history)
      m_history->push(tr("新建实体 %1").arg(name));
  }
  else
  {
    QString err;
    if (!m_ctx.cat->addEntity(e, &err))
    {
      emit statusMessage(tr("新建实体失败：%1").arg(err));
      return;
    }
  }
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("已新建实体「%1」（可撤销）").arg(name));
}

void EntityPanel::beginRenameEntity(const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  const CatalogEntity e = m_ctx.cat->entityById(entityId);
  if (e.id.isEmpty())
    return;
  EntityEditDialog dlg(this);
  dlg.loadEntity(e, m_ctx.entityOverrides ? m_ctx.entityOverrides->overrideFor(e.id)
                                          : EntityOverride());
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString newName = dlg.editedName();
  // D4.1 重名校验：新名不与其它同类实体撞名。
  for (const CatalogEntity &o : m_ctx.cat->entities(e.entityType))
    if (o.id != e.id)
    {
      const QString existing = m_ctx.entityOverrides
                                   ? m_ctx.entityOverrides->displayName(o)
                                   : o.name;
      if (existing.compare(newName, Qt::CaseInsensitive) == 0)
      {
        PaleoNotify::warning(this, tr("重名"),
                             tr("已存在同名实体「%1」").arg(newName));
        return;
      }
    }
  EntityOverride next;
  next.name = newName;
  next.hasCoords = true;
  next.surfaceX = dlg.editedX();
  next.surfaceY = dlg.editedY();
  next.note = dlg.editedNote();
  if (m_stack)
  {
    m_stack->push(new EntityEditCmd(m_ctx, e.id, next,
                                    m_ctx.entityOverrides
                                        ? m_ctx.entityOverrides->overrideFor(e.id)
                                        : EntityOverride(),
                                    false));
    if (m_history)
      m_history->push(tr("编辑实体 %1").arg(newName));
  }
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("实体「%1」已更新（可撤销）").arg(newName));
}

void EntityPanel::beginDeleteEntity(const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  const CatalogEntity e = m_ctx.cat->entityById(entityId);
  if (e.id.isEmpty())
    return;
  const int assetCount = int(m_ctx.cat->linksForEntity(entityId).size());
  EntityDeleteDialog dlg(assetCount, this);
  if (dlg.exec() != QDialog::Accepted)
    return;
  if (dlg.assetsToRecycle())
  {
    // 资产一并软删。
    for (const EntityAssetLink &l : m_ctx.cat->linksForEntity(entityId))
    {
      const CatalogAsset a = m_ctx.cat->assetById(l.assetId);
      if (m_stack)
        m_stack->push(new SoftDeleteCmd(m_ctx, a.id, a.displayName, a.type, true));
    }
  }
  else
  {
    // 保留资产：解除全部关联（可撤销）。
    for (const EntityAssetLink &l : m_ctx.cat->linksForEntity(entityId))
      if (m_stack && !l.unresolved)
        m_stack->push(new DetachLinkCmd(m_ctx, l.assetId, l.role, l.entityId));
  }
  // 实体本身：sidecar 软删（可恢复；catalog 无 removeEntity API——GAPS）。
  if (m_stack)
    m_stack->push(new SoftDeleteCmd(m_ctx, e.id, e.name, e.entityType, true));
  if (m_history)
    m_history->push(tr("删除实体 %1（软删）").arg(e.name));
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("实体「%1」已移入可回收清单（可撤销）").arg(e.name));
}

void EntityPanel::setDocService(PreviewDocService *doc)
{
  m_doc = doc; m_versionId.clear(); m_graphSelected.clear(); m_preserveGraphOnce = false;
  refreshDerivation();
}
void EntityPanel::setVersionContext(const QString &assetId, const QString &versionId)
{
  m_assetId = assetId; m_versionId = versionId;
  m_entityId = m_doc ? m_doc->entityIdForAsset(assetId) : QString();
  // 图内点选仍保留当前图及无关分支的降透明度；预览外部换版本重建子图。
  m_preserveGraphOnce = m_preserveGraphOnce && m_graphSelected == versionId;
  if (!m_preserveGraphOnce) m_graphSelected = versionId;
  m_multiAssetIds.clear(); m_multiEntityIds.clear();
  refresh();
}
void EntityPanel::refreshDerivation()
{
  if (!m_derivation) return;
  if (m_preserveGraphOnce) { m_preserveGraphOnce = false; return; }
  auto query = m_derivation->query();
  query.entityId = m_entityId; query.assetId = m_assetId; query.versionId = m_versionId;
  if (m_multiAssetIds.size() > 1) { query.entityId.clear(); query.assetId.clear(); query.versionId.clear(); }
  m_derivationData = paleo::derivation::Service::build(m_doc ? m_doc->catalog() : nullptr, query);
  m_derivation->setGraph(m_derivationData);
  if (!m_graphSelected.isEmpty())
    m_derivation->graphView()->highlight(m_graphSelected, paleo::derivation::Service::selectionClosure(
        m_doc ? m_doc->catalog() : nullptr, m_derivationData, m_graphSelected));
}

void EntityPanel::setContext(const QString &entityId, const QString &assetId)
{
  m_entityId = entityId;
  m_assetId = assetId;
  m_versionId.clear(); m_graphSelected.clear(); m_preserveGraphOnce = false;
  // 单一上下文覆盖多选批量概要态（D4.9 的回落路径）。
  m_multiAssetIds.clear();
  m_multiEntityIds.clear();
}

void EntityPanel::refresh()
{
  m_mappingReferenceVersion.clear();
  m_mappingReference->setEnabled(false);
  m_mappingReference->setToolTip(tr("请先选择已保存的编图或单因素图件版本"));
  refreshDerivation();
  QWidget *root = this;
  if (!root)
    return;
  // D4.9 多选态：批量概要（多资产选中时优先生效；单选/空选回落常规三态）。
  if (m_multiAssetIds.size() > 1)
  {
    refreshMultiSummary();
    return;
  }
  m_multiAssetIds.clear();
  m_multiEntityIds.clear();
  auto *content = child<QWidget>(root, "entityViewContent");
  auto *empty = child<QLabel>(root, "entityViewEmptyLabel");
  auto *header = child<QLabel>(root, "entityViewHeader");
  auto *roleTable = child<QTableWidget>(root, "entityRoleTable");
  auto *derived = child<QTableWidget>(root, "derivedProductsTable");
  auto *missing = child<QLabel>(root, "missingSourcesLabel");
  if (!content || !empty || !header || !roleTable || !derived || !missing)
    return;

  auto *propName = child<QLabel>(root, "propName");
  auto *propType = child<QLabel>(root, "propType");
  auto *propFormat = child<QLabel>(root, "propFormat");
  auto *propPath = child<QLabel>(root, "propPath");
  auto *propVersion = child<QLabel>(root, "propVersion");
  auto *propStatus = child<QLabel>(root, "propStatus");
  auto *propCrs = child<QLabel>(root, "propCrs");
  auto *propCoord = child<QLabel>(root, "propCoord");
  auto *propZRange = child<QLabel>(root, "propZRange");
  auto *propGrid = child<QLabel>(root, "propGrid");
  auto *propRoleSummary = child<QLabel>(root, "propRoleSummary");
  auto *propDetailsText = child<QLabel>(root, "propDetailsText");

  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  const QString entityId = m_entityId;
  const QString assetId = m_assetId;

  // 空态：工程未开
  if (!cat || !cat->isOpen())
  {
    empty->setText(tr("工程还没打开 — 打开工程后在左侧列表选择实体，"
                      "这里显示它的角色槽数据全貌"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 实体与资产均未选：指引下一步（地图/列表点选）
  if (entityId.isEmpty() && assetId.isEmpty())
  {
    empty->setText(tr("在左侧数据列表选择实体或资产，这里按角色词表显示"
                      "它的数据全貌与派生产物"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 情况 1：选中了测区全景
  if (assetId == QLatin1String("survey_area"))
  {
    empty->setVisible(false);
    content->setVisible(true);
    header->setText(tr("测区全景 (Survey Area)"));
    if (propName) propName->setText(tr("工区全景与空间范围"));
    if (propType) propType->setText(tr("测区全景地图 (Survey Map)"));
    if (propFormat) propFormat->setText(tr("QGIS 地图工程"));
    if (propPath) propPath->setText(cat->catalogPath());
    if (propVersion) propVersion->setText(tr("当前工程"));
    if (propStatus) propStatus->setText(tr("已加载 · 双击打开全景地图"));

    const QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
    const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
    CatalogEntity survey = surveys.isEmpty() ? CatalogEntity() : surveys.front();
    if (propCrs) propCrs->setText(tr("工区三维测网坐标系 (米)"));
    if (propCoord)
    {
      if (!survey.corners.isEmpty())
      {
        propCoord->setText(tr("角点 1: (%1, %2)\n角点 2: (%3, %4)")
                               .arg(QString::number(survey.corners.first().first, 'f', 1))
                               .arg(QString::number(survey.corners.first().second, 'f', 1))
                               .arg(QString::number(survey.corners.last().first, 'f', 1))
                               .arg(QString::number(survey.corners.last().second, 'f', 1)));
      }
      else if (survey.inlineMax > survey.inlineMin)
      {
        propCoord->setText(tr("Inline: %1 ~ %2\nCrossline: %3 ~ %4")
                               .arg(int(survey.inlineMin))
                               .arg(int(survey.inlineMax))
                               .arg(int(survey.xlineMin))
                               .arg(int(survey.xlineMax)));
      }
      else
      {
        propCoord->setText(tr("工区全景坐标覆盖"));
      }
    }
    if (propZRange) propZRange->setText(tr("多测线 / 多层位 / 测井联合空间"));
    if (propGrid) propGrid->setText(tr("总井数: %1 口").arg(wells.size()));
    if (propRoleSummary) propRoleSummary->setText(tr("包含工区所有井位、地震工区范围、构造解释层位及辅助地质底图"));

    if (propDetailsText)
    {
      QStringList details;
      details << tr("数据目录: %1").arg(cat->catalogPath());
      details << tr("井实体数: %1 口").arg(wells.size());
      details << tr("地震工区数: %1 个").arg(surveys.size());
      propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    clearCellWidgets(roleTable);
    roleTable->setRowCount(0);
    derived->setRowCount(0);
    missing->hide();
    return;
  }

  // 情况 2：选中了具体资产（例如地震体、层位、测井文件、GeoJSON相图、参考资料等）
  if (!assetId.isEmpty())
  {
    const CatalogAsset a = cat->assetById(assetId);
    if (a.id.isEmpty())
    {
      empty->setText(tr("所选资产不在目录中：%1").arg(assetId));
      empty->setVisible(true);
      content->setVisible(false);
      return;
    }

    empty->setVisible(false);
    content->setVisible(true);
    const CatalogVersion v = m_versionId.isEmpty() ? cat->currentVersion(a.id) : m_doc->versionForPreview(m_versionId);
    const QString abs = svc ? svc->absolutePathForVersion(v) : QString();
    if (v.extra.value(QStringLiteral("mapping_product")).toBool() &&
        (v.extra.value(QStringLiteral("layer_type")) ==
             QLatin1String("vector") ||
         v.extra.value(QStringLiteral("layer_type")) ==
             QLatin1String("raster"))) {
      m_mappingReferenceVersion = v.id;
      m_mappingReference->setEnabled(!v.id.isEmpty());
      m_mappingReference->setToolTip(
          tr("引用 %1 · v%2；重算后仍保留所选版本，不切换当前编辑图件")
              .arg(a.displayName)
              .arg(v.versionNumber));
    }

    QString typeDisplay = a.type;
    QString formatDisplay = tr("未知格式");
    const bool isGeoJson = a.type == QLatin1String("geojson") ||
                           a.type == QLatin1String("boundary") ||
                           a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive);

    if (v.extra.value(QStringLiteral("mapping_product")).toBool()) {
      typeDisplay = tr("编图 / 单因素图件");
      const auto format = v.fileName.section(QLatin1Char('.'), -1).toUpper();
      formatDisplay = tr("%1 · %2").arg(
          format.isEmpty() ? a.format.toUpper() : format,
          v.extra.value(QStringLiteral("layer_type")) == QLatin1String("vector")
              ? tr("矢量")
              : tr("栅格"));
      header->setText(
          v.extra.value(QStringLiteral("title"), a.displayName).toString());
    } else if (a.type == QLatin1String("seismic")) {
      typeDisplay = tr("三维地震数据体 (3D Seismic)");
      formatDisplay = tr("SEG-Y rev1.0 (IEEE/IBM FP32)");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("三维地震")));
    } else if (a.type == QLatin1String("horizon")) {
      typeDisplay = tr("解释层位 (Horizon Grid)");
      formatDisplay = tr("CPS-3 / ZMAP ASCII");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("解释层位")));
    } else if (a.type == QLatin1String("well_log")) {
      typeDisplay = tr("测井曲线 (Well Log)");
      formatDisplay = tr("CWLS LAS 2.0");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("测井曲线")));
    } else if (a.type == QLatin1String("boundary")) {
      typeDisplay = tr("工区边界 (Boundary)");
      formatDisplay = tr("GeoJSON 矢量");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("工区边界")));
    } else if (isGeoJson) {
      typeDisplay = tr("参考相图 (GeoJSON 矢量)");
      formatDisplay = tr("GeoJSON");
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, tr("参考相图")));
    } else if (a.type == QLatin1String("auxiliary") ||
               a.type == QLatin1String("document")) {
      typeDisplay = tr("辅助参考资料");
      formatDisplay = a.displayName.section(QLatin1Char('.'), -1).toUpper();
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, a.type));
    } else {
      header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, a.type));
    }

    // 1. 基本信息
    if (propName) propName->setText(a.displayName);
    if (propType) propType->setText(typeDisplay);
    if (propFormat) propFormat->setText(formatDisplay);
    if (propPath) propPath->setText(v.path.isEmpty() ? tr("—") : v.path);
    if (propVersion) propVersion->setText(v.versionNumber > 0 ? tr("v%1").arg(v.versionNumber) : tr("v1"));
    if (propStatus)
    {
      if (isGeoJson && v.extra.value(QStringLiteral("provisional")).toBool())
        propStatus->setText(tr("就绪 · 临时配准"));
      else if (isGeoJson)
        propStatus->setText(tr("就绪 · 未配准"));
      else
        propStatus->setText(tr("就绪 · 可预览"));
    }

    // 2. 空间与几何
    if (a.type == QLatin1String("seismic"))
    {
      const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
      CatalogEntity survey = surveys.isEmpty() ? CatalogEntity() : surveys.front();
      if (propCrs) propCrs->setText(tr("工区三维地震测网坐标系"));
      if (propCoord)
      {
        if (survey.inlineMax > survey.inlineMin)
          propCoord->setText(tr("Inline: %1 ~ %2\nCrossline: %3 ~ %4")
              .arg(int(survey.inlineMin)).arg(int(survey.inlineMax))
              .arg(int(survey.xlineMin)).arg(int(survey.xlineMax)));
        else
          propCoord->setText(tr("三维地震数据范围"));
      }
      if (propZRange)
      {
        // 只写实测值：catalog 冻结了起始时间与采样间隔；TWT 末点依赖样本
        // 数（catalog 未存），取不到不臆造。
        if (survey.sampleIntervalUs > 0)
          propZRange->setText(tr("双程旅行时：起始 %1 ms · 采样间隔 %2 ms")
                                  .arg(survey.startTimeMs, 0, 'f', 1)
                                  .arg(survey.sampleIntervalUs / 1000.0, 0, 'f', 1));
        else
          propZRange->setText(tr("—"));
      }
      if (propGrid)
      {
        // 道数 catalog 未存——按冻结的测网范围给真实规格，取不到写「—」。
        if (survey.inlineMax > survey.inlineMin && survey.xlineMax > survey.xlineMin)
          propGrid->setText(tr("三维测网 %1 × %2（Inline × Crossline）")
                                .arg(int(survey.inlineMax - survey.inlineMin) + 1)
                                .arg(int(survey.xlineMax - survey.xlineMin) + 1));
        else
          propGrid->setText(tr("—"));
      }
    }
    else if (a.type == QLatin1String("horizon"))
    {
      if (propCrs) propCrs->setText(tr("工区局部测网坐标系（米）"));
      if (propCoord) propCoord->setText(tr("工区构造层位面网格"));
      if (propZRange) propZRange->setText(tr("双程时间 / 构造深度 (TWT)"));
      if (propGrid) propGrid->setText(tr("—")); // 网格规格 catalog 未存，不臆造
    }
    else if (isGeoJson)
    {
      // F3（goal/perf-systematize 簇2）：GeoJSON 统计（bounds/要素数/属性键）
      // 迁单遍流式门面 + 异步任务（旧路径 readAll 整读 DOM 解析两遍，大相图
      // 阻塞 UI 线程）。此处只铺静态文案，统计占位如实在场；summary 到达后
      // 统一装填（见下方 details 块尾部的一次请求，两处消费共用）。
      if (propCrs)
      {
        if (v.extra.value(QStringLiteral("provisional")).toBool())
          propCrs->setText(tr("工区局部测网（临时配准仿射变换）"));
        else
          propCrs->setText(tr("WGS 84 (经纬度) · 经纬度，与本测网不是同一空间"));
      }
      if (propCoord)
        propCoord->setText(tr("统计中…"));
      if (propZRange) propZRange->setText(tr("—（平面矢量数据）"));
      if (propGrid) propGrid->setText(tr("统计中…"));
    }
    else
    {
      if (propCrs) propCrs->setText(tr("工区局部测网坐标系（米）"));
      if (propCoord) propCoord->setText(tr("工区基准坐标"));
      if (propZRange) propZRange->setText(tr("—"));
      if (propGrid) propGrid->setText(tr("—"));
    }

    // 3. 业务角色与关联
    const QVector<EntityAssetLink> links = cat->linksForAsset(a.id);
    if (propRoleSummary)
    {
      if (links.isEmpty())
        propRoleSummary->setText(tr("独立资产（未挂接到井实体）"));
      else
        propRoleSummary->setText(tr("已挂接 %1 条业务关联").arg(links.size()));
    }
    clearCellWidgets(roleTable);
    roleTable->setRowCount(0);
    for (const EntityAssetLink &l : links)
    {
      const int r = roleTable->rowCount();
      roleTable->insertRow(r);
      auto *rItem = new QTableWidgetItem(l.role);
      rItem->setFlags(rItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 0, rItem);

      const CatalogEntity e = cat->entityById(l.entityId);
      auto *eItem = new QTableWidgetItem(e.name.isEmpty() ? l.entityId : e.name);
      eItem->setFlags(eItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 1, eItem);

      auto *mItem = new QTableWidgetItem(l.isPrimary ? tr("主关联") : tr("成员"));
      mItem->setFlags(mItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 2, mItem);

      auto *uItem = new QTableWidgetItem(l.unresolved ? tr("未决") : tr("已确认"));
      uItem->setFlags(uItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 3, uItem);
    }

    // 4. 属性明细 / 特征
    if (propDetailsText)
    {
      QStringList details;
      bool geoDetailsHandled = false; // F3：GeoJSON 详情文本由 applier 全权管理时置位
      details << tr("资产标识: %1").arg(a.id);
      details << tr("显示名称: %1").arg(a.displayName);
      if (!v.path.isEmpty())
        details << tr("存储位置: %1").arg(v.path);
      if (a.type == QLatin1String("seismic"))
      {
        details << tr("数据类型: 地震振幅数据体 (SEG-Y)");
        details << tr("振幅动态范围: 浮点连续振幅");
      }
      else if (a.type == QLatin1String("horizon"))
      {
        details << tr("层位属性: 构造解释层面");
        details << tr("数据格式: 规则网格插值曲面");
      }
      else if (isGeoJson)
      {
        // F3：详情行的 GeoJSON 段先铺占位（统计中…），非 Geo 行照常同步铺。
        // 统计到达后由此处的 applier 用 detailsBeforeGeo 重建完整详情文本。
        const QStringList detailsBeforeGeo = details;
        const bool provisional = v.extra.value(QStringLiteral("provisional")).toBool();
        const QString spatialHint = provisional
            ? tr("空间提示: 已临时配准（手工仿射变换至工区测网）")
            : tr("空间提示: 经纬度，与本测网不是同一空间（可使用「临时配准」功能）");
        details << tr("要素个数: 统计中…")
                << tr("坐标范围: 统计中…")
                << tr("属性字段: 统计中…")
                << tr("相名字段: 统计中…")
                << spatialHint;

        // 一次请求装两处（属性标签 + 详情文本；世代号随选择变化作废旧代）。
        const QPointer<EntityPanel> self(this);
        const auto applySummary =
            [self, detailsBeforeGeo, spatialHint](const PreviewDocService::GeoJsonSummary &s) {
              if (!self)
                return;
              QStringList faciesKeys;
              for (const QString &k : s.propKeys)
                if (k.contains(QString::fromUtf8("相")))
                  faciesKeys.append(k);
              if (QLabel *coord = self->findChild<QLabel *>(QStringLiteral("propCoord")))
                coord->setText(s.hasBounds
                                   ? tr("X %1–%2, Y %3–%4")
                                         .arg(QString::number(s.bounds[0], 'f', 2),
                                              QString::number(s.bounds[2], 'f', 2),
                                              QString::number(s.bounds[1], 'f', 2),
                                              QString::number(s.bounds[3], 'f', 2))
                                   : tr("未定义坐标"));
              if (QLabel *grid = self->findChild<QLabel *>(QStringLiteral("propGrid")))
                grid->setText(tr("要素个数：%1").arg(s.featureCount));
              if (QLabel *dt = self->findChild<QLabel *>(QStringLiteral("propDetailsText")))
              {
                QStringList d = detailsBeforeGeo;
                d << tr("要素个数: %1").arg(s.featureCount);
                if (s.hasBounds)
                  d << tr("坐标范围: X %1–%2, Y %3–%4")
                           .arg(QString::number(s.bounds[0], 'f', 2),
                                QString::number(s.bounds[2], 'f', 2),
                                QString::number(s.bounds[1], 'f', 2),
                                QString::number(s.bounds[3], 'f', 2));
                d << tr("属性字段: %1").arg(s.propKeys.isEmpty() ? tr("无") : s.propKeys.join(QStringLiteral(", ")));
                d << tr("相名字段: %1").arg(faciesKeys.isEmpty() ? tr("无") : faciesKeys.join(QStringLiteral(", ")));
                d << spatialHint;
                dt->setText(d.join(QStringLiteral("\n")));
              }
            };
        if (abs.isEmpty() || !QFile::exists(abs))
        {
          // 文件缺失：不留「统计中」悬置——空 summary 即回退（未定义坐标/0）。
          applySummary(PreviewDocService::GeoJsonSummary{});
          geoDetailsHandled = true;
        }
        else
        {
          geoDetailsHandled = requestGeoJsonSummary(abs, applySummary);
        }
      }
      if (!geoDetailsHandled) // F3：同步路径 applier 已装终态，跳过占位覆盖
        propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    // 5. 派生产物
    for (int r = 0; r < derived->rowCount(); ++r)
      if (QWidget *w = derived->cellWidget(r, 2))
      {
        derived->removeCellWidget(r, 2);
        w->setParent(nullptr);
        w->deleteLater();
      }
    derived->setRowCount(0);
    const QVector<CatalogVersion> allVers = cat->versionsForAsset(a.id);
    for (const CatalogVersion &ver : allVers)
    {
      if (ver.stage == QLatin1String("DERIVED") || ver.versionNumber > v.versionNumber)
      {
        const int r = derived->rowCount();
        derived->insertRow(r);
        auto *nameItem = new QTableWidgetItem(ver.fileName.isEmpty() ? a.displayName : ver.fileName);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        derived->setItem(r, 0, nameItem);
        auto *verItem = new QTableWidgetItem(tr("v%1").arg(ver.versionNumber));
        verItem->setFont(PaleoTheme::monoFont());
        verItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        derived->setItem(r, 1, verItem);
        if (ver.extra.value(QStringLiteral("stale")).toBool())
          derived->setCellWidget(
              r, 2, PaleoTheme::capsuleLabel(tr("过时"), PaleoTheme::CapsuleKind::Warning, derived));
        else if (ver.extra.value(QStringLiteral("provisional")).toBool())
          derived->setCellWidget(
              r, 2, PaleoTheme::capsuleLabel(tr("临时配准"), PaleoTheme::CapsuleKind::Neutral, derived));
        else
          derived->setItem(r, 2, mutedCell(QStringLiteral("—")));
      }
    }
    missing->hide();
    // D4.8 统计段：资产态摘要。
    if (auto *stats = findChild<QLabel *>(QStringLiteral("propStatsText")))
      stats->setText(tr("版本 %1 个（当前 v%2）· 关联 %3 条 · 未决 %4")
                         .arg(cat->versionsForAsset(a.id).size())
                         .arg(v.versionNumber > 0 ? v.versionNumber : 1)
                         .arg(links.size())
                         .arg(int(std::count_if(links.begin(), links.end(),
                                                [](const EntityAssetLink &l) {
                                                  return l.unresolved;
                                                }))));
    // P3 D4.3：资产版本时间线（新→旧版本卡）。
    if (m_timeline)
      m_timeline->loadVersions(cat->versionsForAsset(a.id));
    if (m_topology)
      m_topology->loadTopology(cat,
                               m_ctx.entityOverrides ? *m_ctx.entityOverrides
                                                     : paleo::dataops::EntityOverrideStore());
    return;
  }

  // 情况 3：选中了实体
  if (!entityId.isEmpty())
  {
    const EntityView view = entityDataView(*cat, entityId);
    if (view.entity.id.isEmpty())
    {
      empty->setText(tr("所选实体不在目录中：%1").arg(entityId));
      empty->setVisible(true);
      content->setVisible(false);
      return;
    }

    empty->setVisible(false);
    content->setVisible(true);

    // D4.1/D4.2：显示名/坐标经实体改写表（视图层 override，可撤销）。
    const paleo::dataops::EntityOverride ovr =
        m_ctx.entityOverrides ? m_ctx.entityOverrides->overrideFor(view.entity.id)
                              : paleo::dataops::EntityOverride();
    const QString title = !ovr.name.isEmpty() ? ovr.name
                          : view.entity.name.isEmpty() ? view.entity.id
                                                       : view.entity.name;
    QString kindText = tr("井实体");
    if (view.entity.entityType == QLatin1String("auxiliary"))
      kindText = tr("辅助资料");
    else if (view.entity.entityType == QLatin1String("seismic_survey"))
      kindText = tr("地震工区");
    else if (view.entity.entityType == QLatin1String("sequence_boundary"))
      kindText = tr("层序界面");
    else if (view.entity.entityType == QLatin1String("planned"))
      kindText = tr("计划井（布井候选）");
    header->setText(QStringLiteral("%1  (%2)").arg(title, kindText));

    // 1. 基本信息
    if (propName) propName->setText(title);
    if (propType)
    {
      if (view.entity.entityType == QLatin1String("auxiliary"))
        propType->setText(tr("辅助资料 (Auxiliary)"));
      else if (view.entity.entityType == QLatin1String("seismic_survey"))
        propType->setText(tr("地震工区 (Survey)"));
      else
        propType->setText(tr("井 (Well)"));
    }
    if (propFormat) propFormat->setText(tr("工程实体记录"));
    if (propPath) propPath->setText(tr("受管工程目录"));
    if (propVersion) propVersion->setText(tr("v1"));
    if (propStatus) propStatus->setText(tr("正常 · 已接入"));

    // 2. 空间与几何
    if (propCrs) propCrs->setText(tr("工区局部测网坐标系（米）"));
    if (propCoord)
    {
      if (ovr.hasCoords)
        propCoord->setText(tr("地面坐标 X: %1, Y: %2")
            .arg(QString::number(ovr.surfaceX, 'f', 2))
            .arg(QString::number(ovr.surfaceY, 'f', 2)));
      else if (view.entity.hasSurface)
        propCoord->setText(tr("地面坐标 X: %1, Y: %2")
            .arg(QString::number(view.entity.surfaceX, 'f', 2))
            .arg(QString::number(view.entity.surfaceY, 'f', 2)));
      else
        propCoord->setText(tr("未定义坐标"));
    }
    if (propZRange)
    {
      if (view.entity.td > 0)
        propZRange->setText(tr("完钻井深 %1 m (补心高 %2 m)")
            .arg(QString::number(view.entity.td, 'f', 2))
            .arg(QString::number(view.entity.kb, 'f', 2)));
      else
        propZRange->setText(tr("—"));
    }
    if (propGrid)
    {
      if (view.entity.entityType == QLatin1String("auxiliary"))
        propGrid->setText(tr("参考相图 / 辅助图件"));
      else if (view.entity.entityType == QLatin1String("seismic_survey"))
        propGrid->setText(tr("地震三维测网网格"));
      else
        propGrid->setText(tr("单井测量与轨迹"));
    }

    // 3. 业务角色与关联
    if (propRoleSummary)
      propRoleSummary->setText(tr("关联资产槽位（全 9 槽词表枚举）"));

    clearCellWidgets(roleTable);
    roleTable->setRowCount(0);
    for (const RoleSlot &slot : view.roleSlots)
    {
      const bool slotEmpty = slot.primary.assetId.isEmpty() && slot.members.isEmpty() &&
                             slot.unresolved.isEmpty();
      if (slot.def.role == QLatin1String("well_log") && !slotEmpty)
      {
        const QString roleLabel =
            slot.def.display.isEmpty() ? slot.def.role : slot.def.display;
        QVector<WellLogRow> logRows;
        if (!slot.primary.assetId.isEmpty())
          logRows.append(WellLogRow{slot.primary, false});
        for (const EntityAssetLink &m : slot.members)
          logRows.append(WellLogRow{m, false});
        for (const EntityAssetLink &u : slot.unresolved)
          logRows.append(WellLogRow{u, true});
        std::stable_sort(logRows.begin(), logRows.end(),
                         [](const WellLogRow &a, const WellLogRow &b) {
                           return a.link.ordinal < b.link.ordinal;
                         });
        for (const WellLogRow &row : logRows)
        {
          const int r = roleTable->rowCount();
          roleTable->insertRow(r);
          auto *roleItem = new QTableWidgetItem(roleLabel);
          roleItem->setFlags(roleItem->flags() & ~Qt::ItemIsEditable);
          roleTable->setItem(r, 0, roleItem);

          const CatalogAsset a = cat->assetById(row.link.assetId);
          QString shown = a.displayName.isEmpty() ? row.link.assetId : a.displayName;
          const CatalogVersion pv = cat->currentVersion(row.link.assetId);
          if (!pv.id.isEmpty())
            shown += tr(" v%1").arg(pv.versionNumber);
          if (!row.unresolved)
            shown = tr("%1 · %2").arg(shown, row.link.isPrimary ? tr("主文件") : tr("成员"));
          auto *nameItem = new QTableWidgetItem(shown);
          nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
          roleTable->setItem(r, 1, nameItem);

          if (row.unresolved)
          {
            roleTable->setItem(r, 2, mutedCell(QStringLiteral("—")));
            auto *pending = new QTableWidgetItem(tr("未决关联"));
            pending->setFlags(pending->flags() & ~Qt::ItemIsEditable);
            if (!row.link.note.isEmpty())
              pending->setToolTip(row.link.note);
            roleTable->setItem(r, 3, pending);
          }
          else if (!row.link.isPrimary)
          {
            auto *btn = new QPushButton(tr("设为主文件"), roleTable);
            btn->setObjectName(QStringLiteral("setWellLogPrimaryButton"));
            const QString linkedAssetId = row.link.assetId;
            connect(btn, &QPushButton::clicked, this, [this, entityId, linkedAssetId] {
              emit wellLogSetPrimaryRequested(entityId, linkedAssetId);
            });
            roleTable->setCellWidget(r, 2, btn);
            roleTable->setItem(r, 3, mutedCell(QStringLiteral("—")));
            roleTable->resizeRowToContents(r);
          }
          else
          {
            roleTable->setItem(r, 2, mutedCell(QStringLiteral("—")));
            roleTable->setItem(r, 3, mutedCell(QStringLiteral("—")));
          }
        }
        continue;
      }

      const int r = roleTable->rowCount();
      roleTable->insertRow(r);
      auto *roleItem = new QTableWidgetItem(
          slot.def.display.isEmpty() ? slot.def.role : slot.def.display);
      roleItem->setFlags(roleItem->flags() & ~Qt::ItemIsEditable);
      if (slotEmpty)
        PaleoTheme::setItemTextColor(roleItem, PaleoTheme::ItemTextColor::Muted); // 空槽灰字（现取随主题）
      roleTable->setItem(r, 0, roleItem);

      if (!slot.primary.assetId.isEmpty())
      {
        const CatalogAsset pa = cat->assetById(slot.primary.assetId);
        QString primary = pa.displayName.isEmpty() ? slot.primary.assetId : pa.displayName;
        const CatalogVersion pv = cat->currentVersion(slot.primary.assetId);
        if (!pv.id.isEmpty())
          primary += tr(" v%1").arg(pv.versionNumber);
        auto *it = new QTableWidgetItem(primary);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        roleTable->setItem(r, 1, it);
      }
      else
        roleTable->setItem(r, 1, mutedCell(slotEmpty ? tr("缺失") : QStringLiteral("—")));

      QStringList memberNames, pendingNames, pendingNotes;
      for (const EntityAssetLink &m : slot.members)
      {
        const CatalogAsset a = cat->assetById(m.assetId);
        memberNames << (a.displayName.isEmpty() ? m.assetId : a.displayName);
      }
      for (const EntityAssetLink &u : slot.unresolved)
      {
        const CatalogAsset a = cat->assetById(u.assetId);
        pendingNames << (a.displayName.isEmpty() ? u.assetId : a.displayName);
        if (!u.note.isEmpty())
          pendingNotes << u.note;
      }
      auto *membersItem = memberNames.isEmpty()
                              ? mutedCell(QStringLiteral("—"))
                              : new QTableWidgetItem(memberNames.join(QStringLiteral("、")));
      if (!memberNames.isEmpty())
        membersItem->setFlags(membersItem->flags() & ~Qt::ItemIsEditable);
      roleTable->setItem(r, 2, membersItem);
      if (pendingNames.isEmpty())
        roleTable->setItem(r, 3, mutedCell(QStringLiteral("—")));
      else
      {
        auto *it = new QTableWidgetItem(pendingNames.join(QStringLiteral("、")));
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        if (!pendingNotes.isEmpty())
          it->setToolTip(pendingNotes.join(QStringLiteral("\n"))); // 候选名在徽标同款位置
        roleTable->setItem(r, 3, it);
      }
    }

    // 4. 属性明细 / 特征
    if (propDetailsText)
    {
      QStringList details;
      if (view.entity.entityType == QLatin1String("auxiliary"))
      {
        details << tr("资料标识: %1").arg(view.entity.id);
        details << tr("资料名称: %1").arg(view.entity.name);
      }
      else
      {
        details << tr("井编号: %1").arg(view.entity.id);
        details << tr("井名: %1").arg(view.entity.name);
        if (view.entity.hasSurface)
        {
          details << tr("井口坐标: X=%1, Y=%2")
                         .arg(QString::number(view.entity.surfaceX, 'f', 2))
                         .arg(QString::number(view.entity.surfaceY, 'f', 2));
        }
        const QVector<EntityAssetLink> links = cat->linksForEntity(view.entity.id);
        QStringList logNames, topNames;
        for (const EntityAssetLink &l : links)
        {
          const CatalogAsset a = cat->assetById(l.assetId);
          if (l.role == QLatin1String("well_log"))
            logNames << a.displayName;
          else if (l.role == QLatin1String("tops"))
            topNames << a.displayName;
        }
        if (!logNames.isEmpty())
          details << tr("测井曲线数据: %1").arg(logNames.join(QStringLiteral(", ")));
        if (!topNames.isEmpty())
          details << tr("分层数据: %1").arg(topNames.join(QStringLiteral(", ")));
      }
      propDetailsText->setText(details.join(QStringLiteral("\n")));
    }

    // 5. 下游派生产物
    for (int r = 0; r < derived->rowCount(); ++r)
      if (QWidget *w = derived->cellWidget(r, 2))
      {
        derived->removeCellWidget(r, 2);
        w->setParent(nullptr);
        w->deleteLater();
      }
    derived->setRowCount(0);
    for (const CatalogVersion &v : view.derivedProducts)
    {
      const int r = derived->rowCount();
      derived->insertRow(r);
      const CatalogAsset a = cat->assetById(v.assetId);
      const QString name = !a.displayName.isEmpty() ? a.displayName
                           : !v.fileName.isEmpty()  ? v.fileName
                                                    : v.id;
      auto *nameItem = new QTableWidgetItem(name);
      nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
      derived->setItem(r, 0, nameItem);
      auto *ver = new QTableWidgetItem(tr("v%1").arg(v.versionNumber));
      ver->setFont(PaleoTheme::monoFont());
      ver->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      derived->setItem(r, 1, ver);
      if (v.extra.value(QStringLiteral("stale")).toBool())
        derived->setCellWidget(
            r, 2, PaleoTheme::capsuleLabel(tr("过时"), PaleoTheme::CapsuleKind::Warning, derived));
      else
        derived->setItem(r, 2, mutedCell(QStringLiteral("—")));
    }

    // 悬空血缘诊断
    if (view.missingSources.isEmpty())
      missing->hide();
    else
    {
      missing->setText(tr("血缘诊断：缺失源版本 %1")
                           .arg(view.missingSources.join(QStringLiteral("、"))));
      missing->setStyleSheet(
          PaleoTheme::capsuleStyleSheet(PaleoTheme::CapsuleKind::Warning));
      missing->show();
    }
    // P3 D4.3：实体态时间线 = 已决资产的全部版本（新→旧聚合）。
    if (m_timeline)
    {
      QVector<CatalogVersion> agg;
      for (const RoleSlot &slot : view.roleSlots)
      {
        if (!slot.primary.assetId.isEmpty())
          agg += cat->versionsForAsset(slot.primary.assetId);
        for (const EntityAssetLink &m : slot.members)
          agg += cat->versionsForAsset(m.assetId);
      }
      m_timeline->loadVersions(agg);
    }
    if (m_topology)
      m_topology->loadTopology(cat,
                               m_ctx.entityOverrides ? *m_ctx.entityOverrides
                                                     : paleo::dataops::EntityOverrideStore());
    // D4.8 统计段：实体态摘要。
    if (auto *stats = findChild<QLabel *>(QStringLiteral("propStatsText")))
    {
      int filled = 0;
      for (const RoleSlot &slot : view.roleSlots)
        if (!slot.primary.assetId.isEmpty() || !slot.members.isEmpty())
          ++filled;
      stats->setText(tr("角色槽 %1/%2 已填 · 派生产物 %3 · 缺失源 %4")
                         .arg(filled)
                         .arg(view.roleSlots.size())
                         .arg(view.derivedProducts.size())
                         .arg(view.missingSources.size()));
    }
    return;
  }
}

bool EntityPanel::requestGeoJsonSummary(
    const QString &absPath,
    const std::function<void(const PreviewDocService::GeoJsonSummary &)> &apply)
{
  // F3（goal/perf-systematize 簇2）：缓存命中/无任务服务 = 同步直装（返回
  // true，调用方跳过占位覆盖）；任务池路径异步回调（世代号防陈旧）。
  if (auto it = m_geoSummaryCache.constFind(absPath); it != m_geoSummaryCache.constEnd())
  {
    apply(it.value());
    return true;
  }
  PaleoTaskService *svc = m_doc ? m_doc->taskService() : nullptr;
  if (!svc)
  {
    PreviewDocService::GeoJsonSummary sum;
    PreviewDocService::geoJsonSummaryAt(absPath, &sum, nullptr); // 失败=空 summary（回退态）
    m_geoSummaryCache.insert(absPath, sum);
    apply(sum);
    return true;
  }
  const int seq = ++m_geoSeq;
  auto out = std::make_shared<PreviewDocService::GeoJsonSummary>();
  auto *task = svc->start(
      tr("统计 GeoJSON 属性"), [absPath, out](PaleoTask *) -> QString {
        QString err;
        if (!PreviewDocService::geoJsonSummaryAt(absPath, out.get(), &err) && !err.isEmpty())
          return err;
        return QString();
      },
      QString(), /*quiet=*/true); // 交互内嵌取数——不拉起任务中心
  connect(task, &PaleoTask::finished, this, [this, seq, absPath, out, apply]() {
    if (seq != m_geoSeq)
      return; // 陈旧结果丢弃：新选择已接管
    m_geoSummaryCache.insert(absPath, *out);
    apply(*out);
  });
  return false;
}
// AUTOMOC：dataopspanelextra.h 的 Q_OBJECT 类（VersionTimeline/TopologyGraph/
// 各对话框）——本 TU 持有 moc（datalist.cpp 已持 panelops/undo/views 等）。
// __has_include 守卫：lint 门 configure-only 场景跳过（详见 datalist.cpp 尾注）。
#if __has_include("moc_dataopspanelextra.cpp")
#include "moc_dataopspanelextra.cpp"
#endif
