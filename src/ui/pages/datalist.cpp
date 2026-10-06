// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "assetentitychoicemodel.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../../workflow/storagegovernancecontroller.h"
#include "dataops/dataopscommands.h"
#include "dataops/dataopsfilter.h"
#include "dataops/dataopsmodel.h"
#include "dataops/dataopsselection.h"
#include "dataopsundo.h"
#include "dataopspanelops.h"
#include "dataopsimportui.h"
#include "dataopsviews.h"
#include "dataopswidgets.h"
#include "datanavtree.h"
#include "../shortcuts/shortcutcatalog.h" // 方向63：快捷键中央注册表
#include <QTimer>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
using namespace paleo::pagesinternal;

void DataListPanel::setDocService(PreviewDocService *doc)
{
  m_doc = doc;
  m_storageController->setCatalog(doc ? doc->catalog() : nullptr);
}

DataListPanel::DataListPanel(QWidget *parent)
  : QWidget(parent)
{
  m_storageController = new StorageGovernanceController(this);
  setMinimumWidth(0);
  auto *lay = panelLayout(this);

  // 三段各自成件（objectName 见头文件），壳按 ribbon 布局重新安放。
  const auto section = [this](const char *name) {
    auto *w = new QWidget(this);
    w->setObjectName(QLatin1String(name));
    w->setMinimumWidth(0);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(PaleoTheme::tokens().spacingSm);
    return w;
  };
  QWidget *importSection = section("dataImportSection");
  auto *importLay = static_cast<QVBoxLayout *>(importSection->layout());
  QWidget *listSection = section("dataListSection");
  auto *listLay = static_cast<QVBoxLayout *>(listSection->layout());

  importLay->addWidget(caption(tr("数据导入"), importSection));
  const struct { const char *name; const char *text; const char *kind; const char *desc; }
      kImports[] = {
    {"importWells", QT_TR_NOOP("导入井数据"), "wells",
     QT_TR_NOOP("选择单个井位/分层文件入库")},
    {"importWellLogs", QT_TR_NOOP("导入测井数据"), "well_log",
     QT_TR_NOOP("选择 LAS 测井曲线或 XML/Excel 综合柱状图入库")},
    {"importSeismic", QT_TR_NOOP("导入地震数据"), "seismic",
     QT_TR_NOOP("选择 SEG-Y 等地震数据文件入库")},
    {"importBoundary", QT_TR_NOOP("导入边界数据"), "boundary",
     QT_TR_NOOP("选择边界矢量文件入库")},
    {"importFolder", QT_TR_NOOP("导入工区文件夹"), "folder",
     QT_TR_NOOP("选择工区目录：确认每个文件的类型后整目录入库")},
  };
  for (const auto &spec : kImports)
  {
    auto *btn = new QPushButton(tr(spec.text), importSection);
    btn->setObjectName(QLatin1String(spec.name));
    // T32 a11y：导入入口各自报名（屏幕阅读器不读图标猜测）。
    btn->setAccessibleName(tr(spec.text));
    btn->setAccessibleDescription(tr(spec.desc));
    connect(btn, &QPushButton::clicked, this,
            [this, kind = QLatin1String(spec.kind)] { emit importRequested(kind); });
    importLay->addWidget(btn);
  }
  // 方向 30：导入台账 + 工区体检（维护面，随导入区安放）。
  auto *ledgerBtn = new QPushButton(tr("导入台账"), importSection);
  ledgerBtn->setObjectName(QStringLiteral("importLedgerButton"));
  ledgerBtn->setToolTip(tr("查看最近导入批次的行级结局（入库/未决/失败/跳过）"));
  ledgerBtn->setAccessibleName(tr("导入台账"));
  connect(ledgerBtn, &QPushButton::clicked, this, &DataListPanel::showImportLedger);
  importLay->addWidget(ledgerBtn);
  auto *healthBtn = new QPushButton(tr("工区体检"), importSection);
  healthBtn->setObjectName(QStringLiteral("healthCheckButton"));
  healthBtn->setToolTip(tr("检查缺失文件 / SHA 失配 / 未决链接 / 孤立实体等"));
  healthBtn->setAccessibleName(tr("工区体检"));
  connect(healthBtn, &QPushButton::clicked, this, &DataListPanel::showHealthCheck);
  auto *storage = new QPushButton(tr("存储治理台"), importSection);
  storage->setObjectName(QStringLiteral("storageGovernanceButton"));
  storage->setAccessibleName(storage->text());
  storage->setToolTip(tr("汇总体积、扫描未引用文件、预览回收过时版本"));
  connect(storage, &QPushButton::clicked, this, &DataListPanel::showStorageGovernance);
  importLay->addWidget(storage);
  importLay->addWidget(healthBtn);
  lay->addWidget(importSection);

  // 列表面表头：标题 + 折叠/展开 + 视图切换
  auto *header = new QWidget(listSection);
  header->setObjectName(QStringLiteral("dataListHeader"));
  header->setMinimumWidth(0);
  auto *hl = new QHBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 0);
  hl->setSpacing(PaleoTheme::tokens().spacingXs);
  hl->addWidget(caption(tr("数据列表"), header));
  hl->addStretch(1);

  auto *expandBtn = new QToolButton(header);
  expandBtn->setObjectName(QStringLiteral("expandAllTreeButton"));
  expandBtn->setToolTip(tr("全部展开"));
  expandBtn->setAccessibleName(tr("全部展开"));
  expandBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionExpandTree.svg")));
  if (expandBtn->icon().isNull())
    expandBtn->setText(QStringLiteral("▼"));
  expandBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.xs}px;")));
  hl->addWidget(expandBtn);

  auto *collapseBtn = new QToolButton(header);
  collapseBtn->setObjectName(QStringLiteral("collapseAllTreeButton"));
  collapseBtn->setToolTip(tr("全部折叠"));
  collapseBtn->setAccessibleName(tr("全部折叠"));
  collapseBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionCollapseTree.svg")));
  if (collapseBtn->icon().isNull())
    collapseBtn->setText(QStringLiteral("▶"));
  collapseBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.xs}px;")));
  hl->addWidget(collapseBtn);

  auto *btnGroup = new QButtonGroup(header);
  auto *treeBtn = new QToolButton(header);
  treeBtn->setObjectName(QStringLiteral("treeViewButton"));
  treeBtn->setText(tr("树形"));
  treeBtn->setCheckable(true);
  treeBtn->setChecked(true);
  treeBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  auto *tableBtn = new QToolButton(header);
  tableBtn->setObjectName(QStringLiteral("listViewButton"));
  tableBtn->setText(tr("列表"));
  tableBtn->setCheckable(true);
  tableBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  btnGroup->addButton(treeBtn, 0);
  btnGroup->addButton(tableBtn, 1);
  hl->addWidget(treeBtn);
  hl->addWidget(tableBtn);

  // 方向 30：未决链接批量归位入口（恰好一候选才自动挂，其余保持未决）。
  auto *pendingBtn = new QToolButton(header);
  pendingBtn->setObjectName(QStringLiteral("pendingResolveButton"));
  pendingBtn->setText(tr("未决归位"));
  pendingBtn->setToolTip(tr("按文件名/备注把能唯一命中一口井的未决链接批量挂接"));
  pendingBtn->setAccessibleName(tr("未决归位"));
  pendingBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  connect(pendingBtn, &QToolButton::clicked, this, &DataListPanel::resolvePendingLinks);
  hl->addWidget(pendingBtn);
  listLay->addWidget(header);

  // 搜索 + 类型筛选 + 计数
  auto *searchRow = new QWidget(listSection);
  searchRow->setMinimumWidth(0);
  auto *srl = new QHBoxLayout(searchRow);
  srl->setContentsMargins(0, 0, 0, 0);
  srl->setSpacing(PaleoTheme::tokens().spacingXs);

  auto *search = new QLineEdit(searchRow);
  search->setObjectName(QStringLiteral("assetSearchEdit"));
  search->setPlaceholderText(tr("搜索名称、类型、关联井"));
  search->setAccessibleName(tr("搜索数据"));
  search->setClearButtonEnabled(true);
  search->setMinimumWidth(20);
  srl->addWidget(search, 1);

  auto *typeFilter = new QComboBox(searchRow);
  typeFilter->setObjectName(QStringLiteral("assetTypeFilter"));
  typeFilter->setAccessibleName(tr("按类型筛选"));
  typeFilter->addItem(tr("所有类型"), QString());
  typeFilter->setMinimumWidth(30);
  typeFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  typeFilter->setMinimumContentsLength(4);
  srl->addWidget(typeFilter);

  auto *count = new QLabel(searchRow);
  count->setObjectName(QStringLiteral("assetCountLabel"));
  // text-muted——活体注册随主题重算。
  PaleoTheme::applyThemedStyleSheet(count, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  count->setMinimumWidth(0);
  srl->addWidget(count);
  listLay->addWidget(searchRow);

  connect(expandBtn, &QToolButton::clicked, this, [this]() {
    if (m_tree)
      m_tree->expandAll();
  });
  connect(collapseBtn, &QToolButton::clicked, this, [this]() {
    if (m_tree)
      m_tree->collapseAll();
  });
  connect(search, &QLineEdit::textChanged, this, [this] {
    m_filter.clearDim(paleo::dataops::FilterDim::Search); applyListFilter();
  });
  connect(typeFilter, &QComboBox::currentIndexChanged, this, [this] {
    m_filter.clearDim(paleo::dataops::FilterDim::Type); applyListFilter();
  });

  // T31「查看未决」过滤条：过滤开启时露出一行，带「清除过滤」。
  auto *filterBar = new QWidget(listSection);
  filterBar->setObjectName(QStringLiteral("unresolvedFilterBar"));
  filterBar->hide();
  auto *fl = new QHBoxLayout(filterBar);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *filterText = new QLabel(tr("只显示未决资产"), filterBar);
  PaleoTheme::applyThemedStyleSheet(filterText, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  fl->addWidget(filterText);
  auto *clearBtn = new QPushButton(tr("清除过滤"), filterBar);
  clearBtn->setObjectName(QStringLiteral("clearUnresolvedFilterButton"));
  clearBtn->setFlat(true);
  fl->addWidget(clearBtn);
  fl->addStretch(1);
  connect(clearBtn, &QPushButton::clicked, this, [this]() { setUnresolvedFilter(false); });
  listLay->addWidget(filterBar);

  m_viewStack = new QStackedWidget(listSection);
  m_viewStack->setObjectName(QStringLiteral("dataViewStack"));
  m_viewStack->setMinimumWidth(0);

  m_tree = new paleo::dataops::DataNavTree(m_viewStack);
  m_tree->setObjectName(QStringLiteral("dataTree"));
  m_tree->setAccessibleName(tr("数据列表树"));
  m_tree->setMinimumWidth(0);
  m_tree->header()->setMinimumSectionSize(30);
  m_tree->setHeaderLabels({tr("数据导航"), tr("类型 / 描述")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
  m_tree->setColumnWidth(1, 65);
  m_tree->header()->setStretchLastSection(false);
  m_tree->setTextElideMode(Qt::ElideRight);
  // DESIGN.md Motion：Qt Widgets 以即时切换为主、无编排动画——树展开
  // 动画属违例项，goal/ui-experience-polish 移除（原来开启着）。
  m_tree->setAnimated(false);
  m_tree->setAlternatingRowColors(true);
  // 树 chrome 走 token（活体注册随主题）；选中态/hover/斑马纹由壳级
  // PaleoTheme::itemViewStyleSheet 统一（primary 底 + onPrimary 字）——
  // 这里不再自写 ::item:selected（原 #E6F0FA 与全局三分叉，已收敛）。
  PaleoTheme::applyThemedStyleSheet(m_tree, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QTreeWidget { border: 1px solid %1; background: %2; }")
        .arg(t.border.name().toUpper(), t.surface.name().toUpper());
  });
  m_tree->installEventFilter(this);
  if (m_tree->viewport())
    m_tree->viewport()->installEventFilter(this);

  connect(btnGroup, &QButtonGroup::idClicked, this, [this](int id) {
    if (m_viewStack)
      m_viewStack->setCurrentIndex(id);
  });

  auto *table = new QTableWidget(0, 3, m_viewStack);
  table->setObjectName(QStringLiteral("assetTable"));
  table->setAccessibleName(tr("资产列表"));
  table->setMinimumWidth(0);
  table->horizontalHeader()->setMinimumSectionSize(25);
  table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("关联")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
  table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
  table->horizontalHeader()->setStretchLastSection(false);
  table->setColumnWidth(1, 55);
  table->setColumnWidth(2, 65);
  table->setTextElideMode(Qt::ElideRight);
  // P3 D1.1：ExtendedSelection（Ctrl/Shift/框选，跨类型混合选中）。
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  // P3 D2.7：搜索命中高亮委托（needle 随 applyListFilter 更新）。
  m_delegate = new paleo::dataops::HighlightDelegate(table);
  table->setItemDelegate(m_delegate);
  // P3 D1.3/D7.8：右键菜单 + 列头漏斗。
  table->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(table, &QTableWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) { showAssetContextMenu(sender(), pos); });
  connect(table->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
          [this, table](const QPoint &pos) {
            const int col = table->horizontalHeader()->logicalIndexAt(pos);
            if (col >= 0)
              paleo::dataops::ColumnFunnel::execForColumn(table, col, this);
          });
  table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  table->installEventFilter(this);
  if (table->viewport())
    table->viewport()->installEventFilter(this);
  refreshAssetEmptyState(table, QString());

  m_viewStack->addWidget(m_tree);  // 0: 树形
  m_viewStack->addWidget(table);   // 1: 列表
  listLay->addWidget(m_viewStack, 1);
  auto *pager = new QWidget(listSection);
  pager->setObjectName(QStringLiteral("assetPager"));
  pager->setAccessibleName(tr("资产分页"));
  auto *pl = new QHBoxLayout(pager);
  pl->setContentsMargins(0, 0, 0, 0);
  pl->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *previous = new QPushButton(tr("上一页"), pager);
  previous->setObjectName(QStringLiteral("assetPreviousPage"));
  previous->setAccessibleName(previous->text());
  auto *next = new QPushButton(tr("下一页"), pager);
  next->setObjectName(QStringLiteral("assetNextPage"));
  next->setAccessibleName(next->text());
  auto *pageLabel = new QLabel(pager);
  pageLabel->setObjectName(QStringLiteral("assetPageLabel"));
  pageLabel->setAccessibleName(tr("资产分页位置"));
  pl->addWidget(previous);
  pl->addWidget(pageLabel, 1);
  pl->addWidget(next);
  listLay->addWidget(pager);
  pager->hide();
  connect(m_viewStack, &QStackedWidget::currentChanged, this, [this, pager](int mode) {
    pager->setVisible(m_rows.size() > 200 && mode != 3);
  });
  connect(previous, &QPushButton::clicked, this, [this] { --m_assetPage; applyListFilter(); });
  connect(next, &QPushButton::clicked, this, [this] { ++m_assetPage; applyListFilter(); });
  table->horizontalHeader()->setSortIndicatorShown(false);
  connect(table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this, table](int column) {
    m_assetSortAscending = column == m_assetSortColumn ? !m_assetSortAscending : true;
    m_assetSortColumn = column;
    table->horizontalHeader()->setSortIndicatorShown(true);
    table->horizontalHeader()->setSortIndicator(column, m_assetSortAscending ? Qt::AscendingOrder : Qt::DescendingOrder);
    applyListFilter();
  });
  lay->addWidget(listSection, 1);

  const auto handleTreeActivation = [this](QTreeWidgetItem *item, int) {
    if (!item)
      return;
    const QString assetId = item->data(0, Qt::UserRole).toString();
    const QString wellId = item->data(0, Qt::UserRole + 1).toString();
    const QString nodeType = item->data(0, Qt::UserRole + 2).toString();
    const QString lineMode = item->data(0, Qt::UserRole + 3).toString();

    if (nodeType == QLatin1String("survey_area") || assetId == QLatin1String("survey_area"))
    {
      emit surveyAreaActivated();
      emit assetFocusRequested(QStringLiteral("survey_area"));
      return;
    }
    // 方向 47：集合成员/统计面叶 → 壳侧物化图层（缺席叶已禁选，到不了这里）。
    if (nodeType == QLatin1String("realization_member"))
    {
      emit realizationMemberRequested(item->data(0, Qt::UserRole + 3).toString(),
                                      item->data(0, Qt::UserRole + 4).toInt());
      return;
    }
    if (nodeType == QLatin1String("realization_stat"))
    {
      emit realizationStatRequested(item->data(0, Qt::UserRole + 3).toString(),
                                    item->data(0, Qt::UserRole + 4).toString());
      return;
    }
    if (nodeType == QLatin1String("realization_set"))
    {
      item->setExpanded(!item->isExpanded());
      return;
    }
    if (nodeType == QLatin1String("seismic_line"))
    {
      emit seismicLineActivated(assetId, lineMode);
      return;
    }
    if (!wellId.isEmpty() && !assetId.isEmpty())
    {
      emit assetWellActivated(assetId, wellId);
      emit assetFocusRequested(assetId);
      return;
    }
    if (!assetId.isEmpty())
    {
      emit assetActivated(assetId);
      return;
    }
    if (nodeType == QLatin1String("well"))
    {
      item->setExpanded(!item->isExpanded());
      // 双击或Enter激活井节点：打开第一条关联资产（如 A1.Las 测井曲线或井位）
      for (int i = 0; i < item->childCount(); ++i)
      {
        QTreeWidgetItem *child = item->child(i);
        const QString cid = child->data(0, Qt::UserRole).toString();
        if (!cid.isEmpty())
        {
          emit assetWellActivated(cid, wellId);
          emit assetFocusRequested(cid);
          break;
        }
      }
    }
    else
    {
      item->setExpanded(!item->isExpanded());
    }
  };

  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, handleTreeActivation);
  connect(m_tree, &QTreeWidget::itemActivated, this, handleTreeActivation);

  // P3 D3：树拖放——资产→实体（挂接/转移）、→标签（打标签）、外部文件→导入。
  connect(m_tree, &paleo::dataops::DataNavTree::assetsDroppedOnEntity, this,
          [this](const QStringList &assetIds, const QString &entityId) {
            applyEntityDrop(assetIds, entityId);
          });
  connect(m_tree, &paleo::dataops::DataNavTree::assetsDroppedOnTag, this,
          [this](const QStringList &assetIds, const QString &tag) {
            for (const QString &id : assetIds)
              pushCommand(new paleo::dataops::TagCmd(m_ctx, id, tag, true));
            emit statusMessage(tr("已给 %1 个资产打标签「%2」（可撤销）")
                                   .arg(assetIds.size()).arg(tag));
          });
  connect(m_tree, &paleo::dataops::DataNavTree::externalFilesDropped, this,
          &DataListPanel::handleExternalFiles);
  connect(m_tree, &paleo::dataops::DataNavTree::searchFocusRequested, this, [this] {
    if (auto *edit = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
      edit->setFocus();
  });

  connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
    if (!m_tree)
      return;
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty())
      return;
    QTreeWidgetItem *item = sel.front();
    const QString wellId = item->data(0, Qt::UserRole + 1).toString();
    const QString assetId = item->data(0, Qt::UserRole).toString();
    const QString nodeType = item->data(0, Qt::UserRole + 2).toString();
    if (nodeType == QLatin1String("survey_area") || assetId == QLatin1String("survey_area"))
    {
      emit assetFocusRequested(QStringLiteral("survey_area"));
      return;
    }
    if (!assetId.isEmpty())
    {
      emit assetFocusRequested(assetId);
    }
    else if (!wellId.isEmpty())
    {
      emit entitiesFocusRequested({wellId});
      emit wellSelected(wellId);
    }
  });

  // D5 命令栈：会话内撤销/重做（push 见各操作处理器）。
  m_opStack = new paleo::dataops::DataOpsUndoStack(this);
  connect(m_opStack, &paleo::dataops::DataOpsUndoStack::stackChanged, this,
          [this] { refreshUndoButtons(); });

  // P3 dataops 增量 UI（过滤条/队列/视图页/徽标/命令登记）。
  m_history = std::make_shared<paleo::dataops::OperationsHistory>();
  buildDataOpsUi();

  // Default surface: one search row and the data view. Infrequent operations
  // remain available behind an explicit disclosure instead of seven tool rows.
  auto *advanced = new QWidget(listSection);
  advanced->setObjectName(QStringLiteral("dataListAdvancedOptions"));
  auto *advancedLayout = new QVBoxLayout(advanced);
  advancedLayout->setContentsMargins(0, 0, 0, 0);
  advancedLayout->setSpacing(PaleoTheme::tokens().spacingXs);
  for (const char *name : {"dataListHeader", "dataOpsToolbar", "dataViewModeRow"})
    if (auto *row = findChild<QWidget *>(QLatin1String(name))) {
      listLay->removeWidget(row);
      advancedLayout->addWidget(row);
    }
  auto *typeRow = new QWidget(advanced);
  auto *typeLayout = new QHBoxLayout(typeRow);
  typeLayout->setContentsMargins(0, 0, 0, 0);
  typeLayout->addWidget(typeFilter, 1);
  typeLayout->addWidget(count);
  advancedLayout->addWidget(typeRow);
  if (auto *filters = findChild<QWidget *>(QStringLiteral("filterWrap"))) {
    listLay->removeWidget(filters);
    advancedLayout->addWidget(filters);
  }
  listLay->insertWidget(listLay->indexOf(searchRow) + 1, advanced);
  advanced->hide();
  auto *options = new QToolButton(searchRow);
  options->setObjectName(QStringLiteral("dataListOptionsButton"));
  options->setText(tr("选项"));
  options->setToolTip(tr("展开筛选、视图、排序与标签选项"));
  options->setAccessibleName(options->toolTip());
  options->setCheckable(true);
  srl->addWidget(options);
  connect(options, &QToolButton::toggled, advanced, &QWidget::setVisible);
  m_treeSort = paleo::dataops::recalledTreeSort();

  applyListFilter();   // 空表也写计数

  // 列表选中一条资产 → 中央预览标签（预览部件由 shell 持有，重选聚焦语义
  // 由 DataPreviewTabs 实现）。
  connect(table, &QTableWidget::itemSelectionChanged, this, [this, table]() {
    // P3 D1：多选中不逐个开预览（批量打开走「打开预览」动作）；程序化多选
    //（selectAssetsForEntities）保持旧行为——首个命中行激活。
    const QList<QTableWidgetItem *> sel = table->selectedItems();
    for (const auto &row : m_pageRows)
      m_pageSelection.remove(row.assetId);
    m_pageSelection.unite(paleo::dataops::selectedAssetIds(table));
    refreshSelectionBadge(); // 空选/多选都刷新（徽标隐藏 = 0/1 选中）
    if (sel.isEmpty())
      return;
    if (!m_progSelect && paleo::dataops::selectedAssetIds(table).size() > 1)
      return;
    const QString assetId = sel.front()->data(Qt::UserRole).toString();
    if (!assetId.isEmpty())
      emit assetActivated(assetId);
  });
  // 树选中联动徽标（实体/资产混合选中计数）。
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
          [this] { refreshSelectionBadge(); });
}

bool DataListPanel::eventFilter(QObject *watched, QEvent *event)
{
  // 键盘语义（D6.4 补实现——注释曾声称但未落地）：资产视图上
  //   Space  = 切换当前行选中态（ExtendedSelection 下 Qt 内建无此语义）；
  //   Delete = 软删当前选中（同 dataops.removeSoft 命令流，带确认与撤销）。
  // 搜索框等编辑控件不受影响（过滤器只装在资产视图上）。
  if (event && event->type() == QEvent::KeyPress)
  {
    QAbstractItemView *assetView = nullptr;
    if (watched == m_tree)
      assetView = m_tree;
    else if (m_iconView && watched == m_iconView)
      assetView = m_iconView;
    else if (m_groupTree && watched == m_groupTree)
      assetView = m_groupTree;
    else if (auto *tbl = qobject_cast<QTableWidget *>(watched);
             tbl && tbl->objectName() == QLatin1String("assetTable"))
      assetView = tbl;
    if (assetView)
    {
      auto *ke = static_cast<QKeyEvent *>(event);
      if (ke->key() == Qt::Key_Space && ke->modifiers() == Qt::NoModifier)
      {
        const QModelIndex cur = assetView->currentIndex();
        if (cur.isValid() && assetView->selectionModel())
        {
          const bool on = assetView->selectionModel()->isSelected(cur);
          assetView->selectionModel()->select(
              cur, on ? (QItemSelectionModel::Deselect | QItemSelectionModel::Rows)
                      : (QItemSelectionModel::Select | QItemSelectionModel::Rows));
        }
        return true;
      }
      if ((ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) &&
          ke->modifiers() == Qt::NoModifier && !currentAssetSelection().isEmpty())
      {
        batchRemoveSoft();
        return true;
      }
    }
  }
  if (event && event->type() == QEvent::Resize)
  {
    if (m_rows.size() > 200 && watched == findChild<QTableWidget *>(QStringLiteral("assetTable")))
      if (!property("paleo.pageResizeQueued").toBool()) {
        setProperty("paleo.pageResizeQueued", true);
        QTimer::singleShot(0, this, [this] { setProperty("paleo.pageResizeQueued", false); applyListFilter(); });
      }
    if (watched == m_tree || (m_tree && watched == m_tree->viewport()))
    {
      const int w = (watched == m_tree && event)
                        ? static_cast<QResizeEvent *>(event)->size().width()
                        : (m_tree ? m_tree->viewport()->width() : 0);
      if (w > 40)
      {
        const int col1W = qBound(35, w * 22 / 100, 75);
        if (m_tree->columnWidth(1) != col1W)
          m_tree->setColumnWidth(1, col1W);
      }
    }
    else if (auto *tbl = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    {
      if (watched == tbl || watched == tbl->viewport())
      {
        const int w = (watched == tbl && event)
                          ? static_cast<QResizeEvent *>(event)->size().width()
                          : tbl->viewport()->width();
        if (w > 50)
        {
          const int c1 = qBound(30, w * 18 / 100, 60);
          const int c2 = qBound(35, w * 22 / 100, 75);
          if (tbl->columnWidth(1) != c1)
            tbl->setColumnWidth(1, c1);
          if (tbl->columnWidth(2) != c2)
            tbl->setColumnWidth(2, c2);
        }
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void DataListPanel::buildDataOpsUi()
{
  using namespace paleo::dataops;
  QWidget *listSection = findChild<QWidget *>(QStringLiteral("dataListSection"));
  if (!listSection)
    return;
  auto *listLay = static_cast<QVBoxLayout *>(listSection->layout());

  // ---- 头部：仅选中徽标（D1.2）——宽度契约（D9）：本行保持 P3 之前的
  //      最小宽，新增按钮全部下放工具行，列表仍可压到 ~150px。----
  if (auto *header = findChild<QWidget *>(QStringLiteral("dataListHeader")))
  {
    auto *hl = static_cast<QHBoxLayout *>(header->layout());
    auto *badge = new SelectionBadge(header);
    hl->addWidget(badge);
  }

  // ---- dataops 工具行（D5.2 撤销/重做 + D2.5 排序 + D7.2 列配置 +
  //      D7.1 视图模式）：头行之下、搜索行之上的独立横排。----
  auto *toolbar = new QWidget(listSection);
  toolbar->setObjectName(QStringLiteral("dataOpsToolbar"));
  toolbar->setMinimumWidth(0);
  auto *tl = new QHBoxLayout(toolbar);
  tl->setContentsMargins(0, 0, 0, 0);
  tl->setSpacing(PaleoTheme::tokens().spacingXs);
  m_undoBtn = new QPushButton(tr("撤销"), toolbar);
  m_undoBtn->setObjectName(QStringLiteral("dataUndoButton"));
  m_undoBtn->setEnabled(false);
  m_undoBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  m_redoBtn = new QPushButton(tr("重做"), toolbar);
  m_redoBtn->setObjectName(QStringLiteral("dataRedoButton"));
  m_redoBtn->setEnabled(false);
  m_redoBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  m_undoBtn->setMaximumWidth(150); // 长操作名截断（tooltip 全文）
  m_redoBtn->setMaximumWidth(150);
  connect(m_undoBtn, &QPushButton::clicked, this, &DataListPanel::undoOp);
  connect(m_redoBtn, &QPushButton::clicked, this, &DataListPanel::redoOp);
  tl->addWidget(m_undoBtn);
  tl->addWidget(m_redoBtn);
  // D2.5 树排序下拉（记忆在 QSettings）。
  auto *sortBox = new QComboBox(toolbar);
  sortBox->setObjectName(QStringLiteral("treeSortCombo"));
  sortBox->setToolTip(tr("树排序方式（记忆到下次会话）"));
  sortBox->addItem(tr("按名称"), int(TreeSortKind::Name));
  sortBox->addItem(tr("按时间"), int(TreeSortKind::Time));
  sortBox->addItem(tr("按类型"), int(TreeSortKind::Type));
  sortBox->addItem(tr("按大小"), int(TreeSortKind::Size));
  sortBox->setCurrentIndex(qMax(0, sortBox->findData(int(m_treeSort))));
  sortBox->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt;")));
  sortBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  sortBox->setMinimumContentsLength(2);
  sortBox->setMinimumWidth(0);
  connect(sortBox, &QComboBox::currentIndexChanged, this, [this, sortBox](int) {
    setTreeSort(TreeSortKind(sortBox->currentData().toInt()));
  });
  tl->addWidget(sortBox);
  // D7.2 列配置。
  auto *colBtn = new QToolButton(toolbar);
  colBtn->setText(tr("列"));
  colBtn->setObjectName(QStringLiteral("columnConfigButton"));
  colBtn->setToolTip(tr("配置列（显隐/顺序）"));
  colBtn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
  connect(colBtn, &QToolButton::clicked, this, &DataListPanel::openColumnConfig);
  tl->addWidget(colBtn);
  tl->addStretch(1);
  // 第二行：视图模式全家（D7.1 树/表/图标 + 高速/分组）。树形/列表按钮
  // 从 header 迁入（objectName 不变；宽度契约：header 行回到 P3 前宽度）。
  auto *modeRow = new QWidget(listSection);
  modeRow->setObjectName(QStringLiteral("dataViewModeRow"));
  modeRow->setMinimumWidth(0);
  auto *ml = new QHBoxLayout(modeRow);
  ml->setContentsMargins(0, 0, 0, 0);
  ml->setSpacing(PaleoTheme::tokens().spacingXs);
  if (auto *btnGroup = findChild<QButtonGroup *>())
  {
    if (auto *header = findChild<QWidget *>(QStringLiteral("dataListHeader")))
      if (auto *treeBtn = header->findChild<QToolButton *>(QStringLiteral("treeViewButton")))
        treeBtn->setParent(modeRow);
    if (auto *header = findChild<QWidget *>(QStringLiteral("dataListHeader")))
      if (auto *listBtn = header->findChild<QToolButton *>(QStringLiteral("listViewButton")))
        listBtn->setParent(modeRow);
    if (auto *treeBtn = modeRow->findChild<QToolButton *>(QStringLiteral("treeViewButton")))
      ml->addWidget(treeBtn);
    if (auto *listBtn = modeRow->findChild<QToolButton *>(QStringLiteral("listViewButton")))
      ml->addWidget(listBtn);
    const struct
    {
      const char *name;
      const char *text;
      int id;
    } kModes[] = {
      {"iconViewButton", QT_TR_NOOP("图标"), 2},
      {"virtualViewButton", QT_TR_NOOP("高速"), 3},
      {"groupViewButton", QT_TR_NOOP("分组"), 4},
    };
    for (const auto &m : kModes)
    {
      auto *btn = new QToolButton(modeRow);
      btn->setObjectName(QLatin1String(m.name));
      btn->setText(tr(m.text));
      btn->setCheckable(true);
      btn->setStyleSheet(PaleoTheme::metricStyleSheet(QStringLiteral("font-size: {typography.label}pt; padding: {spacing.xs}px {spacing.sm}px;")));
      btnGroup->addButton(btn, m.id);
      ml->addWidget(btn);
    }
  }
  ml->addStretch(1);
  // 插到 header 之后：toolbar 第 1、模式行第 2。
  listLay->insertWidget(1, toolbar);
  listLay->insertWidget(2, modeRow);

  // ---- viewStack 新页：图标（2）/ 虚拟（3）/ 分组（4）----
  m_iconView = new AssetIconView(m_viewStack);
  m_virtualView = new AssetVirtualView(m_viewStack);
  m_groupTree = new AssetGroupTree(m_viewStack);
  // 键盘语义与树/表一致（Space 切换选中态 / Delete 软删——D6.4 补实现）。
  for (QAbstractItemView *v : {static_cast<QAbstractItemView *>(m_iconView),
                               static_cast<QAbstractItemView *>(m_groupTree)})
    v->installEventFilter(this);
  m_viewStack->addWidget(m_iconView);   // 2
  m_viewStack->addWidget(m_virtualView); // 3
  m_viewStack->addWidget(m_groupTree);  // 4
  // 图标/分组视图的激活语义与树一致（双击/回车 → 预览）。
  connect(m_iconView, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
    if (it)
      emit assetActivated(it->data(Qt::UserRole).toString());
  });
  connect(m_iconView, &QListWidget::itemSelectionChanged, this, [this] {
    refreshSelectionBadge();
  });
  connect(m_groupTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it, int) {
    if (it)
    {
      const QString id = it->data(0, Qt::UserRole).toString();
      if (!id.isEmpty())
        emit assetActivated(id);
    }
  });
  connect(m_groupTree, &QTreeWidget::itemSelectionChanged, this,
          [this] { refreshSelectionBadge(); });

  // ---- D2 多维过滤条 + chip 行 + 未决快捷行 ----
  auto *filterWrap = new QWidget(listSection);
  filterWrap->setObjectName(QStringLiteral("filterWrap"));
  auto *fw = new QVBoxLayout(filterWrap);
  fw->setContentsMargins(0, 0, 0, 0);
  fw->setSpacing(PaleoTheme::tokens().spacingXs);
  m_filterBar = new FilterBar(filterWrap);
  fw->addWidget(m_filterBar);
  m_chipBar = new FilterChipBar(filterWrap);
  fw->addWidget(m_chipBar);
  m_quickBar = new PendingQuickBar(filterWrap);
  fw->addWidget(m_quickBar);
  m_tagCloud = new TagCloudWidget(filterWrap);
  fw->addWidget(m_tagCloud);
  m_emptyState = new FilterEmptyState(filterWrap);
  fw->addWidget(m_emptyState);
  // 插在搜索行之后（搜索行已是 listLay 第 2 项——按索引插到其后）。
  const int searchRowIdx = [listLay]() {
    for (int i = 0; i < listLay->count(); ++i)
      if (auto *w = listLay->itemAt(i)->widget())
        if (w->findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
          return i;
    return -1;
  }();
  listLay->insertWidget(searchRowIdx >= 0 ? searchRowIdx + 1 : listLay->count(),
                        filterWrap);

  // 过滤条信号接线。
  connect(m_filterBar, &FilterBar::conditionAdded, this, [this](const FilterCondition &c) {
    m_filter.add(c);
    applyListFilter();
    refreshChipBar();
  });
  connect(m_filterBar, &FilterBar::modeChanged, this, [this](bool or_) {
    m_filter.orMode = or_;
    applyListFilter();
    refreshChipBar();
  });
  connect(m_filterBar, &FilterBar::presetChosen, this, [this](const QString &name) {
    const FilterGroup g = loadFilterPreset(name);
    m_filter = g;
    m_filterBar->setOrMode(g.orMode);
    applyListFilter();
    refreshChipBar();
    emit statusMessage(tr("已应用过滤器预设「%1」").arg(name));
  });
  connect(m_filterBar, &FilterBar::savePresetRequested, this, [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("保存过滤器"),
                                               tr("预设名称:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (ok && saveFilterPreset(name, m_filter))
    {
      m_filterBar->reloadPresets();
      emit statusMessage(tr("过滤器已存为预设「%1」").arg(name));
    }
  });
  connect(m_filterBar, &FilterBar::shareStateRequested, this, [this] {
    // D2.10：状态串复制到剪贴板（可粘贴分享/恢复）。
    QGuiApplication::clipboard()->setText(m_filter.toStateString());
    emit statusMessage(tr("过滤器状态串已复制（paleo://dataops-filter?…）"));
  });
  connect(m_chipBar, &FilterChipBar::chipRemoved, this, [this](const FilterCondition &c) {
    m_filter.removeOne(c);
    applyListFilter();
    refreshChipBar();
  });
  connect(m_chipBar, &FilterChipBar::chipToggled, this, [this](const FilterCondition &c) {
    for (FilterCondition &cc : m_filter.conditions)
      if (cc == c)
        cc.negate = !cc.negate;
    applyListFilter();
    refreshChipBar();
  });
  connect(m_chipBar, &FilterChipBar::clearAllRequested, this, [this] {
    m_filter.conditions.clear();
    m_filter.orMode = false;
    m_activeTag.clear();
    m_quickBar->clearAll();
    if (auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
      search->clear();
    if (auto *tf = findChild<QComboBox *>(QStringLiteral("assetTypeFilter")))
      tf->setCurrentIndex(0);
    applyListFilter();
    refreshChipBar();
    emit statusMessage(tr("已清除全部过滤条件"));
  });
  connect(m_quickBar, &PendingQuickBar::quickToggled, this,
          [this](FilterDim d, bool on) {
            if (on)
              m_filter.add({d, QString(), false});
            else
              m_filter.clearDim(d);
            applyListFilter();
            refreshChipBar();
          });
  connect(m_tagCloud, &TagCloudWidget::tagClicked, this, [this](const QString &t, bool) {
    // 开关态由处理器现算（点击的 chip 可能在重建 deleteLater 队列里——
    // 其捕获的 active 态不作真相源）。
    const bool on = m_activeTag != t;
    m_activeTag = on ? t : QString();
    applyListFilter();
    refreshTagCloud();
    emit statusMessage(on ? tr("按标签「%1」过滤").arg(t) : tr("已取消标签过滤"));
  });
  connect(m_emptyState, &FilterEmptyState::relaxRequested, this, [this] {
    // 逐级放宽：去掉最后一个条件（旧控件持有的维度同步清空，防 sync 回填）。
    if (!m_filter.conditions.isEmpty())
    {
      const FilterCondition last = m_filter.conditions.last();
      m_filter.conditions.removeLast();
      if (last.dim == FilterDim::Search)
        if (auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
        {
          const QSignalBlocker b(search);
          search->clear();
        }
      if (last.dim == FilterDim::Type)
        if (auto *tf = findChild<QComboBox *>(QStringLiteral("assetTypeFilter")))
        {
          const QSignalBlocker b(tf);
          tf->setCurrentIndex(0);
        }
    }
    else if (!m_activeTag.isEmpty())
      m_activeTag.clear();
    applyListFilter();
    refreshChipBar();
  });
  connect(m_emptyState, &FilterEmptyState::clearRequested, this, [this] {
    m_filter.conditions.clear();
    m_activeTag.clear();
    applyListFilter();
    refreshChipBar();
  });

  // ---- D8 导入队列面板（挂在列表段底）----
  m_importQueue = new ImportQueuePanel(listSection);
  listLay->addWidget(m_importQueue);

  // ---- 树右键菜单（D1.3 实体侧）----
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) { showAssetContextMenu(m_tree, pos); });

  // ---- 快捷键（D6.4/D6.6；命令面板/快捷键表在 DataPage 层）----
  // 方向63：键序登记在 shortcuts/shortcutcatalog（data.list.*），这里只按 id 绑定。
  const auto addShortcut = [this](const char *id, const char *name,
                                  std::function<void()> handler) {
    auto *sc = paleo::shortcuts::bindShortcut(QLatin1String(id), this);
    sc->setObjectName(QLatin1String(name));
    m_shortcuts.append({sc->key().toString(), QLatin1String(name)});
    connect(sc, &QShortcut::activated, this, std::move(handler));
    return sc;
  };
  addShortcut("data.list.undo", "scUndo",
              [this] { undoOp(); });
  addShortcut("data.list.redo", "scRedo",
              [this] { redoOp(); });
  addShortcut("data.list.invert", "scInvert",
              [this] { invertAssetSelection(); });
  addShortcut("data.list.selectFiltered", "scSelectFiltered",
              [this] { selectByCurrentFilter(); });
  addShortcut("data.list.rename", "scRename", [this] {
    // D4.1：树内实体节点 F2 → 实体重命名意图（EntityPanel/壳接）。
    const QStringList ents = currentEntitySelection();
    if (!ents.isEmpty())
      emit entityRenameRequested(ents.front());
  });
  addShortcut("data.list.shortcuts", "scShortcuts", [this] {
    emit shortcutsDialogRequested();
  });
  addShortcut("data.list.focusSearch", "scFocusSearch", [this] {
    if (auto *edit = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
      edit->setFocus();
  });
  addShortcut("data.list.vimToggle", "scVimToggle", [this] {
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    const bool on = !s.value(QStringLiteral("dataops/vimMode")).toBool();
    s.setValue(QStringLiteral("dataops/vimMode"), on);
    m_tree->setVimMode(on);
    emit statusMessage(on ? tr("Vim 风导航：开（j/k/g/G//）")
                          : tr("Vim 风导航：关"));
  });

  // 树/表键盘 Space 勾选语义（D6.4：Space = 切换选中态）——ExtendedSelection
  // 下 Qt 内建 Space 在 item view 无勾选语义；补 keyPressEvent 处理。
  installEventFilter(this);

  registerCommands();
}

#if __has_include("moc_dataopspanelops.cpp")
#include "moc_dataopspanelops.cpp"
#endif
#if __has_include("moc_dataopsimportui.cpp")
#include "moc_dataopsimportui.cpp"
#endif
#if __has_include("moc_dataopsundo.cpp")
#include "moc_dataopsundo.cpp"
#endif
#if __has_include("moc_dataopsviews.cpp")
#include "moc_dataopsviews.cpp"
#endif
#if __has_include("moc_dataopswidgets.cpp")
#include "moc_dataopswidgets.cpp"
#endif
#if __has_include("moc_datanavtree.cpp")
#include "moc_datanavtree.cpp"
#endif
#if __has_include("moc_versiondialog.cpp")
#include "moc_versiondialog.cpp"
#endif
#if __has_include("moc_pendinglinkdialog.cpp")
#include "moc_pendinglinkdialog.cpp"
#endif
#if __has_include("moc_importledgerdialog.cpp")
#include "moc_importledgerdialog.cpp"
#endif
