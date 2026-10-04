// 层：视图
#include "datalist.h"
#include "assetentitychoicemodel.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../../domain/importrows.h"
#include "dataops/dataopscommands.h"
#include "dataops/dataopsexport.h"
#include "dataops/dataopsfilter.h"
#include "dataops/dataopsmodel.h"
#include "dataops/dataopsselection.h"
#include "dataopsundo.h"
#include "dataopspanelops.h"
#include "dataopsimportui.h"
#include "dataopsviews.h"
#include "dataopswidgets.h"
#include "datanavtree.h"
#include "versiondialog.h"
#include "pendinglinkdialog.h"
#include "importledgerdialog.h"
#include "../dialogs/cataloghealthdialog.h"
#include "../dialogs/storagegovernancedialog.h"
#include "../../workflow/storagegovernancecontroller.h"
#include <QTimer>
#include "../../workflow/assetops.h"
#include "../../workflow/importledger.h"
#include <algorithm>
#include <QButtonGroup>
#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QShortcut>
#include <functional>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{
  // §42.4/T31: an empty asset table shows a centered guidance row — never a
  // blank panel. Copy names the next concrete step (导入工区文件夹 first).
  void refreshAssetEmptyState(QTableWidget *t, const QString &text)
  {
    if (t->rowCount() > 0)
      return;
    t->insertRow(0);
    auto *it = new QTableWidgetItem(text.isEmpty()
                                        ? DataListPanel::tr("还没有数据资产 — 先导入工区文件夹，"
                                                       "或用上方按钮导入单个文件")
                                        : text);
    it->setFlags(Qt::NoItemFlags);
    PaleoTheme::setItemTextColor(it, PaleoTheme::ItemTextColor::Muted); // text-muted（现取随主题）
    it->setTextAlignment(Qt::AlignCenter);                // T31 居中提示
    t->setItem(0, 0, it);
    t->setSpan(0, 0, 1, t->columnCount());
  }

  // p5a 实体视图占位格（「缺失」/「—」）：缺源可见但样式克制（upstream
  // missing-source 原则——空角色如实显示为缺失槽位，灰字、不可交互）。
  QTableWidgetItem *mutedCell(const QString &text)
  {
    auto *it = new QTableWidgetItem(text);
    it->setFlags(Qt::NoItemFlags);
    PaleoTheme::setItemTextColor(it, PaleoTheme::ItemTextColor::Muted); // text-muted（现取随主题）
    return it;
  }


  // ---- T28：链接身份寻址 + undo 库 -----------------------------------------
  // 动作（挂接/撤销/设为主版本）按 (assetId, role[, entityId]) 链接身份在
  // 点击时刻重扫 links()，不用刷新时捕获的行下标——open() 重建或任何
  // 中途变更后仍命中同一条链接。
  int indexOfLink(DataCatalog *cat, const QString &assetId, const QString &role,
                  const QString &entityId)
  {
    const QVector<EntityAssetLink> ls = cat->links();
    for (int i = 0; i < ls.size(); ++i)
    {
      const EntityAssetLink &l = ls.at(i);
      if (l.assetId != assetId || l.role != role)
        continue;
      if (entityId.isEmpty())
      {
        if (l.unresolved)
          return i; // 未决链接：entityId 空
      }
      else if (!l.unresolved && l.entityId == entityId)
        return i;
    }
    return -1;
  }

  // undo 记录：attach 时落档，撤销时消费。跨 open() 持久（随工程的
  // sidecar .paleo/undo_stack.json——catalog 无 note 写回 API（A 包接缝），
  // note 在 UI 层保管：撤销后未决徽标 tooltip 从 note 记忆恢复显示）。
  struct UndoRecord
  {
    QString assetId, entityId, role, entityType, note;
    // 被 attach 降级的前主关联（D4：撤销恢复降级 primary）；空 = 没有。
    QString demotedAssetId, demotedEntityId, demotedRole, demotedEntityType;
    bool isValid() const
    {
      return !assetId.isEmpty() && !entityId.isEmpty() && !role.isEmpty();
    }
  };

  QString undoVaultPath(DataCatalog *cat)
  {
    const QString cp = cat ? cat->catalogPath() : QString();
    if (cp.isEmpty() || !cat->isOpen())
      return QString();
    // catalog 在 <projectDir>/artifacts/metadata/catalog.json → 退三级到工程
    // 目录（方向 30 修正：与 dataopsmodel.h projectDirFor 同步——原两级推导
    // 落在 artifacts/ 下）。
    const QString projectDir = QFileInfo(
        QFileInfo(QFileInfo(cp).dir().absolutePath()).dir().absolutePath())
        .dir()
        .absolutePath();
    return QDir(projectDir).filePath(QStringLiteral(".paleo/undo_stack.json"));
  }

  // 副作用清洗：attach 的目标链接已不在（被外部改掉/新会话没有这条挂接）
  // → 记录作废剔除；note 记忆只留仍未决链接能用的。
  QVector<UndoRecord> loadUndoVault(DataCatalog *cat, QHash<QString, QString> *notesOut)
  {
    QVector<UndoRecord> records;
    const QString path = undoVaultPath(cat);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return records;
    const QJsonObject root =
        QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : root.value(QStringLiteral("undo")).toArray())
    {
      const QJsonObject o = v.toObject();
      UndoRecord r;
      r.assetId = o.value(QStringLiteral("asset_id")).toString();
      r.entityId = o.value(QStringLiteral("entity_id")).toString();
      r.role = o.value(QStringLiteral("role")).toString();
      r.entityType = o.value(QStringLiteral("entity_type")).toString();
      r.note = o.value(QStringLiteral("note")).toString();
      const QJsonObject d = o.value(QStringLiteral("demoted")).toObject();
      r.demotedAssetId = d.value(QStringLiteral("asset_id")).toString();
      r.demotedEntityId = d.value(QStringLiteral("entity_id")).toString();
      r.demotedRole = d.value(QStringLiteral("role")).toString();
      r.demotedEntityType = d.value(QStringLiteral("entity_type")).toString();
      if (r.isValid() && indexOfLink(cat, r.assetId, r.role, r.entityId) >= 0)
        records.append(r); // 链接仍在（已决到同一实体）→ 撤销入口有效
    }
    if (notesOut)
      for (const QJsonValue &v : root.value(QStringLiteral("notes")).toArray())
      {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("asset_id")).toString() +
                            QLatin1Char('|') + o.value(QStringLiteral("role")).toString();
        notesOut->insert(key, o.value(QStringLiteral("note")).toString());
      }
    return records;
  }

  void saveUndoVault(DataCatalog *cat, const QVector<UndoRecord> &records,
                     const QHash<QString, QString> &notes)
  {
    const QString path = undoVaultPath(cat);
    if (path.isEmpty())
      return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject root;
    QJsonArray arr;
    for (const UndoRecord &r : records)
    {
      QJsonObject o;
      o.insert(QStringLiteral("asset_id"), r.assetId);
      o.insert(QStringLiteral("entity_id"), r.entityId);
      o.insert(QStringLiteral("role"), r.role);
      o.insert(QStringLiteral("entity_type"), r.entityType);
      o.insert(QStringLiteral("note"), r.note);
      if (!r.demotedAssetId.isEmpty())
      {
        QJsonObject d;
        d.insert(QStringLiteral("asset_id"), r.demotedAssetId);
        d.insert(QStringLiteral("entity_id"), r.demotedEntityId);
        d.insert(QStringLiteral("role"), r.demotedRole);
        d.insert(QStringLiteral("entity_type"), r.demotedEntityType);
        o.insert(QStringLiteral("demoted"), d);
      }
      arr.append(o);
    }
    root.insert(QStringLiteral("undo"), arr);
    QJsonArray noteArr;
    for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
    {
      const int sep = it.key().indexOf(QLatin1Char('|'));
      QJsonObject o;
      o.insert(QStringLiteral("asset_id"), it.key().left(sep));
      o.insert(QStringLiteral("role"), it.key().mid(sep + 1));
      o.insert(QStringLiteral("note"), it.value());
      noteArr.append(o);
    }
    root.insert(QStringLiteral("notes"), noteArr);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
      f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    // 写失败只在日志说明——undo 是增强面，不阻塞挂接本身。
    else
      qWarning() << "DataPage undo vault write failed:" << path;
  }
  bool naturalNameSort(const QString &na, const QString &nb)
{
  int ia = 0, ib = 0;
  while (ia < na.size() && !na.at(ia).isDigit()) ++ia;
  while (ib < nb.size() && !nb.at(ib).isDigit()) ++ib;
  const QString preA = na.left(ia);
  const QString preB = nb.left(ib);
  if (preA != preB)
    return preA < preB;
  bool okA = false, okB = false;
  const int numA = na.mid(ia).toInt(&okA);
  const int numB = nb.mid(ib).toInt(&okB);
  if (okA && okB)
    return numA < numB;
  return na < nb;
}
} // namespace

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
void DataListPanel::refreshAssetTable()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table || !m_doc)
    return;
  loadStoresForCatalog();
  m_selKeep.snapshot(m_tree, table);
  m_pageSelection.unite(m_selKeep.ids());
  DataCatalog *cat = m_doc->catalog();
  for (auto *model : m_entityChoiceModels) model->deleteLater();
  m_entityChoiceModels.clear();
  rebuildRowSnapshot();
  m_tableDirty = true;
  QSet<QString> live;
  for (const auto &row : m_rows)
    live.insert(row.assetId);
  m_pageSelection.intersect(live);
  // 类型下拉按当前资产类型集重建（保留原选择）。
  if (auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter")))
  {
    const QString keep = typeFilter->currentData().toString();
    QStringList types;
    for (const CatalogAsset &a : cat->assets())
      if (!a.type.isEmpty() && !types.contains(a.type))
        types << a.type;
    types.sort();
    const QSignalBlocker block(typeFilter);
    typeFilter->clear();
    typeFilter->addItem(tr("所有类型"), QString());
    for (const QString &t : types)
    {
      QString label = t;
      if (t == QLatin1String("well_log")) label = tr("测井曲线 (well_log)");
      else if (t == QLatin1String("well_head")) label = tr("井位/井身 (well_head)");
      else if (t == QLatin1String("tops")) label = tr("井分层 (tops)");
      else if (t == QLatin1String("time_depth")) label = tr("时深关系 (time_depth)");
      else if (t == QLatin1String("seismic")) label = tr("地震数据 (seismic)");
      else if (t == QLatin1String("horizon")) label = tr("层位解释 (horizon)");
      else if (t == QLatin1String("boundary")) label = tr("边界/相图 (boundary)");
      else if (t == QLatin1String("auxiliary") || t == QLatin1String("reference")) label = tr("辅助/综合图 (auxiliary)");
      typeFilter->addItem(label, t);
    }
    typeFilter->setCurrentIndex(qMax(0, typeFilter->findData(keep)));
  }
  refreshTagCloud();
  updatePendingCounts();
  refreshChipBar();
  refreshUndoButtons();
  applyListFilter();
  refreshSelectionBadge();
}

void DataListPanel::renderAssetPage()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table || !m_doc)
    return;
  const QSignalBlocker block(table);
  DataCatalog *cat = m_doc->catalog();
  // T28 undo 库：attach 落档（含被降级的前主关联 + 挂前 note），撤销消费；
  // note 记忆独立长存（撤销后未决徽标 tooltip 从这里恢复——catalog 无 note
  // 写回 API，A 包接缝，UI 层保管）。随工程 sidecar 持久，跨 open() 存活。
  QHash<QString, QString> noteMemory;
  QVector<UndoRecord> undoRecords = loadUndoVault(cat, &noteMemory);
  // 确认条存活（T28）：changed() 刷新重建表时，未决的确认状态从这恢复。
  const QVariantMap pendingConfirm =
      property("paleo.page.pendingConfirm").toMap();

  // Qt 清行不删单元格控件（setRowCount(0)/removeCellWidget 都只摘不删，
  // 控件会活成表内孤儿）——摘出父子树再 deleteLater：刷新可能正被这行
  // 自己的按钮 clicked 触发，同步 delete 会在发件者栈上自杀。
  for (int r = 0; r < table->rowCount(); ++r)
    if (QWidget *w = table->cellWidget(r, 2))
    {
      table->removeCellWidget(r, 2);
      w->setParent(nullptr);
      w->deleteLater();
    }
  table->setRowCount(0);
  const bool unresolvedOnly = property("paleo.page.filterUnresolved").toBool();
  for (const auto &row : m_pageRows)
  {
    const CatalogAsset a = cat->assetById(row.assetId);
    const int r = table->rowCount();
    table->insertRow(r);
    auto *nameItem = new QTableWidgetItem(a.displayName);
    nameItem->setData(Qt::UserRole, a.id);
    // 「来源」并入名称 tooltip：受管相对路径 / 外链绝对路径（§4）。
    const CatalogVersion v = cat->currentVersion(a.id);
    if (!v.id.isEmpty())
      nameItem->setToolTip(v.managed ? tr("受管 %1").arg(v.path)
                                     : tr("外部链接 %1").arg(v.path));
    table->setItem(r, 0, nameItem);
    const QString shownType = m_typeOv.overriddenType(a.id).isEmpty()
                                  ? a.type
                                  : m_typeOv.overriddenType(a.id);
    table->setItem(r, 1, new QTableWidgetItem(shownType));

    // ---- 关联列 ----------------------------------------------------------
    QStringList parts;    // item 文本（控件行也保留，便于检索与断言）
    QStringList resolved; // 控件行回显的实体名/「参考」
    QStringList notes;    // 未决备注（徽标 tooltip：候选名在这里）
    // 身份而非下标（T28）：动作在点击时刻按这些键重扫 links()。
    QString unresolvedRole, unresolvedEntityType; // 本资产第一条未决链接的身份
    const UndoRecord *undoRecord = nullptr;       // 本资产可撤销的挂接记录
    QString promotableRole, promotableEntityId;   // 已决非主链接（「设为主版本」）
    for (const EntityAssetLink &l : cat->linksForAsset(a.id))
    {
      if (l.unresolved)
      {
        if (unresolvedRole.isEmpty())
        {
          unresolvedRole = l.role;
          unresolvedEntityType = l.entityType;
        }
        if (!parts.contains(QStringLiteral("未决")))
          parts << tr("未决");
        if (!l.note.isEmpty())
          notes << l.note;
        continue;
      }
      if (l.role == QLatin1String("reference"))
      {
        if (!parts.contains(QStringLiteral("参考")))
        {
          parts << tr("参考");
          resolved << tr("参考");
        }
        continue;
      }
      const CatalogEntity e = cat->entityById(l.entityId);
      const QString nm = e.name.isEmpty() ? l.entityId : e.name;
      if (!nm.isEmpty() && !parts.contains(nm))
      {
        parts << nm;
        resolved << nm;
      }
      if (!l.isPrimary && promotableRole.isEmpty())
      {
        promotableRole = l.role;
        promotableEntityId = l.entityId;
      }
    }
    for (const UndoRecord &rec : undoRecords)
      if (rec.assetId == a.id)
      {
        undoRecord = &rec;
        break;
      }

    auto *assoc = new QTableWidgetItem(parts.join(QStringLiteral("、")));
    if (!notes.isEmpty())
      assoc->setToolTip(notes.join(QStringLiteral("\n")));
    table->setItem(r, 2, assoc);

    if (unresolvedRole.isEmpty() && !undoRecord && promotableRole.isEmpty())
      continue; // 纯已决文本行——不需要单元格控件

    // 控件单元格：[已决名…] [未决徽标+实体下拉+挂接] [撤销] [设为主版本]。
    // 「挂到这口井」走页内确认条（同时写出资产名和实体名），不弹模态框。
    auto *cell = new QWidget(table);
    auto *stack = new QStackedLayout(cell);
    stack->setContentsMargins(0, 0, 0, 0);
    auto *browse = new QWidget(cell);
    auto *bl = new QHBoxLayout(browse);
    // 保持原生表格既定行高；纵向留白由行控制，单元格不重复叠加。
    bl->setContentsMargins(PaleoTheme::tokens().spacingXs, 0, PaleoTheme::tokens().spacingXs, 0);
    bl->setSpacing(PaleoTheme::tokens().spacingXs);
    if (!resolved.isEmpty())
    {
      auto *names = new QLabel(resolved.join(QStringLiteral("、")), browse);
      PaleoTheme::applyThemedStyleSheet(names, [] { return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().text.name().toUpper()); });
      bl->addWidget(names);
    }

    if (!unresolvedRole.isEmpty())
    {
      const QString noteKey = a.id + QLatin1Char('|') + unresolvedRole;
      const QString badgeNote = [cat, &noteMemory, &noteKey]() {
        for (const EntityAssetLink &l : cat->linksForAsset(
                 noteKey.section(QLatin1Char('|'), 0, 0)))
          if (l.role == noteKey.section(QLatin1Char('|'), 1) && l.unresolved &&
              !l.note.isEmpty())
            return l.note; // catalog 里的 note 优先
        return noteMemory.value(noteKey, QString()); // T28：撤销后从记忆恢复
      }();
      auto *badge = new QLabel(tr("未决"), browse);
      badge->setObjectName(QStringLiteral("unresolvedBadge"));
      // §4 未决徽标：warningBg 底 / text 字 / warning 边（DESIGN.md 浅值
      // #FFF4E0/#24303E/#F29900——token 活体，随主题出暗色变体）。
      PaleoTheme::applyThemedStyleSheet(badge, [] {
        const auto &t = PaleoTheme::tokens();
        return PaleoTheme::metricStyleSheet(QStringLiteral(
                   "background: %1; color: %2; border: 1px solid %3;"
                   "border-radius: {rounded.sm}px; padding: 0 {spacing.sm}px;"))
            .arg(t.warningBg.name().toUpper(), t.text.name().toUpper(),
                 t.warning.name().toUpper());
      });
      badge->setToolTip(badgeNote.isEmpty() ? tr("未决关联") : badgeNote);
      bl->addWidget(badge);

      // 下拉框：全量同类实体（井→全部井实体），默认空（哨兵项）。
      auto *combo = new QComboBox(browse);
      combo->setObjectName(QStringLiteral("resolveEntityCombo"));
      combo->setAccessibleName(tr("挂到实体"));
      const bool isWell = unresolvedEntityType == QLatin1String("well");
      auto *choices = m_entityChoiceModels.value(unresolvedEntityType);
      if (!choices) {
        choices = new AssetEntityChoiceModel(cat->entities(unresolvedEntityType),
          isWell ? tr("（选择井）") : tr("（选择实体）"), this);
        m_entityChoiceModels.insert(unresolvedEntityType, choices);
      }
      combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      combo->setMinimumContentsLength(8);
      combo->setMaxVisibleItems(20);
      combo->setModel(choices);
      combo->setCurrentIndex(0);
      combo->setMinimumWidth(72);
      bl->addWidget(combo, 1);

      auto *attach = new QPushButton(isWell ? tr("挂到这口井") : tr("挂接关联"), browse);
      attach->setObjectName(QStringLiteral("attachLinkButton"));
      attach->setEnabled(false); // 选到实体才放闸
      if (combo->count() <= 1)
        attach->setToolTip(tr("工程里还没有可挂的实体"));
      connect(combo, &QComboBox::currentIndexChanged, attach, [combo, attach](int) {
        attach->setEnabled(!combo->currentData().toString().isEmpty());
      });
      bl->addWidget(attach);

      // 确认条：资产名 + 选中的实体名同时写出（§4），不弹模态对话框。
      auto *confirmStrip = new QWidget(cell);
      auto *cf = new QHBoxLayout(confirmStrip);
      cf->setContentsMargins(PaleoTheme::tokens().spacingXs, 0, PaleoTheme::tokens().spacingXs, 0);
      cf->setSpacing(PaleoTheme::tokens().spacingXs);
      auto *confirmText = new QLabel(confirmStrip);
      confirmText->setObjectName(QStringLiteral("attachConfirmText"));
      confirmText->setWordWrap(true);
      cf->addWidget(confirmText, 1);
      auto *ok = new QPushButton(tr("确认"), confirmStrip);
      ok->setObjectName(QStringLiteral("attachConfirmButton"));
      auto *cancel = new QPushButton(tr("取消"), confirmStrip);
      cancel->setObjectName(QStringLiteral("attachCancelButton"));
      cf->addWidget(ok);
      cf->addWidget(cancel);
      stack->addWidget(browse);
      stack->addWidget(confirmStrip);
      stack->setCurrentWidget(browse);

      // 确认状态落在页面属性上（T28）：changed() 触发的整表重建会从这里
      // 恢复确认条——用户不会被一次后台刷新打断。
      const auto armConfirm = [this, confirmText, stack, confirmStrip, combo,
                               displayName = a.displayName, assetId = a.id]() {
        const QString eid = combo->currentData().toString();
        if (eid.isEmpty())
          return;
        const QString ename = combo->currentText();
        QVariantMap pc;
        pc.insert(QStringLiteral("asset_id"), assetId);
        pc.insert(QStringLiteral("entity_id"), eid);
        pc.insert(QStringLiteral("entity_name"), ename);
        setProperty("paleo.page.pendingConfirm", pc);
        confirmText->setText(
            tr("把「%1」挂到「%2」？").arg(displayName, ename));
        stack->setCurrentWidget(confirmStrip);
      };
      connect(attach, &QPushButton::clicked, this, armConfirm);
      const auto clearPending = [this]() {
        setProperty("paleo.page.pendingConfirm", QVariantMap());
      };
      connect(cancel, &QPushButton::clicked, this, [this, stack, browse, clearPending]() {
        clearPending();
        stack->setCurrentWidget(browse);
      });
      connect(ok, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, role = unresolvedRole, combo, clearPending]() {
                clearPending();
                const QString eid = combo->currentData().toString();
                // 身份寻址（T28）：点击时刻重扫——刷新与点击之间 links()
                // 变过也命中本资产的这条未决链接。
                const int idx = indexOfLink(cat, assetId, role, QString());
                if (eid.isEmpty() || idx < 0)
                  return;
                const QVector<EntityAssetLink> ls = cat->links();
                const EntityAssetLink &link = ls.at(idx);
                // 被降级的前主关联（撤销时恢复，D4）：同（实体,角色）当前主链。
                UndoRecord rec;
                rec.assetId = assetId;
                rec.entityId = eid;
                rec.role = role;
                rec.entityType = link.entityType;
                rec.note = link.note; // 挂前 note（attachLink 会清）
                for (int i = 0; i < ls.size(); ++i)
                  if (i != idx && ls.at(i).isPrimary && !ls.at(i).unresolved &&
                      ls.at(i).entityType == link.entityType &&
                      ls.at(i).entityId == eid && ls.at(i).role == role)
                  {
                    rec.demotedAssetId = ls.at(i).assetId;
                    rec.demotedEntityId = ls.at(i).entityId;
                    rec.demotedRole = ls.at(i).role;
                    rec.demotedEntityType = ls.at(i).entityType;
                    break;
                  }
                QString err;
                if (!cat->attachLink(idx, eid, &err))
                {
                  qWarning() << "DataPage attachLink failed:" << err;
                  refreshAssetTable();
                  emit entityRefreshRequested();
                  return;
                }
                // undo 落档 + note 记忆（跨 open() 持久；从盘上状态续写——
                // 处理器运行时刷新局部早没了）。
                QHash<QString, QString> notes;
                QVector<UndoRecord> records = loadUndoVault(cat, &notes);
                records.append(rec);
                if (!rec.note.isEmpty())
                  notes.insert(assetId + QLatin1Char('|') + role, rec.note);
                saveUndoVault(cat, records, notes);
                refreshAssetTable();
                emit entityRefreshRequested();
              });

      // 刷新后恢复确认条（pendingConfirm 仍指向本资产的未决链接）。
      if (pendingConfirm.value(QStringLiteral("asset_id")).toString() == a.id &&
          pendingConfirm.value(QStringLiteral("entity_id")).toString().isEmpty() == false)
      {
        const QString pendEid =
            pendingConfirm.value(QStringLiteral("entity_id")).toString();
        const int comboIdx = combo->findData(pendEid);
        if (comboIdx >= 0 && indexOfLink(cat, a.id, unresolvedRole, QString()) >= 0)
        {
          combo->setCurrentIndex(comboIdx);
          confirmText->setText(tr("把「%1」挂到「%2」？")
                                   .arg(a.displayName,
                                        pendingConfirm.value(QStringLiteral("entity_name"))
                                            .toString()));
          stack->setCurrentWidget(confirmStrip);
        }
        else
          clearPending();
      }
    }
    else
      stack->addWidget(browse);

    if (undoRecord)
    {
      auto *undo = new QPushButton(tr("撤销"), browse);
      undo->setObjectName(QStringLiteral("undoAttachButton"));
      const CatalogEntity e = cat->entityById(undoRecord->entityId);
      const QString target = e.name.isEmpty() ? undoRecord->entityId : e.name;
      undo->setToolTip(tr("撤回对「%1」的挂接（回到未决）").arg(target));
      connect(undo, &QPushButton::clicked, this,
              [this, cat, assetId = undoRecord->assetId, entityId = undoRecord->entityId,
               role = undoRecord->role, demotedAssetId = undoRecord->demotedAssetId,
               demotedEntityId = undoRecord->demotedEntityId,
               demotedRole = undoRecord->demotedRole]() {
                // 身份寻址（T28）：撤销的是这条挂接，不是某个行号。
                const int idx = indexOfLink(cat, assetId, role, entityId);
                QString err;
                if (idx < 0 || !cat->setLinkUnresolved(idx, &err))
                {
                  qWarning() << "DataPage setLinkUnresolved failed:" << err;
                  refreshAssetTable();
                  emit entityRefreshRequested();
                  return;
                }
                // D4：恢复被降级的前主关联（同井同角色换回旧主链）。
                if (!demotedAssetId.isEmpty())
                {
                  const int pIdx = indexOfLink(cat, demotedAssetId, demotedRole,
                                               demotedEntityId);
                  if (pIdx >= 0 && !cat->setLinkPrimary(pIdx, &err))
                    qWarning() << "DataPage restore demoted primary failed:" << err;
                }
                // note 记忆保留（撤销后的未决徽标 tooltip 用），undo 记录消费
                // （vault 从盘上续读——刷新局部不进处理器）。
                QHash<QString, QString> notes;
                QVector<UndoRecord> records = loadUndoVault(cat, &notes);
                for (int i = 0; i < records.size(); ++i)
                  if (records.at(i).assetId == assetId &&
                      records.at(i).entityId == entityId &&
                      records.at(i).role == role)
                    records.removeAt(i--);
                saveUndoVault(cat, records, notes);
                refreshAssetTable();
                emit entityRefreshRequested();
              });
      bl->addWidget(undo);
    }

    if (!promotableRole.isEmpty())
    {
      // 「将此版本设为主版本」：同（实体,角色）的旧版本资产可拿回主关联——
      // 只动链接的 isPrimary 标志，不复制版本字节（§4）。身份寻址（T28）。
      // well_log 文案是「设为主文件」，点击仍按 assetId+role+entityId 重查。
      const bool wellLogPrimary = promotableRole == QLatin1String("well_log");
      auto *primary = new QPushButton(wellLogPrimary ? tr("设为主文件") : tr("设为主版本"),
                                      browse);
      primary->setObjectName(wellLogPrimary ? QStringLiteral("setWellLogPrimaryButton")
                                            : QStringLiteral("setPrimaryButton"));
      primary->setToolTip(wellLogPrimary ? tr("同井测井 — 把这份文件设为主文件")
                                         : tr("同角色旧版本 — 把这条关联设为主关联"));
      connect(primary, &QPushButton::clicked, this,
              [this, cat, assetId = a.id, role = promotableRole,
               eid = promotableEntityId]() {
                const int idx = indexOfLink(cat, assetId, role, eid);
                QString err;
                if (idx < 0 || !cat->setLinkPrimary(idx, &err))
                  qWarning() << "DataPage setLinkPrimary failed:" << err;
                refreshAssetTable();
                emit entityRefreshRequested();
              });
      bl->addWidget(primary);
    }

    bl->addStretch(1);
    table->setCellWidget(r, 2, cell);
  }
  refreshAssetEmptyState(table, unresolvedOnly ? tr("没有未决资产 — 全部资产都已挂接") : QString());
  m_selKeep.snapshotIds(m_pageSelection);
  m_selKeep.restoreTable(table);
  refreshAssetTree();
  m_selKeep.restoreTree(m_tree);
}

void DataListPanel::refreshAssetTree()
{
  if (!m_tree)
    return;
  m_tree->clear();
  PreviewDocService *svc = m_doc;
  if (!svc)
    return;
  DataCatalog *cat = svc->catalog();
  if (!cat)
    return;

  if (m_rows.size() > 200)
  {
    // 树、图标、分组共享当前页；实体节点保留井选中与激活语义。
    auto *survey = new QTreeWidgetItem(m_tree);
    survey->setText(0, tr("测区")); survey->setData(0, Qt::UserRole, QStringLiteral("survey_area"));
    survey->setData(0, Qt::UserRole + 2, QStringLiteral("survey_area"));
    QHash<QString, QTreeWidgetItem *> entityNodes;
    for (const auto &row : m_pageRows)
    {
      QTreeWidgetItem *parent = nullptr;
      QString wellId;
      for (const auto &link : cat->linksForAsset(row.assetId))
        if (!link.unresolved && !link.entityId.isEmpty()) {
          const auto entity = cat->entityById(link.entityId);
          parent = entityNodes.value(entity.id);
          if (!parent) {
            parent = new QTreeWidgetItem(m_tree); parent->setText(0, m_entityOv.displayName(entity));
            parent->setData(0, Qt::UserRole + 1, entity.id);
            parent->setData(0, Qt::UserRole + 2, entity.entityType);
            parent->setExpanded(true); entityNodes.insert(entity.id, parent);
          }
          if (entity.entityType == QLatin1String("well")) wellId = entity.id;
          break;
        }
      auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
      item->setText(0, row.displayName); item->setText(1, row.effectiveType);
      item->setData(0, Qt::UserRole, row.assetId); item->setData(0, Qt::UserRole + 1, wellId);
      item->setData(0, Qt::UserRole + 2, QStringLiteral("asset"));
    }
    return;
  }

  // 0. 测区 (Survey Area) — 首项显示，双击打开测区全景地图 (QGIS 画布)
  auto *surveyRoot = new QTreeWidgetItem(m_tree);
  surveyRoot->setText(0, tr("测区"));
  surveyRoot->setText(1, tr("工区全景"));
  surveyRoot->setData(0, Qt::UserRole, QStringLiteral("survey_area"));
  surveyRoot->setData(0, Qt::UserRole + 2, QStringLiteral("survey_area"));
  surveyRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  surveyRoot->setExpanded(true);

  auto *surveyMapItem = new QTreeWidgetItem(surveyRoot);
  surveyMapItem->setText(0, tr("工区全景地图"));
  surveyMapItem->setText(1, tr("QGIS地图画布"));
  surveyMapItem->setData(0, Qt::UserRole, QStringLiteral("survey_area"));
  surveyMapItem->setData(0, Qt::UserRole + 2, QStringLiteral("survey_area"));
  surveyMapItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));

  // 1. 井 (Wells)——显示名走实体改写表（D4.1），排序按 D2.5 记忆键。
  QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
  // 软删实体（撤销新建等）不显示。
  const auto wellsVisible = [this](const QVector<CatalogEntity> &in) {
    QVector<CatalogEntity> out;
    for (const CatalogEntity &e : in)
      if (!m_recycle.isRemoved(e.id))
        out.append(e);
    return out;
  };
  wells = wellsVisible(wells);
  const paleo::dataops::EntityOverrideStore *ovStore = &m_entityOv;
  std::sort(wells.begin(), wells.end(),
            [ovStore, this](const CatalogEntity &a, const CatalogEntity &b) {
              if (m_treeSort == paleo::dataops::TreeSortKind::Time)
              {
                const paleo::dataops::EntityOverride oa = ovStore->overrideFor(a.id);
                const paleo::dataops::EntityOverride ob = ovStore->overrideFor(b.id);
                return false; // 实体无时间面——保持稳定序
              }
              return naturalNameSort(ovStore->displayName(a), ovStore->displayName(b));
            });
  auto *wellRoot = new QTreeWidgetItem(m_tree);
  wellRoot->setText(0, tr("井 (%1)").arg(wells.size()));
  wellRoot->setText(1, tr("井位 / 测井曲线 / 分层 / 时深"));
  wellRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  wellRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
  wellRoot->setExpanded(false);

  for (const CatalogEntity &w : wells)
  {
    auto *wellItem = new QTreeWidgetItem(wellRoot);
    wellItem->setText(0, m_entityOv.displayName(w));
    wellItem->setData(0, Qt::UserRole + 1, w.id);
    wellItem->setData(0, Qt::UserRole + 2, QStringLiteral("well"));
    wellItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
    if (w.hasSurface)
    {
      wellItem->setText(1, QStringLiteral("X: %1, Y: %2").arg(QString::number(w.surfaceX, 'f', 1)).arg(QString::number(w.surfaceY, 'f', 1)));
      wellItem->setFont(1, PaleoTheme::monoFont()); // 坐标列走 mono 数字面（DESIGN.md）
    }

    // 获取该井所有关联资产并按角色序排：测井曲线 -> 井分层 -> 时深关系 -> 井身/井位
    const QVector<EntityAssetLink> wLinks = cat->linksForEntity(w.id);
    const auto roleOrder = [](const QString &r) {
      if (r == QLatin1String("well_log")) return 0;
      if (r == QLatin1String("tops")) return 1;
      if (r == QLatin1String("time_depth")) return 2;
      if (r == QLatin1String("well_head")) return 3;
      return 4;
    };
    QVector<EntityAssetLink> sortedLinks = wLinks;
    std::stable_sort(sortedLinks.begin(), sortedLinks.end(),
                     [roleOrder](const EntityAssetLink &a, const EntityAssetLink &b) {
                       const int ra = roleOrder(a.role);
                       const int rb = roleOrder(b.role);
                       if (ra != rb)
                         return ra < rb;
                       if (a.ordinal != b.ordinal)
                         return a.ordinal < b.ordinal;
                       return false;
                     });

    for (const EntityAssetLink &l : sortedLinks)
    {
      const CatalogAsset a = cat->assetById(l.assetId);
      if (a.id.isEmpty())
        continue;
      auto *sub = new QTreeWidgetItem(wellItem);
      sub->setData(0, Qt::UserRole, a.id);
      sub->setData(0, Qt::UserRole + 1, w.id);
      sub->setData(0, Qt::UserRole + 2, l.role);

      QString roleDisplay = l.role;
      QString detailDisplay;
      if (l.role == QLatin1String("well_log"))
      {
        roleDisplay = tr("测井曲线");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
      }
      else if (l.role == QLatin1String("tops"))
      {
        roleDisplay = tr("井分层");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      else if (l.role == QLatin1String("time_depth"))
      {
        roleDisplay = tr("时深关系");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      else if (l.role == QLatin1String("well_head"))
      {
        roleDisplay = tr("井身/井位");
        detailDisplay = a.displayName;
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
      }
      else
      {
        sub->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      }
      if (l.role == QLatin1String("well_log") && !l.unresolved)
      {
        const QString kind = l.isPrimary ? tr("主文件") : tr("成员");
        sub->setText(0, tr("%1 · %2 (%3)").arg(a.displayName, kind, roleDisplay));
        sub->setText(1, tr("%1 · %2").arg(kind, a.displayName));
      }
      else
      {
        sub->setText(0, QStringLiteral("%1 (%2)").arg(a.displayName, roleDisplay));
        sub->setText(1, detailDisplay);
      }
      if (l.unresolved)
      {
        PaleoTheme::setItemTextColor(sub, 0, PaleoTheme::ItemTextColor::Warning); // 待复核色（现取随主题）
        sub->setText(1, tr("未决关联"));
      }
    }
  }

  // 资产挂在 auxiliary 实体（reference 角色）上 = 辅助资料，不进测区井/测井分组。
  // D1.6 软删资产全树不显示。
  const auto assetVisible = [this](const CatalogAsset &a) {
    return !m_recycle.isRemoved(a.id);
  };
  const auto linkedToAuxEntity = [cat](const QString &assetId) {
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    {
      if (!l.entityId.isEmpty() &&
          cat->entityById(l.entityId).entityType == QLatin1String("auxiliary"))
        return true;
    }
    return false;
  };

  // 1b. 计划井 (Planned：方向34 布井候选)——虚拟部署实体，独立成组
  // 与实井分组隔开；显示名同走实体改写表，软删实体不显示。
  {
    QVector<CatalogEntity> planned = cat->entities(QStringLiteral("planned"));
    QVector<CatalogEntity> plannedVisible;
    for (const CatalogEntity &e : planned)
      if (!m_recycle.isRemoved(e.id) &&
          (!m_plannedVisible || m_plannedVisible(e.id)))
        plannedVisible.append(e);
    std::sort(plannedVisible.begin(), plannedVisible.end(),
              [ovStore](const CatalogEntity &a, const CatalogEntity &b) {
                return naturalNameSort(ovStore->displayName(a), ovStore->displayName(b));
              });
    auto *plannedRoot = new QTreeWidgetItem(m_tree);
    plannedRoot->setText(0, tr("计划井 (%1)").arg(plannedVisible.size()));
    plannedRoot->setText(1, tr("布井候选（不进实井计算）"));
    plannedRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    plannedRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
    plannedRoot->setExpanded(false);
    for (const CatalogEntity &p : plannedVisible)
    {
      auto *item = new QTreeWidgetItem(plannedRoot);
      item->setText(0, m_entityOv.displayName(p));
      item->setData(0, Qt::UserRole + 1, p.id);
      item->setData(0, Qt::UserRole + 2, QStringLiteral("planned"));
      item->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
      if (p.hasSurface)
      {
        item->setText(1, QStringLiteral("X: %1, Y: %2")
                                 .arg(QString::number(p.surfaceX, 'f', 1))
                                 .arg(QString::number(p.surfaceY, 'f', 1)));
        item->setFont(1, PaleoTheme::monoFont());
      }
    }
  }

  // 2. 测井 (Well Logs: 综合柱状图 + 测井曲线)
  QList<CatalogAsset> logAssets;
  QList<CatalogAsset> compositeAssets;
  for (const CatalogAsset &a : cat->assets())
  {
    if (!assetVisible(a))
      continue;
    if (a.type == QLatin1String("well_log") || a.displayName.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
    {
      logAssets.append(a);
    }
    else if ((a.displayName.contains(QStringLiteral("柱状图")) ||
              (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) && a.displayName.contains(QStringLiteral("综合")))) &&
             !linkedToAuxEntity(a.id))
    {
      compositeAssets.append(a);
    }
  }

  std::sort(logAssets.begin(), logAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  std::sort(compositeAssets.begin(), compositeAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });

  const int totalLogs = logAssets.size() + compositeAssets.size();
  auto *logRoot = new QTreeWidgetItem(m_tree);
  logRoot->setText(0, tr("测井 (%1)").arg(totalLogs));
  logRoot->setText(1, tr("综合柱状图 / 测井曲线 (LAS)"));
  logRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  logRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
  logRoot->setExpanded(true);

  if (!compositeAssets.isEmpty())
  {
    auto *compBranch = new QTreeWidgetItem(logRoot);
    compBranch->setText(0, tr("综合柱状图 (%1)").arg(compositeAssets.size()));
    compBranch->setText(1, tr("多井道地质综合柱状图 (ResFormStar 规范)"));
    compBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    compBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    compBranch->setExpanded(true);

    for (const CatalogAsset &a : compositeAssets)
    {
      auto *it = new QTreeWidgetItem(compBranch);
      it->setText(0, a.displayName);
      it->setText(1, tr("多井道地质综合柱状图")); // 道数/曲线数未解析，不臆造
      it->setData(0, Qt::UserRole, a.id);
      it->setData(0, Qt::UserRole + 2, QStringLiteral("composite_log"));
      it->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    }
  }

  if (!logAssets.isEmpty())
  {
    auto *curveBranch = new QTreeWidgetItem(logRoot);
    curveBranch->setText(0, tr("测井曲线 (%1)").arg(logAssets.size()));
    curveBranch->setText(1, tr("LAS 连续测井曲线数据"));
    curveBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
    curveBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    curveBranch->setExpanded(true);

    for (const CatalogAsset &a : logAssets)
    {
      auto *it = new QTreeWidgetItem(curveBranch);
      QString linkedWellName;
      QString linkedWellId;
      for (const auto &w : wells)
      {
        const auto links = cat->linksForEntity(w.id);
        for (const auto &lk : links)
        {
          if (lk.assetId == a.id)
          {
            linkedWellName = w.name;
            linkedWellId = w.id;
            break;
          }
        }
        if (!linkedWellName.isEmpty())
          break;
      }

      if (!linkedWellName.isEmpty())
      {
        it->setText(0, tr("%1 · 井 %2").arg(a.displayName, linkedWellName));
        it->setData(0, Qt::UserRole + 1, linkedWellId);
      }
      else
      {
        it->setText(0, a.displayName);
      }

      it->setText(1, tr("测井曲线 (GR/AC/DEN/电阻率等)"));
      it->setData(0, Qt::UserRole, a.id);
      it->setData(0, Qt::UserRole + 2, QStringLiteral("well_log"));
      it->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
    }
  }

  // 3. 地震 (Seismic)
  QList<CatalogAsset> seisAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("seismic"))
      seisAssets.append(a);
  auto *seismicRoot = new QTreeWidgetItem(m_tree);
  seismicRoot->setText(0, tr("地震 (%1)").arg(seisAssets.size()));
  seismicRoot->setText(1, tr("三维地震数据体"));
  seismicRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  seismicRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  seismicRoot->setExpanded(true);

  const QVector<CatalogEntity> surveys = cat->entities(QStringLiteral("seismic_survey"));
  CatalogEntity survey;
  if (!surveys.isEmpty())
    survey = surveys.front();

  for (const CatalogAsset &a : seisAssets)
  {
    auto *seisItem = new QTreeWidgetItem(seismicRoot);
    seisItem->setText(0, a.displayName);
    seisItem->setData(0, Qt::UserRole, a.id);
    seisItem->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_volume"));
    seisItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
    if (!survey.id.isEmpty() && survey.inlineMax > survey.inlineMin)
    {
      seisItem->setText(1, QStringLiteral("Inline %1–%2 · Xline %3–%4 · %5ms")
          .arg(int(survey.inlineMin)).arg(int(survey.inlineMax))
          .arg(int(survey.xlineMin)).arg(int(survey.xlineMax))
          .arg(survey.sampleIntervalUs / 1000.0, 0, 'f', 1));
      
      auto *inl = new QTreeWidgetItem(seisItem);
      inl->setText(0, tr("Inline 剖面 (主测线 %1–%2)").arg(int(survey.inlineMin)).arg(int(survey.inlineMax)));
      inl->setText(1, tr("双击预览剖面"));
      inl->setData(0, Qt::UserRole, a.id);
      inl->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_line"));
      inl->setData(0, Qt::UserRole + 3, QStringLiteral("inline"));
      inl->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));

      auto *xl = new QTreeWidgetItem(seisItem);
      xl->setText(0, tr("Crossline 剖面 (联络线 %1–%2)").arg(int(survey.xlineMin)).arg(int(survey.xlineMax)));
      xl->setText(1, tr("双击预览剖面"));
      xl->setData(0, Qt::UserRole, a.id);
      xl->setData(0, Qt::UserRole + 2, QStringLiteral("seismic_line"));
      xl->setData(0, Qt::UserRole + 3, QStringLiteral("crossline"));
      xl->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconLineLayer.svg")));
      seisItem->setExpanded(true);
    }
    else
    {
      seisItem->setText(1, tr("SEG-Y 地震数据"));
    }
  }

  // 4. 层位 (Horizons)
  QList<CatalogAsset> horAssets;
  for (const CatalogAsset &a : cat->assets())
    if (assetVisible(a) && a.type == QLatin1String("horizon"))
      horAssets.append(a);
  std::sort(horAssets.begin(), horAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  auto *horRoot = new QTreeWidgetItem(m_tree);
  horRoot->setText(0, tr("层位 (%1)").arg(horAssets.size()));
  horRoot->setText(1, tr("解释层位数据"));
  horRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  horRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
  horRoot->setExpanded(true);

  for (const CatalogAsset &a : horAssets)
  {
    auto *hItem = new QTreeWidgetItem(horRoot);
    hItem->setText(0, a.displayName);
    hItem->setData(0, Qt::UserRole, a.id);
    hItem->setData(0, Qt::UserRole + 2, QStringLiteral("horizon"));
    hItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
    // 网格规格不在 catalog 里（按文件名臆造 411×641 已回收）——如实标类型。
    hItem->setText(1, tr("层位网格"));
  }

  // 5. 辅助资料 (Auxiliary)
  QList<CatalogAsset> auxAssets;
  for (const CatalogAsset &a : cat->assets())
  {
    if (!assetVisible(a))
      continue;
    // 挂在辅助实体下的综合柱状图（如参考井 XML）归参考资料，其余柱状图进测井分组。
    const bool isCompositeXml = a.displayName.contains(QStringLiteral("柱状图")) ||
        (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive) && a.displayName.contains(QStringLiteral("综合")));
    if (isCompositeXml && !linkedToAuxEntity(a.id))
      continue;
    if (isCompositeXml)
    {
      auxAssets.append(a);
      continue;
    }

    if (a.type == QLatin1String("boundary") || a.type == QLatin1String("auxiliary") ||
        a.type == QLatin1String("document") || a.type == QLatin1String("reference") ||
        a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive))
      auxAssets.append(a);
  }
  std::sort(auxAssets.begin(), auxAssets.end(), [](const CatalogAsset &a, const CatalogAsset &b) {
    return naturalNameSort(a.displayName, b.displayName);
  });
  auto *auxRoot = new QTreeWidgetItem(m_tree);
  auxRoot->setText(0, tr("辅助资料 (%1)").arg(auxAssets.size()));
  auxRoot->setText(1, tr("参考相图 / 文档 / 图片"));
  auxRoot->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  auxRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
  auxRoot->setExpanded(true);

  auto *faciesBranch = new QTreeWidgetItem(auxRoot);
  faciesBranch->setText(0, tr("参考相图"));
  faciesBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  faciesBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  faciesBranch->setExpanded(true);

  auto *docBranch = new QTreeWidgetItem(auxRoot);
  docBranch->setText(0, tr("参考资料"));
  docBranch->setData(0, Qt::UserRole + 2, QStringLiteral("category"));
  docBranch->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
  docBranch->setExpanded(true);

  for (const CatalogAsset &a : auxAssets)
  {
    const bool isGeo = a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive) || a.type == QLatin1String("boundary");
    QTreeWidgetItem *parent = isGeo ? faciesBranch : docBranch;
    auto *it = new QTreeWidgetItem(parent);
    it->setText(0, a.displayName);
    it->setData(0, Qt::UserRole, a.id);
    it->setData(0, Qt::UserRole + 2, QStringLiteral("auxiliary"));
    it->setIcon(0, PaleoIcons::qgisTheme(isGeo ? QStringLiteral("mIconPolygonLayer.svg") : QStringLiteral("mActionOpenTable.svg")));
    if (isGeo)
      it->setText(1, tr("GeoJSON 矢量相图"));
    else if (a.displayName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      it->setText(1, tr("PDF 文档"));
    else if (a.displayName.endsWith(QLatin1String(".pptx"), Qt::CaseInsensitive))
      it->setText(1, tr("PPT 演示文稿"));
    else if (a.displayName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
      it->setText(1, tr("图像"));
    else if (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
      it->setText(1, tr("XML 数据表"));
    else
      it->setText(1, a.type);
  }

  // 6. 标签分组（D2.4 组织面 + D3.5 拖放目标）：每个标签一个叶节点，
  //    拖资产到标签节点 = 打标签；点击标签 = 过滤。
  const auto tagCloud = m_tags.tagCloud();
  if (!tagCloud.isEmpty())
  {
    auto *tagRoot = new QTreeWidgetItem(m_tree);
    tagRoot->setText(0, tr("标签 (%1)").arg(tagCloud.size()));
    tagRoot->setText(1, tr("点击过滤 / 拖资产来打标签"));
    tagRoot->setData(0, Qt::UserRole + 2, QStringLiteral("tag_group"));
    tagRoot->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionFolder.svg")));
    tagRoot->setExpanded(false);
    for (const auto &tc : tagCloud)
    {
      auto *leaf = new QTreeWidgetItem(tagRoot);
      leaf->setText(0, QStringLiteral("%1 ×%2").arg(tc.first).arg(tc.second));
      leaf->setData(0, Qt::UserRole + 2, QStringLiteral("tag_leaf"));
      leaf->setData(0, Qt::UserRole + 3, tc.first);
      leaf->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
      // 点击标签节点 = 按标签过滤（与标签云同语义）。
    }
  }
}
void DataListPanel::applyListFilter()
{
  using namespace paleo::dataops;
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  syncLegacyControlsIntoFilter();
  // 可见集 = 行快照 × FilterGroup × 激活标签（D2.2/D2.4）。
  QSet<QString> visibleIds;
  int shown = 0;
  for (const AssetRowInfo &row : m_rows)
  {
    if (!m_activeTag.isEmpty() && !row.tags.contains(m_activeTag, Qt::CaseInsensitive))
      continue;
    if (!m_filter.matches(row))
      continue;
    visibleIds.insert(row.assetId);
    ++shown;
  }
  QVector<AssetRowInfo> filtered;
  for (const auto &row : m_rows)
    if (visibleIds.contains(row.assetId))
      filtered.append(row);
  const QString sortField = m_assetSortColumn == 1 ? QStringLiteral("type")
                           : m_assetSortColumn == 2 ? QStringLiteral("entity") : QStringLiteral("name");
  if (m_assetSortColumn >= 0) sortAssetRows(&filtered, sortField, m_assetSortAscending);
  m_filteredRows = filtered;
  const bool paged = m_rows.size() > 200;
  const int pageSize = qMax(1, table->viewport()->height() / qMax(1, table->verticalHeader()->defaultSectionSize()));
  const int pages = qMax(1, int((filtered.size() + pageSize - 1) / pageSize));
  m_assetPage = qBound(0, m_assetPage, pages - 1);
  auto pageRows = paged ? filtered.mid(m_assetPage * pageSize, pageSize) : m_rows;
  if (!paged && m_assetSortColumn >= 0) sortAssetRows(&pageRows, sortField, m_assetSortAscending);
  bool samePage = pageRows.size() == m_pageRows.size();
  for (int i = 0; samePage && i < pageRows.size(); ++i)
    samePage = pageRows.at(i).assetId == m_pageRows.at(i).assetId;
  m_pageRows = pageRows;
  if (m_tableDirty || !samePage) { renderAssetPage(); m_tableDirty = false; }
  table->setProperty("paleo.totalAssets", m_rows.size());
  table->setProperty("paleo.pageSize", pageSize);
  if (auto *pager = findChild<QWidget *>(QStringLiteral("assetPager")))
    pager->setVisible(paged && (!m_viewStack || m_viewStack->currentIndex() != 3));
  if (auto *label = findChild<QLabel *>(QStringLiteral("assetPageLabel")))
    label->setText(tr("第 %1 / %2 页 · %3 项").arg(m_assetPage + 1).arg(pages).arg(shown));
  if (auto *btn = findChild<QPushButton *>(QStringLiteral("assetPreviousPage")))
    btn->setEnabled(m_assetPage > 0);
  if (auto *btn = findChild<QPushButton *>(QStringLiteral("assetNextPage")))
    btn->setEnabled(m_assetPage + 1 < pages);
  const int total = int(m_rows.size());
  for (int r = 0; r < table->rowCount(); ++r)
  {
    const QTableWidgetItem *name = table->item(r, 0);
    // 空态指引行（无 UserRole）不算数据，也永远不藏。
    if (!name || name->data(Qt::UserRole).toString().isEmpty())
    {
      table->setRowHidden(r, false);
      continue;
    }
    table->setRowHidden(r, !visibleIds.contains(name->data(Qt::UserRole).toString()));
  }
  if (auto *count = findChild<QLabel *>(QStringLiteral("assetCountLabel")))
    count->setText(shown == total ? tr("共 %1 条").arg(total)
                                  : tr("显示 %1 / 共 %2 条").arg(shown).arg(total));

  // D2.7 高亮 needle（搜索词来自 Search 条件或旧搜索框）。
  if (m_delegate)
  {
    QString needle;
    for (const FilterCondition &c : m_filter.conditions)
      if (c.dim == FilterDim::Search && !c.negate)
        needle = c.value;
    m_delegate->setNeedle(needle);
    if (m_delegate->needle().isEmpty())
    {
      const auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
      m_delegate->setNeedle(search ? search->text().trimmed() : QString());
    }
  }

  // 树形视图过滤（P3：按可见 id 集 + 文本匹配递归）。
  const bool filtering = !m_filter.conditions.isEmpty() || !m_activeTag.isEmpty();
  applyFilterToTree(visibleIds, filtering);

  // D2.9 空结果态：有过滤而零命中 → 放宽/清除快捷钮（空态指引行仍显示）。
  if (m_emptyState)
  {
    const bool noHits = filtering && shown == 0 && total > 0;
    m_emptyState->setVisible(noHits);
    if (noHits)
      m_emptyState->setMessage(
          tr("没有匹配的资产 — 试试放宽条件（当前 %1 个条件%2）")
              .arg(m_filter.conditions.size())
              .arg(m_activeTag.isEmpty() ? QString()
                                         : tr(" + 标签「%1」").arg(m_activeTag)));
  }

  // D7 新视图页同口径重灌（图标/分组；虚拟模型给全量行——fetchMore 自管）。
  if (m_iconView)
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    m_iconView->loadRows(paged ? m_pageRows : vis);
    if (m_groupTree)
      m_groupTree->loadRows(paged ? m_pageRows : vis);
  }
  if (m_virtualView)
  {
    QVector<AssetRowInfo> vis;
    for (const AssetRowInfo &row : m_rows)
      if (visibleIds.contains(row.assetId))
        vis.append(row);
    // D7.4 稳定排序（多列次级）：名称升序为次级键。
    sortAssetRows(&vis, QStringLiteral("name"), true);
    m_virtualView->flatModel()->setRows(vis);
    if (m_virtualView->horizontalHeader())
      restoreColumnState(QStringLiteral("assetVirtualTable"),
                         m_virtualView->horizontalHeader());
  }
}

void DataListPanel::setUnresolvedFilter(bool on)
{
  setProperty("paleo.page.filterUnresolved", on);
  if (auto *bar = findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
    bar->setVisible(on);
  refreshAssetTable();
}
void DataListPanel::selectAssetsForEntities(const QStringList &entityIds)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  PreviewDocService *svc = m_doc;
  if (!table || !svc || entityIds.isEmpty())
    return;
  // 实体 → 已决关联资产集合（未决链接实体 id 为空，天然不命中）。
  QSet<QString> wanted;
  for (const QString &eid : entityIds)
    for (const EntityAssetLink &l : svc->catalog()->linksForEntity(eid))
      if (!l.unresolved)
        wanted.insert(l.assetId);
  QSet<QString> live;
  for (const auto &row : m_rows) live.insert(row.assetId);
  wanted.intersect(live);
  if (wanted.isEmpty()) return;
  if (m_rows.size() > 200)
  {
    QString first = *wanted.constBegin();
    for (const auto &row : m_filteredRows)
      if (wanted.contains(row.assetId)) { first = row.assetId; break; }
    selectAssetInViews(first);
    m_pageSelection = wanted;
  }
  // 一次应用整份选中（QTableView::selectRow 是单点替换语义，逐行调会互相
  // 顶掉）；选中变化照发 itemSelectionChanged → assetActivated 首个命中行。
  QItemSelection sel;
  QTableWidgetItem *firstHit = nullptr;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    QTableWidgetItem *it = table->item(r, 0);
    if (!it || !wanted.contains(it->data(Qt::UserRole).toString()))
      continue;
    sel.select(table->model()->index(r, 0),
               table->model()->index(r, table->columnCount() - 1));
    if (!firstHit)
      firstHit = it;
  }
  if (sel.isEmpty())
    return;
  // P3 D1：程序化多选守卫——选中变化仍发首个命中行的 assetActivated（旧行为），
  // 但用户 Ctrl 多选不逐个激活。刷新期间的选中还原走 QSignalBlocker 不入此路径。
  m_progSelect = true;
  table->selectionModel()->select(
      sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  m_progSelect = false;
  if (m_rows.size() > 200) m_pageSelection.unite(wanted);
  if (firstHit)
    table->scrollToItem(firstHit);
}
void DataListPanel::selectAssetInViews(const QString &assetId)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (table && m_rows.size() > 200)
  {
    bool live = false;
    for (const auto &row : m_rows) if (row.assetId == assetId) { live = true; break; }
    if (!live) return;
    m_pageSelection = {assetId};
    { const QSignalBlocker block(table); table->clearSelection(); }
    if (m_tree) { const QSignalBlocker block(m_tree); m_tree->clearSelection(); }
    for (int i = 0; i < m_filteredRows.size(); ++i)
      if (m_filteredRows.at(i).assetId == assetId)
      {
        const int pageSize = table->property("paleo.pageSize").toInt();
        m_assetPage = i / qMax(1, pageSize);
        m_pageSelection = {assetId};
        applyListFilter();
        break;
      }
  }
  if (table)
  {
    for (int r = 0; r < table->rowCount(); ++r)
    {
      QTableWidgetItem *it = table->item(r, 0);
      if (it && it->data(Qt::UserRole).toString() == assetId)
      {
        const QSignalBlocker b(table);
        table->setCurrentCell(r, 0);
        break;
      }
    }
  }

  if (m_tree)
  {
    const QList<QTreeWidgetItem *> sel = m_tree->selectedItems();
    if (sel.isEmpty() || sel.front()->data(0, Qt::UserRole).toString() != assetId)
    {
      QTreeWidgetItemIterator it(m_tree);
      while (*it)
      {
        if ((*it)->data(0, Qt::UserRole).toString() == assetId)
        {
          const QSignalBlocker b(m_tree);
          m_tree->setCurrentItem(*it);
          break;
        }
        ++it;
      }
    }
  }
}
// ===========================================================================
// P3 数据操作重构（wave/data-page-operations）：多选/过滤/拖拽/批量/撤销/
// 视图形态。以下方法按 docs/dataops/OPERATIONS.md 的操作矩阵实现。
// ===========================================================================

namespace
{
// 快捷键表（D6.3 数据源；registerCommands 同步登记）。
struct ShortcutSpec
{
  const char *id;
  const char *title;
  const char *shortcut;
  const char *category;
};
const ShortcutSpec kShortcutSpecs[] = {
  {"dataops.undo", QT_TR_NOOP("撤销"), "Ctrl+Z", QT_TR_NOOP("编辑")},
  {"dataops.redo", QT_TR_NOOP("重做"), "Ctrl+Y", QT_TR_NOOP("编辑")},
  {"dataops.selectAll", QT_TR_NOOP("全选可见项"), "Ctrl+A", QT_TR_NOOP("选择")},
  {"dataops.invertSelection", QT_TR_NOOP("反选"), "Ctrl+Shift+A", QT_TR_NOOP("选择")},
  {"dataops.selectFiltered", QT_TR_NOOP("按过滤器选中"), "Ctrl+Shift+F", QT_TR_NOOP("选择")},
  {"dataops.commandPalette", QT_TR_NOOP("命令面板"), "Ctrl+Shift+P", QT_TR_NOOP("工具")},
  {"dataops.shortcutsDialog", QT_TR_NOOP("快捷键表"), "?", QT_TR_NOOP("帮助")},
  {"dataops.focusSearch", QT_TR_NOOP("聚焦搜索"), "Ctrl+F", QT_TR_NOOP("工具")},
  {"dataops.vimToggle", QT_TR_NOOP("Vim 风导航开关"), "Ctrl+Alt+V", QT_TR_NOOP("工具")},
  {"dataops.recursiveExpand", QT_TR_NOOP("全部展开"), "", QT_TR_NOOP("视图")},
  {"dataops.collapseAll", QT_TR_NOOP("全部折叠"), "", QT_TR_NOOP("视图")},
};
} // namespace

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
  const auto addShortcut = [this](QKeySequence key, const char *name,
                                  std::function<void()> handler) {
    auto *sc = new QShortcut(key, this);
    sc->setObjectName(QLatin1String(name));
    m_shortcuts.append({key.toString(), QLatin1String(name)});
    connect(sc, &QShortcut::activated, this, std::move(handler));
    return sc;
  };
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), "scUndo",
              [this] { undoOp(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), "scRedo",
              [this] { redoOp(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), "scInvert",
              [this] { invertAssetSelection(); });
  addShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), "scSelectFiltered",
              [this] { selectByCurrentFilter(); });
  addShortcut(QKeySequence(Qt::Key_F2), "scRename", [this] {
    // D4.1：树内实体节点 F2 → 实体重命名意图（EntityPanel/壳接）。
    const QStringList ents = currentEntitySelection();
    if (!ents.isEmpty())
      emit entityRenameRequested(ents.front());
  });
  addShortcut(QKeySequence(Qt::Key_Question), "scShortcuts", [this] {
    emit shortcutsDialogRequested();
  });
  addShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), "scFocusSearch", [this] {
    if (auto *edit = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit")))
      edit->setFocus();
  });
  addShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), "scVimToggle", [this] {
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

void DataListPanel::registerCommands()
{
  using namespace paleo::dataops;
  const ShortcutSpec *specs = kShortcutSpecs;
  const int n = int(sizeof(kShortcutSpecs) / sizeof(kShortcutSpecs[0]));
  for (int i = 0; i < n; ++i)
  {
    CommandEntry e;
    e.id = QLatin1String(specs[i].id);
    e.title = tr(specs[i].title);
    e.category = tr(specs[i].category);
    e.shortcut = QLatin1String(specs[i].shortcut);
    e.keywords << e.title << e.category;
    m_reg.registerCommand(e);
  }
  const auto reg = [this](const char *id, const char *title, const char *cat,
                          std::function<void()> fn) {
    CommandEntry e;
    e.id = QLatin1String(id);
    e.title = tr(title);
    e.category = tr(cat);
    e.trigger = std::move(fn);
    e.keywords << e.title << tr(cat);
    m_reg.registerCommand(e);
  };
  reg("dataops.selectAllVisible", QT_TR_NOOP("全选可见项"), QT_TR_NOOP("选择"),
      [this] { selectAllVisibleAssets(); });
  reg("dataops.invertSel", QT_TR_NOOP("反选"), QT_TR_NOOP("选择"),
      [this] { invertAssetSelection(); });
  reg("dataops.attachBatch", QT_TR_NOOP("批量挂接到实体"), QT_TR_NOOP("批量"),
      [this] { batchAttachToEntity(); });
  reg("dataops.changeType", QT_TR_NOOP("批量改类型"), QT_TR_NOOP("批量"),
      [this] { batchChangeType(); });
  reg("dataops.removeSoft", QT_TR_NOOP("移除（软删）"), QT_TR_NOOP("批量"),
      [this] { batchRemoveSoft(); });
  reg("dataops.exportManifest", QT_TR_NOOP("导出清单 CSV/JSON"), QT_TR_NOOP("批量"),
      [this] { batchExportManifest(); });
  reg("dataops.openPreviewAll", QT_TR_NOOP("批量打开预览"), QT_TR_NOOP("批量"),
      [this] { batchOpenPreview(); });
  reg("dataops.addTag", QT_TR_NOOP("给选中打标签"), QT_TR_NOOP("批量"),
      [this] { batchAddTag(); });
  reg("dataops.recycleBin", QT_TR_NOOP("打开可回收清单"), QT_TR_NOOP("批量"),
      [this] { showRecycleBin(); });
  reg("dataops.clearFilter", QT_TR_NOOP("清除全部过滤"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.conditions.clear();
        m_activeTag.clear();
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.filterUnlinked", QT_TR_NOOP("过滤：未挂接"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.add({FilterDim::Unlinked, QString(), false});
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.filterPending", QT_TR_NOOP("过滤：有警告"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.add({FilterDim::Warned, QString(), false});
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.viewTree", QT_TR_NOOP("视图：树形"), QT_TR_NOOP("视图"),
      [this] { setViewMode(0); });
  reg("dataops.viewTable", QT_TR_NOOP("视图：列表"), QT_TR_NOOP("视图"),
      [this] { setViewMode(1); });
  reg("dataops.viewIcon", QT_TR_NOOP("视图：图标"), QT_TR_NOOP("视图"),
      [this] { setViewMode(2); });
  reg("dataops.viewVirtual", QT_TR_NOOP("视图：高速（大数据）"), QT_TR_NOOP("视图"),
      [this] { setViewMode(3); });
  reg("dataops.viewGroup", QT_TR_NOOP("视图：分组"), QT_TR_NOOP("视图"),
      [this] { setViewMode(4); });
  reg("dataops.refresh", QT_TR_NOOP("刷新数据列表"), QT_TR_NOOP("视图"),
      [this] { refreshAssetTable(); });
}

void DataListPanel::refreshChipBar()
{
  if (m_chipBar)
    m_chipBar->setConditions(m_filter);
  if (m_filterBar)
  {
    // 维度词表（下拉候选）。
    QSet<QString> types, statuses, roles, entities, tags;
    for (const paleo::dataops::AssetRowInfo &r : m_rows)
    {
      if (!types.contains(r.effectiveType) && !r.effectiveType.isEmpty())
        types.insert(r.effectiveType);
      if (!statuses.contains(r.status))
        statuses.insert(r.status);
      for (const QString &ro : r.roles)
        if (!roles.contains(ro))
          roles.insert(ro);
      for (const QString &en : r.entityNames)
        if (!entities.contains(en))
          entities.insert(en);
      for (const QString &t : r.tags)
        if (!tags.contains(t))
          tags.insert(t);
    }
    const auto sorted = [](const QSet<QString> &set) { QStringList list = set.values(); list.sort(); return list; };
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Type, sorted(types));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Status,
                                    {QStringLiteral("RAW"), QStringLiteral("DERIVED")});
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Role, sorted(roles));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Entity, sorted(entities));
    m_filterBar->setValueVocabulary(paleo::dataops::FilterDim::Tag, sorted(tags));
    m_filterBar->reloadPresets();
  }
}

void DataListPanel::loadStoresForCatalog()
{
  using namespace paleo::dataops;
  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  if (!cat || !cat->isOpen())
    return;
  const QString path = cat->catalogPath();
  if (path != m_lastCatalogPath)
  {
    // D5.6：项目会话切换 → 命令栈/历史清空（旧工程的命令对新 catalog 无意义）。
    m_lastCatalogPath = path;
    m_pageSelection.clear();
    m_assetPage = 0;
    if (m_opStack)
      m_opStack->clear();
    if (m_history)
      m_history->clear();
  }
  m_tags.load(cat);
  m_typeOv.load(cat);
  m_entityOv.load(cat);
  m_recycle.load(cat);
  m_ctx.cat = cat;
  m_ctx.tags = &m_tags;
  m_ctx.assetOverrides = &m_typeOv;
  m_ctx.entityOverrides = &m_entityOv;
  m_ctx.recycle = &m_recycle;
}

void DataListPanel::rebuildRowSnapshot()
{
  using namespace paleo::dataops;
  m_rows.clear();
  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  if (!cat)
    return;
  const bool unresolvedOnly = property("paleo.page.filterUnresolved").toBool();
  const auto assets = cat->assets();
  m_rows.reserve(assets.size());
  for (const CatalogAsset &a : assets)
  {
    if (m_recycle.isRemoved(a.id))
      continue;
    AssetRowInfo row = buildAssetRow(cat, a, m_tags, m_typeOv, m_recycle, m_entityOv, assets.size() <= 200);
    if (unresolvedOnly && !row.unresolved)
      continue;
    m_rows.append(row);
  }
}

QSet<QString> DataListPanel::currentAssetSelection() const
{
  using namespace paleo::dataops;
  QSet<QString> out = selectedAssetIds(m_tree) + m_pageSelection;
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    out += selectedAssetIds(table);
  if (m_iconView)
  {
    for (QListWidgetItem *it : m_iconView->selectedItems())
    {
      const QString id = it->data(Qt::UserRole).toString();
      if (!id.isEmpty())
        out.insert(id);
    }
  }
  // 软删资产不在表里——选中集只含现存可见资产。
  return out;
}

QStringList DataListPanel::currentEntitySelection() const
{
  return paleo::dataops::selectedEntityIds(m_tree);
}

paleo::dataops::SelectionMix DataListPanel::currentSelectionMix() const
{
  paleo::dataops::SelectionMix mix;
  mix.assetIds = currentAssetSelection();
  mix.entityIds = currentEntitySelection();
  return mix;
}

void DataListPanel::refreshSelectionBadge()
{
  using namespace paleo::dataops;
  if (auto *badge = findChild<SelectionBadge *>(QStringLiteral("selectionBadge")))
    badge->setCount(int(currentAssetSelection().size()));
  emit selectionCountChanged(int(currentAssetSelection().size()),
                            currentEntitySelection().size());
}

void DataListPanel::refreshTagCloud()
{
  if (m_tagCloud)
    m_tagCloud->setCloud(m_tags.tagCloud(), m_activeTag);
}

void DataListPanel::updatePendingCounts()
{
  using namespace paleo::dataops;
  if (!m_quickBar)
    return;
  int unlinked = 0, unknown = 0, warned = 0;
  for (const AssetRowInfo &r : m_rows)
  {
    FilterCondition c;
    c.dim = FilterDim::Unlinked;
    if (c.matches(r)) ++unlinked;
    c.dim = FilterDim::UnknownType;
    if (c.matches(r)) ++unknown;
    c.dim = FilterDim::Warned;
    if (c.matches(r)) ++warned;
  }
  m_quickBar->setCounts(unlinked, unknown, warned);
}

void DataListPanel::refreshUndoButtons()
{
  if (m_undoBtn)
  {
    // D5.2：菜单项（按钮同面）显示操作名——「撤销 挂接 …」。
    m_undoBtn->setEnabled(m_opStack && m_opStack->canUndo());
    m_undoBtn->setText(m_opStack && m_opStack->canUndo()
                           ? tr("撤销 %1").arg(m_opStack->undoText())
                           : tr("撤销"));
    m_undoBtn->setToolTip(m_opStack && m_opStack->canUndo()
                              ? m_opStack->undoText() : tr("无可撤销操作"));
  }
  if (m_redoBtn)
  {
    m_redoBtn->setEnabled(m_opStack && m_opStack->canRedo());
    m_redoBtn->setText(m_opStack && m_opStack->canRedo()
                           ? tr("重做 %1").arg(m_opStack->redoText())
                           : tr("重做"));
    m_redoBtn->setToolTip(m_opStack && m_opStack->canRedo()
                              ? m_opStack->redoText() : tr("无可重做操作"));
  }
}

void DataListPanel::undoOp()
{
  if (!m_opStack || !m_opStack->canUndo())
    return;
  const QString text = m_opStack->undo();
  refreshAssetTable();
  emit entityRefreshRequested();
  // D5.4：撤销后状态栏确认反馈。
  emit statusMessage(tr("已撤销：%1").arg(text));
}

void DataListPanel::redoOp()
{
  if (!m_opStack || !m_opStack->canRedo())
    return;
  const QString text = m_opStack->redo();
  refreshAssetTable();
  emit entityRefreshRequested();
  emit statusMessage(tr("已重做：%1").arg(text));
}

void DataListPanel::pushCommand(paleo::dataops::DataOpCommand *cmd)
{
  if (!m_opStack || !cmd)
    return;
  m_opStack->push(cmd);
  refreshAssetTable();
  emit entityRefreshRequested();
}

void DataListPanel::syncLegacyControlsIntoFilter()
{
  using namespace paleo::dataops;
  // Search/Type 两维有双输入面：旧控件（assetSearchEdit/assetTypeFilter，
  // 兼容面）与 FilterBar/预设/状态串。reconcile 双向：
  //   1) FilterGroup 已有该维条件 → 写回旧控件（镜像一致）；
  //   2) 控件有值而条件缺 → 从控件带入条件。
  // 旧测试（直接打字/选类型）与新路径（FilterBar/预设/状态串）互不覆盖。
  auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
  auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
  const auto condValue = [this](FilterDim d) {
    for (const FilterCondition &c : m_filter.conditions)
      if (c.dim == d && !c.negate)
        return c.value;
    return QString();
  };
  if (search)
  {
    const QString inFilter = condValue(FilterDim::Search);
    if (!inFilter.isEmpty() && inFilter != search->text().trimmed())
    {
      const QSignalBlocker b(search);
      search->setText(inFilter);
    }
    else if (inFilter.isEmpty() && !search->text().trimmed().isEmpty())
    {
      m_filter.clearDim(FilterDim::Search);
      m_filter.conditions.prepend(
          {FilterDim::Search, search->text().trimmed(), false});
    }
    else if (inFilter.isEmpty() && search->text().trimmed().isEmpty())
      m_filter.clearDim(FilterDim::Search);
  }
  if (typeFilter)
  {
    const QString inFilter = condValue(FilterDim::Type);
    const QString combo = typeFilter->currentData().toString();
    if (!inFilter.isEmpty() && inFilter != combo)
    {
      const QSignalBlocker b(typeFilter);
      const int idx = typeFilter->findData(inFilter);
      typeFilter->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    else if (inFilter.isEmpty() && !combo.isEmpty())
    {
      m_filter.clearDim(FilterDim::Type);
      m_filter.conditions.prepend({FilterDim::Type, combo, false});
    }
    else if (inFilter.isEmpty() && combo.isEmpty())
      m_filter.clearDim(FilterDim::Type);
  }
}

void DataListPanel::applyFilterGroup(const paleo::dataops::FilterGroup &g)
{
  m_filter = g;
  if (m_filterBar)
    m_filterBar->setOrMode(g.orMode);
  applyListFilter();
  refreshChipBar();
}

void DataListPanel::setFilterFromStateString(const QString &s)
{
  applyFilterGroup(paleo::dataops::FilterGroup::fromStateString(s));
}

void DataListPanel::setTreeSort(paleo::dataops::TreeSortKind kind)
{
  m_treeSort = kind;
  paleo::dataops::rememberTreeSort(kind);
  refreshAssetTable();
}

void DataListPanel::selectAllVisibleAssets()
{
  using namespace paleo::dataops;
  if (m_rows.size() > 200) {
    m_pageSelection.clear();
    for (const auto &row : m_filteredRows) m_pageSelection.insert(row.assetId);
    m_selKeep.snapshotIds(m_pageSelection);
    renderAssetPage();
  }
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    selectAllVisible(table);
  // 树/图标视图同步全选（可见项）。
  if (m_iconView)
    m_iconView->selectAll();
}

void DataListPanel::invertAssetSelection()
{
  using namespace paleo::dataops;
  if (m_rows.size() > 200) {
    QSet<QString> inverted;
    for (const auto &row : m_filteredRows) if (!m_pageSelection.contains(row.assetId)) inverted.insert(row.assetId);
    m_pageSelection = inverted; renderAssetPage(); refreshSelectionBadge(); return;
  }
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable")))
    invertSelection(table);
}

void DataListPanel::selectByCurrentFilter()
{
  // 按过滤器选中 = 全选可见项（过滤已先行生效）。
  selectAllVisibleAssets();
  emit statusMessage(tr("已选中全部 %1 个可见资产").arg(currentAssetSelection().size()));
}

// ---- D1.4 批量挂接 ----------------------------------------------------------
void DataListPanel::batchAttachToEntity()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  EntityPickerDialog dlg(this);
  dlg.loadEntities(m_ctx.cat, m_entityOv, QString());
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString target = dlg.selectedEntityId();
  if (target.isEmpty())
    return;
  applyEntityDrop(QStringList(ids.values()), target);
}

// 拖放/批量挂接共用核心（D3.1/D3.4/D1.4）：按资产当前链接态分派
// 挂接（未决）/转移（已决换实体）/跳过（已挂到同一实体）。
void DataListPanel::applyEntityDrop(const QStringList &assetIds, const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  const CatalogEntity target = m_ctx.cat->entityById(entityId);
  int attached = 0, transferred = 0, skipped = 0;
  for (const QString &id : assetIds)
  {
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    if (links.isEmpty())
    {
      // 无链接资产：直接建链接挂到目标实体（addLink——不可撤销路径，
      // D5.5 语义由确认对话框承担；这里作为拖放主路径不弹额外确认，
      // 失败明细走状态栏）。
      EntityAssetLink l;
      l.entityType = target.entityType;
      l.entityId = entityId;
      l.assetId = id;
      l.role = QStringLiteral("reference");
      QString err;
      if (!m_ctx.cat->addLink(l, &err))
      {
        emit statusMessage(tr("挂接失败：%1").arg(err));
        continue;
      }
      ++attached;
      continue;
    }
    bool acted = false;
    for (const EntityAssetLink &l : links)
    {
      if (l.unresolved)
      {
        pushCommand(new AttachLinkCmd(m_ctx, id, l.role, entityId, target.entityType));
        ++attached;
        acted = true;
        break; // 每资产处理第一条未决链接（与既有行内控件口径一致）
      }
    }
    if (acted)
      continue;
    for (const EntityAssetLink &l : links)
    {
      if (!l.unresolved && l.entityId != entityId)
      {
        pushCommand(new TransferLinkCmd(m_ctx, id, l.role, l.entityId, entityId));
        ++transferred;
        acted = true;
        break;
      }
    }
    if (!acted)
      ++skipped; // 已挂到目标实体的资产
  }
  if (m_history)
    m_history->push(tr("批量挂接 %1（挂 %2 / 转移 %3 / 跳过 %4）")
                        .arg(entityId).arg(attached).arg(transferred).arg(skipped));
  // D3.1 确认 toast（非模态反馈 + 可撤销提示）。
  emit statusMessage(tr("挂接到「%1」：新挂 %2、转移 %3、跳过 %4（可撤销）")
                         .arg(target.name.isEmpty() ? entityId : target.name)
                         .arg(attached).arg(transferred).arg(skipped));
}

// ---- D1.5 批量改类型 ----------------------------------------------------------
void DataListPanel::batchChangeType()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  // 类型词表 = 库内类型 ∪ 分类器常用类型。
  QStringList vocab;
  for (const AssetRowInfo &r : m_rows)
    if (!vocab.contains(r.effectiveType) && !r.effectiveType.isEmpty())
      vocab << r.effectiveType;
  vocab << QStringLiteral("well_log") << QStringLiteral("well_head")
        << QStringLiteral("tops") << QStringLiteral("time_depth")
        << QStringLiteral("seismic") << QStringLiteral("horizon")
        << QStringLiteral("boundary") << QStringLiteral("auxiliary");
  vocab.removeDuplicates();
  vocab.sort();
  QStringList names;
  for (const QString &id : ids)
  {
    const CatalogAsset a = m_ctx.cat->assetById(id);
    names << a.displayName;
  }
  BatchTypeDialog dlg(names, vocab, this);
  dlg.setNote(tr("改型写入视图层改写表（可撤销，可清除回原型）；catalog 资产"
                 "记录的类型字段不动。"));
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString newType = dlg.chosenType();
  if (newType.isEmpty())
    return;
  QStringList failures;
  for (const QString &id : ids)
  {
    const QString prev = m_typeOv.overriddenType(id);
    if (!m_typeOv.setType(id, newType))
    {
      // 幂等跳过不算失败。
      continue;
    }
    if (!m_typeOv.save())
      failures << tr("%1：sidecar 写入失败").arg(id);
    else
      pushCommand(new TypeOverrideCmd(m_ctx, id, newType, prev));
  }
  if (m_history)
    m_history->push(tr("批量改类型 → %1（%2 项）").arg(newType).arg(ids.size()));
  if (!failures.isEmpty())
    showBatchFailureDetail(this, tr("批量改类型"), failures);
  else
    emit statusMessage(tr("已把 %1 个资产类型改为 %2（可撤销）").arg(ids.size()).arg(newType));
}

// ---- D1.6 批量移除（软删）+ 可回收清单 ----------------------------------------
void DataListPanel::batchRemoveSoft()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  // D5.5：软删可撤销——确认说明这一点（不是破坏性删除）。
  if (QMessageBox::question(this, tr("移除资产"),
                            tr("把 %1 个资产移入可回收清单？\n"
                               "（软删：可从「可回收清单」恢复，可撤销；"
                               "catalog 记录保留）").arg(ids.size())) !=
      QMessageBox::Yes)
    return;
  // 方向 30：单命令批量软删（一次落盘、整组可撤销）——逐项命令会随 N 放大
  // recycle_bin.json 重写次数（tst_ui_blocking 探针的非空转门槛）。
  QVector<std::pair<QString, RecycleEntry>> entries;
  for (const QString &id : ids)
  {
    const CatalogAsset a = m_ctx.cat->assetById(id);
    RecycleEntry e;
    e.assetId = id;
    e.type = a.type;
    e.displayName = a.displayName;
    entries.append({id, e});
  }
  const QStringList idList(ids.constBegin(), ids.constEnd());
  pushCommand(new BatchRemoveCmd(m_ctx, idList, entries, tr("批量移除")));
  if (m_history)
    m_history->push(tr("批量移除 %1 项（软删）").arg(ids.size()));
  emit statusMessage(tr("已移除 %1 个资产到可回收清单（可撤销）").arg(ids.size()));
}

void DataListPanel::showRecycleBin()
{
  using namespace paleo::dataops;
  if (!m_ctx.valid())
    return;
  DataCatalog *cat = m_ctx.cat;
  const QString pd = projectDirFor(cat);

  // 方向 30：软删资产的受管字节占用（assetId → bytes；外链源不计）。
  const auto entrySizes = [&]() {
    QHash<QString, qint64> sizes;
    for (const RecycleEntry &e : m_recycle.entries())
    {
      qint64 t = 0;
      for (const CatalogVersion &v : cat->versionsForAsset(e.assetId))
      {
        if (!v.managed)
          continue;
        const QString abs = DataCatalog::resolvedVersionPath(pd, v);
        if (abs.isEmpty())
          continue;
        const QFileInfo fi(abs);
        if (fi.exists())
          t += fi.size();
      }
      sizes.insert(e.assetId, t);
    }
    return sizes;
  };

  RecycleBinDialog dlg(this);
  auto reload = [&]() {
    dlg.setEntrySizes(entrySizes());
    dlg.loadEntries(m_recycle.entries());
  };
  reload();
  connect(&dlg, &RecycleBinDialog::restoreRequested, this,
          [&](const QStringList &assetIds) {
            if (assetIds.isEmpty())
              return;
            // 方向 30：单命令批量恢复（一次落盘、整组可撤销）。
            QVector<std::pair<QString, RecycleEntry>> entries;
            for (const QString &id : assetIds)
              for (const RecycleEntry &x : m_recycle.entries())
                if (x.assetId == id)
                  entries.append({id, x});
            pushCommand(new BatchRestoreCmd(m_ctx, assetIds, entries));
            reload();
            emit statusMessage(tr("已恢复 %1 个资产（可整组撤销）").arg(assetIds.size()));
          });
  connect(&dlg, &RecycleBinDialog::restoreAllRequested, this, [&] {
    QStringList ids;
    QVector<std::pair<QString, RecycleEntry>> entries;
    for (const RecycleEntry &e : m_recycle.entries())
    {
      ids << e.assetId;
      entries.append({e.assetId, e});
    }
    if (ids.isEmpty())
      return;
    pushCommand(new BatchRestoreCmd(m_ctx, ids, entries));
    reload();
    emit statusMessage(tr("已全部恢复（%1 项，可整组撤销）").arg(ids.size()));
  });
  connect(&dlg, &RecycleBinDialog::purgeAllRequested, this, [&] {
    m_recycle.clearAll();
    m_recycle.save();
    reload();
    refreshAssetTable();
    emit statusMessage(tr("可回收清单已清空（不可撤销）"));
  });
  // 方向 30：物理删除（catalog + 磁盘双清；二次确认在对话框内）。
  connect(&dlg, &RecycleBinDialog::purgeRequested, this, [&](const QStringList &ids) {
    if (ids.isEmpty() || !cat->isOpen())
      return;
    const paleo::assetops::PurgeOutcome out =
        paleo::assetops::purgeAssets(cat, pd, ids);
    for (const QString &id : ids)
      m_recycle.purge(id); // 清 sidecar 条目（不可恢复路径同形）
    m_recycle.save();
    reload();
    refreshAssetTable();
    emit entityRefreshRequested();
    QString msg = tr("物理删除完成：%1 项，释放 %2 字节。")
                      .arg(out.purgedAssetIds.size())
                      .arg(out.bytesFreed);
    if (!out.failedAssets.isEmpty() || !out.leftoverFiles.isEmpty())
    {
      QStringList lines{msg};
      for (const QString &f : out.failedAssets)
        lines << tr("· 被拒：%1").arg(f);
      for (const QString &f : out.leftoverFiles)
        lines << tr("· 残留文件（请手动清理）：%1").arg(f);
      QMessageBox::warning(&dlg, tr("物理删除（部分未完成）"), lines.join(QLatin1Char('\n')));
    }
    else
      emit statusMessage(msg);
  });
  dlg.exec();
}

// ---- 方向 30：导入台账查看器 -------------------------------------------------
void DataListPanel::showImportLedger()
{
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
  {
    emit statusMessage(tr("工程未打开，暂无导入台账"));
    return;
  }
  paleo::imports::ImportLedger ledger;
  ledger.load(cat);
  paleo::dataops::ImportLedgerDialog dlg(this);
  dlg.setBatches(ledger.batches());
  dlg.exec();
}

// ---- 方向 30：未决链接批量归位 -------------------------------------------------
void DataListPanel::resolvePendingLinks()
{
  using namespace paleo::dataops;
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
    return;
  PendingLinkDialog dlg(this);
  const int total = cat->unresolvedLinks().size();
  dlg.setProposals(paleo::assetops::proposablePendingLinks(cat), total);
  connect(&dlg, &PendingLinkDialog::applyRequested, this,
          [&](const QVector<int> &linkIndexes) {
            QString err;
            const int n = paleo::assetops::applyPendingResolutions(cat, linkIndexes, &err);
            dlg.setApplied(n, linkIndexes.size());
            refreshAssetTable();
            emit entityRefreshRequested();
            if (n > 0)
              emit statusMessage(tr("已归位 %1 条未决链接（单事务落盘）").arg(n));
            if (!err.isEmpty())
              QMessageBox::warning(&dlg, tr("归位失败"), err);
          });
  dlg.exec();
}

// ---- 方向 30：资产版本面（对比 + 回滚）---------------------------------------
QVector<paleo::dataops::VersionRow>
DataListPanel::versionRowsForAsset(const QString &assetId) const
{
  using namespace paleo::dataops;
  QVector<VersionRow> rows;
  DataCatalog *cat = m_ctx.cat;
  if (!cat)
    return rows;
  const QString pd = projectDirFor(cat);
  const CatalogVersion cur = cat->currentVersion(assetId);
  for (const CatalogVersion &v : cat->versionsForAsset(assetId))
  {
    VersionRow r;
    r.versionId = v.id;
    r.versionNumber = v.versionNumber;
    r.stage = v.stage;
    r.managed = v.managed;
    r.fileName = v.fileName;
    r.sha256 = v.sha256;
    r.sourceUri = v.sourceUri;
    r.isCurrent = (v.id == cur.id);
    const QString abs = v.managed ? DataCatalog::resolvedVersionPath(pd, v) : v.path;
    if (!abs.isEmpty())
    {
      const QFileInfo fi(abs);
      if (fi.exists())
        r.sizeBytes = fi.size();
    }
    rows.append(r);
  }
  return rows;
}

void DataListPanel::showVersionTable()
{
  using namespace paleo::dataops;
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
    return;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.size() != 1)
  {
    emit statusMessage(tr("版本面对单资产——请只选一个资产"));
    return;
  }
  const QString assetId = ids.values().first();
  const CatalogAsset a = cat->assetById(assetId);
  if (a.id.isEmpty())
    return;
  const QString pd = projectDirFor(cat);

  VersionTableDialog dlg(this);
  dlg.setAssetTitle(a.displayName);
  dlg.setRows(versionRowsForAsset(assetId));
  dlg.setCompareHook([cat, pd](const QString &va, const QString &vb) {
    return paleo::assetops::compareVersions(pd, cat->versionById(va), cat->versionById(vb));
  });
  connect(&dlg, &VersionTableDialog::rollbackRequested, this,
          [&](const QString &versionId) {
            QString err;
            const paleo::assetops::RollbackOutcome out =
                paleo::assetops::rollbackToVersion(cat, pd, assetId, versionId, &err);
            if (out.versionId.isEmpty())
            {
              QMessageBox::warning(&dlg, tr("回滚失败"), err);
              return;
            }
            dlg.setRowsRefreshed(
                versionRowsForAsset(assetId),
                tr("已回滚：新增 v%1（内容与所选版本一致，历史全保留）。")
                    .arg(out.versionNumber));
            refreshAssetTable();
            emit entityRefreshRequested();
            emit statusMessage(tr("「%1」已回滚到所选版本内容（新版本 v%2）")
                                   .arg(a.displayName)
                                   .arg(out.versionNumber));
          });
  dlg.exec();
}

// ---- 方向 30：资产体检 ---------------------------------------------------------
void DataListPanel::refreshHealthReportInDialog()
{
  if (!m_healthDlg)
    return;
  m_healthDlg->setReport(m_healthBase, m_healthRecycleCount, m_healthRecycleBytes);
}

void DataListPanel::showStorageGovernance()
{
  if (!m_ctx.cat || !m_ctx.cat->isOpen()) { emit statusMessage(tr("工程未打开，无法扫描存储")); return; }
  if (m_storageDialog) { m_storageDialog->raise(); m_storageDialog->activateWindow(); return; }
  auto *dialog = new StorageGovernanceDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  m_storageDialog = dialog;
  auto *controller = m_storageController;
  connect(dialog, &StorageGovernanceDialog::scanRequested, controller, &StorageGovernanceController::scan);
  connect(dialog, &StorageGovernanceDialog::cancelRequested, controller, &StorageGovernanceController::cancel);
  connect(dialog, &QDialog::finished, controller, &StorageGovernanceController::cancel);
  connect(dialog, &StorageGovernanceDialog::previewRequested, controller, &StorageGovernanceController::requestPreview);
  connect(dialog, &StorageGovernanceDialog::confirmRequested, controller, &StorageGovernanceController::confirmPreview);
  connect(dialog, &StorageGovernanceDialog::verifyShaRequested, controller, &StorageGovernanceController::verifySha);
  connect(controller, &StorageGovernanceController::reportReady, dialog, &StorageGovernanceDialog::setReport);
  connect(controller, &StorageGovernanceController::previewReady, dialog, &StorageGovernanceDialog::setPreview);
  connect(controller, &StorageGovernanceController::stateChanged, dialog, &StorageGovernanceDialog::setBusy);
  connect(controller, &StorageGovernanceController::progress, dialog, &StorageGovernanceDialog::setProgress);
  connect(controller, &StorageGovernanceController::message, dialog, &StorageGovernanceDialog::setMessage);
  connect(controller, &StorageGovernanceController::shaReady, dialog,
    [dialog](const auto &issues, bool complete) {
      dialog->setMessage(complete ? tr("外链 SHA 复验完成：%1 个不一致版本。").arg(issues.size())
                                 : tr("外链 SHA 复验未完成；结果仅覆盖已扫部分。"));
    });
  connect(controller, &StorageGovernanceController::cleanupFinished, dialog, [this, dialog](const auto &outcome) {
    dialog->setMessage(tr("回收完成：实际释放 %1 B；残留文件 %2 个。请重新扫描。%3")
      .arg(outcome.bytesFreed).arg(outcome.leftoverFiles.size()).arg(outcome.leftoverFiles.join(QStringLiteral("\n"))));
    refreshAssetTable();
  });
  dialog->setBusy(controller->busy(), controller->cancellable());
  dialog->open();
  controller->scan();
}

void DataListPanel::showHealthCheck()
{
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen()) { emit statusMessage(tr("工程未打开，无法体检")); return; }
  CatalogHealthDialog dlg(this);
  StorageGovernanceController controller;
  controller.setCatalog(cat);
  m_healthDlg = &dlg;
  const auto quickScan = [&] {
    QStringList ids;
    for (const auto &entry : m_recycle.entries()) ids << entry.assetId;
    m_healthRecycleCount = ids.size();
    dlg.setShaState(tr("体检中；目录检查在后台执行，SHA 尚未复验。"));
    controller.healthScan(ids);
  };
  connect(&dlg, &CatalogHealthDialog::refreshRequested, &controller, quickScan);
  connect(&dlg, &CatalogHealthDialog::verifyShaRequested, &controller, &StorageGovernanceController::verifySha);
  connect(&dlg, &CatalogHealthDialog::cancelVerifyRequested, &controller, &StorageGovernanceController::cancel);
  connect(&controller, &StorageGovernanceController::stateChanged, &dlg, [&dlg](bool busy, bool) { dlg.setVerifyRunning(busy); });
  connect(&controller, &StorageGovernanceController::progress, &dlg, [&dlg](int n, int total, const QString &path) {
    dlg.setShaState(tr("后台检查 %1/%2：%3").arg(n).arg(total).arg(path));
  });
  connect(&controller, &StorageGovernanceController::healthReady, &dlg, [&](const auto &report, qint64 bytes, bool complete) {
    m_healthBase = report; m_healthRecycleBytes = bytes;
    refreshHealthReportInDialog();
    dlg.setShaState(complete ? tr("目录体检完成；外链 SHA 尚未复验。") : tr("体检未完成，结果仅覆盖已扫部分。"));
  });
  connect(&controller, &StorageGovernanceController::shaReady, &dlg, [&](const auto &issues, bool complete) {
    m_healthBase.issues.erase(std::remove_if(m_healthBase.issues.begin(), m_healthBase.issues.end(),
      [](const auto &issue) { return issue.kind == paleo::health::IssueKind::ShaMismatch; }), m_healthBase.issues.end());
    m_healthBase.issues += issues; m_healthBase.shaVerifyComplete = complete;
    refreshHealthReportInDialog();
    dlg.setShaState(complete ? tr("外链 SHA 复验完成。") : tr("外链 SHA 复验已取消，未扫完。"));
  });
  connect(&dlg, &CatalogHealthDialog::jumpToVersion, this, [&](const QString &versionId) {
    dlg.accept();
    emit versionActivated(versionId); // #199 体检 stale 版本定位——分支重写时补回
  });
  connect(&dlg, &CatalogHealthDialog::jumpToAsset, this, [&](const QString &id) { dlg.accept(); emit assetActivated(id); });
  connect(&dlg, &CatalogHealthDialog::jumpToEntity, this, [&](const QString &id) { dlg.accept(); emit entitiesFocusRequested({id}); });
  quickScan(); dlg.exec(); controller.cancel(); m_healthDlg = nullptr;
}

// ---- D1.7 导出清单 -------------------------------------------------------------
void DataListPanel::batchExportManifest()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  QVector<AssetRowInfo> rows;
  for (const AssetRowInfo &r : m_rows)
    if (ids.isEmpty() || ids.contains(r.assetId))
      rows.append(r);
  if (rows.isEmpty())
    return;
  const QString path = exportRowsToFile(this, rows);
  if (!path.isEmpty())
    emit statusMessage(tr("清单已导出：%1").arg(path));
}

// ---- D1.8 批量打开预览 ---------------------------------------------------------
void DataListPanel::batchOpenPreview()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty())
    return;
  const auto plan = batchOpenPreviewPlan(QStringList(ids.values()));
  for (const QString &id : plan.first)
    emit assetActivated(id);
  if (plan.second > 0)
    emit statusMessage(tr("已打开前 %1 项预览；其余 %2 项未打开（防标签爆炸）")
                           .arg(plan.first.size()).arg(plan.second));
}

// ---- D2.4 选中打标签 ------------------------------------------------------------
void DataListPanel::batchAddTag()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  bool ok = false;
  const QString tag = QInputDialog::getText(this, tr("打标签"),
                                            tr("标签名（选中 %1 个资产）:")
                                                .arg(ids.size()),
                                            QLineEdit::Normal, QString(), &ok);
  if (!ok)
    return;
  int n = 0;
  for (const QString &id : ids)
    if (m_tags.addTag(id, tag))
    {
      m_tags.save();
      pushCommand(new TagCmd(m_ctx, id, tag, true));
      ++n;
    }
  if (n > 0 && m_history)
    m_history->push(tr("打标签「%1」（%2 项）").arg(tag).arg(n));
  emit statusMessage(n > 0 ? tr("已给 %1 个资产打标签「%2」").arg(n).arg(tag)
                           : tr("标签未变化（已存在或为空）"));
}

// ---- 单资产链接操作 --------------------------------------------------------------
void DataListPanel::detachSingleAssetLink()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  for (const QString &id : ids)
    for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(id))
      if (!l.unresolved)
      {
        pushCommand(new DetachLinkCmd(m_ctx, id, l.role, l.entityId));
        emit statusMessage(tr("已解挂 %1→%2（可撤销）").arg(id, l.entityId));
        return; // 单操作语义：首个选中资产的第一条已决链接
      }
  emit statusMessage(tr("选中资产没有已决关联"));
}

void DataListPanel::setPrimaryForSelection()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  for (const QString &id : ids)
  {
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    for (const EntityAssetLink &l : links)
      if (!l.unresolved && !l.isPrimary)
      {
        // 前主关联（撤销时恢复）。
        QString prevPrimary;
        for (const EntityAssetLink &o : links)
          if (!o.unresolved && o.isPrimary && o.entityId == l.entityId && o.role == l.role)
            prevPrimary = o.assetId;
        pushCommand(new SetPrimaryCmd(m_ctx, id, l.role, l.entityId, prevPrimary));
        emit statusMessage(tr("已把 %1 设为主关联（可撤销）").arg(id));
        return;
      }
  }
  emit statusMessage(tr("选中资产没有可提升的非主关联"));
}

void DataListPanel::editRoleForSelection()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid() || ids.size() != 1)
    return;
  const QString assetId = *ids.constBegin();
  QString role, entityId;
  for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(assetId))
    if (!l.unresolved)
    {
      role = l.role;
      entityId = l.entityId;
      break;
    }
  if (role.isEmpty())
  {
    emit statusMessage(tr("该资产没有已决关联可改角色"));
    return;
  }
  // 词表 = roleRegistry 按实体类型的合法角色（工程可扩词表如实进下拉）。
  QStringList vocab;
  QString linkEntityType;
  for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(assetId))
    if (!l.unresolved)
    {
      linkEntityType = l.entityType;
      break;
    }
  for (const RoleDef &d : m_ctx.cat->roleRegistry().forEntity(linkEntityType))
    vocab << d.role;
  if (!vocab.contains(role))
    vocab << role;
  vocab.sort();
  RoleEditDialog dlg(this);
  dlg.loadRole(role, vocab);
  if (dlg.exec() != QDialog::Accepted || dlg.newRole() == role)
    return;
  // D4.7 + D5.5：addLink 无删除对偶 → 不可撤销，确认说明。
  if (QMessageBox::warning(
          this, tr("角色变更（不可撤销）"),
          tr("将为「%1」新增角色关联 %2（原 %3 关联保留）。\n"
             "此操作不可撤销。继续？")
              .arg(assetId, dlg.newRole(), role),
          QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
    return;
  EntityAssetLink l;
  l.entityType = m_ctx.cat->entityById(entityId).entityType;
  l.entityId = entityId;
  l.assetId = assetId;
  l.role = dlg.newRole();
  l.isPrimary = false;
  QString err;
  if (m_ctx.cat->addLink(l, &err))
  {
    if (m_history)
      m_history->push(tr("角色变更 %1：%2→%3").arg(assetId, role, dlg.newRole()));
    refreshAssetTable();
    emit entityRefreshRequested();
    emit statusMessage(tr("已新增角色关联 %1（原关联保留，不可撤销）")
                           .arg(dlg.newRole()));
  }
  else
    emit statusMessage(tr("角色变更失败：%1").arg(err));
}

// ---- D3.2 外部文件拖入 → 导入流 ---------------------------------------------------
void DataListPanel::handleExternalFiles(const QStringList &paths)
{
  using namespace paleo::dataops;
  if (paths.isEmpty())
    return;
  // D8.6：目录/大量文件 → 预估 + 分批确认；单文件直接走壳导入流。
  QString dir;
  int fileCount = 0;
  for (const QString &p : paths)
  {
    const QFileInfo fi(p);
    if (fi.isDir())
      dir = p;
    else if (fi.isFile())
      ++fileCount;
  }
  if (!dir.isEmpty() || paths.size() > 20)
  {
    ImportEstimate estimate;
    if (!dir.isEmpty())
      estimate = estimateDirectory(dir);
    else
    {
      estimate.fileCount = fileCount;
      for (const QString &p : paths)
        if (QFileInfo(p).isFile())
          estimate.totalBytes += QFileInfo(p).size();
    }
    ImportEstimateDialog dlg(this);
    dlg.setEstimate(estimate);
    if (dlg.exec() != QDialog::Accepted)
      return;
    emit statusMessage(dlg.batchChosen()
                           ? tr("分批导入已确认（首批 %1 项）").arg(batchPaths(paths, 200).size())
                           : tr("整批导入已确认（%1 项）").arg(paths.size()));
  }
  // 视图只发意图：实际导入由壳的 FolderImport 流执行（T22 确认表复用）。
  emit externalImportRequested(paths);
  // 导入队列（D8.1）：未注入 runner 时仅登记（生产接线见 docs/dataops/GAPS.md）。
  if (m_importQueue)
  {
    QHash<QString, QString> typeByExt;
    for (const ImportPreset &p : importPresets())
      typeByExt = p.typeMap; // 预设的类型映射（最后一个命中的预设）
    m_importQueue->enqueuePaths(paths, typeByExt);
  }
}

// ---- D7 视图形态 ------------------------------------------------------------------
void DataListPanel::setViewMode(int mode)
{
  if (m_viewStack && mode >= 0 && mode < m_viewStack->count())
    m_viewStack->setCurrentIndex(mode);
}

void DataListPanel::openColumnConfig()
{
  using namespace paleo::dataops;
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  QStringList labels;
  QList<bool> visible;
  for (int c = 0; c < table->columnCount(); ++c)
  {
    const int logical = table->horizontalHeader()->logicalIndex(c);
    labels << (table->horizontalHeaderItem(logical)
                   ? table->horizontalHeaderItem(logical)->text()
                   : QString::number(logical));
    visible << !table->isColumnHidden(logical);
  }
  ColumnConfigDialog dlg(labels, visible, this);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QList<QPair<int, bool>> result = dlg.result();
  for (int visual = 0; visual < result.size(); ++visual)
  {
    const int logical = result.at(visual).first;
    table->setColumnHidden(logical, !result.at(visual).second);
    const int curVisual = table->horizontalHeader()->visualIndex(logical);
    if (curVisual != visual)
      table->horizontalHeader()->moveSection(curVisual, visual);
  }
  saveColumnState(QStringLiteral("assetTable"), table->horizontalHeader());
}

void DataListPanel::openGroupConfig()
{
  using namespace paleo::dataops;
  if (!m_groupTree)
    return;
  QStringList dims = {tr("按类型"), tr("按实体"), tr("按标签"), tr("按版本")};
  bool ok = false;
  const QString chosen = QInputDialog::getItem(
      this, tr("分组维度"), tr("按什么分组:"), dims, m_groupMode, false, &ok);
  if (!ok)
    return;
  m_groupMode = dims.indexOf(chosen);
  if (m_groupMode < 0)
    m_groupMode = 0;
  m_groupTree->setGroupBy(AssetGroupTree::GroupBy(m_groupMode));
  setViewMode(4);
  // 重灌当前可见行。
  QSet<QString> visible;
  for (const AssetRowInfo &r : m_rows)
    if (!r.removed)
      visible.insert(r.assetId);
  QVector<AssetRowInfo> shown;
  for (const AssetRowInfo &r : m_rows)
    if (visible.contains(r.assetId))
      shown.append(r);
  m_groupTree->loadRows(m_rows.size() > 200 ? m_pageRows : shown);
}

// ---- D1.3 上下文菜单 --------------------------------------------------------------
QTreeWidgetItem *DataListPanel::treeItemForAsset(const QString &assetId) const
{
  if (!m_tree)
    return nullptr;
  QTreeWidgetItemIterator it(m_tree);
  while (*it)
  {
    if ((*it)->data(0, Qt::UserRole).toString() == assetId)
      return *it;
    ++it;
  }
  return nullptr;
}

void DataListPanel::showAssetContextMenu(QObject *source, const QPoint &pos)
{
  using namespace paleo::dataops;
  // 选择集（右键不改变选择——Qt 菜单口径：右键在选中项上保持多选）。
  const SelectionMix mix = currentSelectionMix();
  ContextMenuSpec spec;
  spec.hasAssets = !mix.assetIds.isEmpty();
  spec.hasEntities = !mix.entityIds.isEmpty();
  spec.assetCount = int(mix.assetIds.size());
  spec.entityCount = mix.entityIds.size();
  if (spec.assetCount == 1)
  {
    const QString id = *mix.assetIds.constBegin();
    for (const AssetRowInfo &r : m_rows)
      if (r.assetId == id)
      {
        spec.singleAssetUnresolved = r.unresolved;
        spec.singleAssetResolved = !r.entityNames.isEmpty();
        spec.singleAssetIsHorizon = r.effectiveType == QLatin1String("horizon");
        spec.singleAssetIsTops = r.effectiveType == QLatin1String("well_stratification");
        break;
      }
  }
  spec.hasRecycleEntries = !m_recycle.entries().isEmpty();
  const QStringList keys = contextMenuActions(spec);
  if (keys.isEmpty())
    return;
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("dataContextMenu"));
  const struct
  {
    const char *key;
    const char *text;
  } kTitles[] = {
    {"openPreview", QT_TR_NOOP("打开预览")},
    {"showVersions", QT_TR_NOOP("版本与回滚…")},
    {"gridHorizon", QT_TR_NOOP("网格化…")},
    {"editTops", QT_TR_NOOP("编辑分层…")},
    {"openPreviewAll", QT_TR_NOOP("批量打开预览（前 8 项）")},
    {"attachToEntity", QT_TR_NOOP("挂接到实体…")},
    {"detachLink", QT_TR_NOOP("解除挂接")},
    {"setPrimary", QT_TR_NOOP("设为主关联")},
    {"editRole", QT_TR_NOOP("编辑挂接角色…")},
    {"transferLink", QT_TR_NOOP("转移到其它实体…")},
    {"addTag", QT_TR_NOOP("打标签…")},
    {"changeType", QT_TR_NOOP("改类型…")},
    {"exportManifest", QT_TR_NOOP("导出清单 CSV/JSON…")},
    {"removeSoft", QT_TR_NOOP("移除（进可回收清单）")},
    {"showInFolder", QT_TR_NOOP("在文件管理器中显示")},
    {"renameEntity", QT_TR_NOOP("重命名实体…")},
    {"editCoords", QT_TR_NOOP("编辑坐标/备注…")},
    {"deleteEntity", QT_TR_NOOP("删除实体…")},
    {"focusEntities", QT_TR_NOOP("定位选中实体")},
    {"selectAll", QT_TR_NOOP("全选可见项")},
    {"invertSelection", QT_TR_NOOP("反选")},
    {"showRecycleBin", QT_TR_NOOP("可回收清单…")},
  };
  for (const QString &key : keys)
  {
    const char *text = nullptr;
    for (const auto &t : kTitles)
      if (key == QLatin1String(t.key))
      {
        text = t.text;
        break;
      }
    if (!text)
      continue;
    QAction *act = menu.addAction(tr(text));
    act->setData(key);
  }
  QWidget *src = qobject_cast<QWidget *>(source);
  const QPoint global = src ? src->mapToGlobal(pos) : QCursor::pos();
  QAction *picked = menu.exec(global);
  if (!picked)
    return;
  const QString key = picked->data().toString();
  if (key == QLatin1String("openPreview") || key == QLatin1String("openPreviewAll"))
    batchOpenPreview();
  else if (key == QLatin1String("showVersions"))
    showVersionTable();
  else if (key == QLatin1String("gridHorizon"))
    emit gridHorizonRequested(*mix.assetIds.constBegin()); // 意图信号回壳（视图不干活）
  else if (key == QLatin1String("editTops"))
    emit topsEditRequested(*mix.assetIds.constBegin()); // 意图信号回壳（视图不干活）
  else if (key == QLatin1String("attachToEntity"))
    batchAttachToEntity();
  else if (key == QLatin1String("detachLink"))
    detachSingleAssetLink();
  else if (key == QLatin1String("setPrimary"))
    setPrimaryForSelection();
  else if (key == QLatin1String("editRole"))
    editRoleForSelection();
  else if (key == QLatin1String("transferLink"))
    batchAttachToEntity(); // 转移 = 选目标实体（与挂接同一对话框）
  else if (key == QLatin1String("addTag"))
    batchAddTag();
  else if (key == QLatin1String("changeType"))
    batchChangeType();
  else if (key == QLatin1String("exportManifest"))
    batchExportManifest();
  else if (key == QLatin1String("removeSoft"))
    batchRemoveSoft();
  else if (key == QLatin1String("showInFolder"))
  {
    const QSet<QString> ids = currentAssetSelection();
    if (!ids.isEmpty() && m_doc)
    {
      const CatalogVersion v = m_doc->catalog()->currentVersion(*ids.constBegin());
      const QString abs = m_doc->absolutePathForVersion(v);
      if (!abs.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(abs).absolutePath()));
    }
  }
  else if (key == QLatin1String("renameEntity"))
    emit entityRenameRequested(mix.entityIds.isEmpty() ? QString() : mix.entityIds.front());
  else if (key == QLatin1String("editCoords") || key == QLatin1String("deleteEntity"))
    emit entityDeleteRequested(mix.entityIds.isEmpty() ? QString() : mix.entityIds.front());
  else if (key == QLatin1String("focusEntities"))
    emit entitiesFocusRequested(mix.entityIds);
  else if (key == QLatin1String("selectAll"))
    selectAllVisibleAssets();
  else if (key == QLatin1String("invertSelection"))
    invertAssetSelection();
  else if (key == QLatin1String("showRecycleBin"))
    showRecycleBin();
}

// ---- D2 过滤：树面 ---------------------------------------------------------------
void DataListPanel::applyFilterToTree(const QSet<QString> &visibleIds, bool filtering)
{
  if (!m_tree)
    return;
  const QString needle = [this]() {
    const auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
    return search ? search->text().trimmed() : QString();
  }();
  // 递归判定：分类/标签组随子命中；资产叶按可见 id 集；测区节点按文本；
  // 井/标签叶按自身文本或子命中。
  std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *node) -> bool {
    const QString nodeType = node->data(0, Qt::UserRole + 2).toString();
    const QString id = node->data(0, Qt::UserRole).toString();
    const bool selfMatch =
        needle.isEmpty() || node->text(0).contains(needle, Qt::CaseInsensitive) ||
        node->text(1).contains(needle, Qt::CaseInsensitive);
    if (nodeType == QLatin1String("category") || nodeType == QLatin1String("tag_group"))
    {
      bool any = false;
      for (int k = 0; k < node->childCount(); ++k)
        if (visit(node->child(k)))
          any = true;
      node->setHidden(!any);
      return any;
    }
    if (!id.isEmpty() && nodeType != QLatin1String("well"))
    {
      // 资产叶 / 测线 / 测区：资产在可见集；测区/测线随文本。
      const bool ok = selfMatch &&
                      (nodeType == QLatin1String("survey_area") ||
                       visibleIds.contains(id));
      node->setHidden(!ok);
      return ok;
    }
    // 井节点 / 标签叶 / 其它无 id 结构节点。
    bool any = selfMatch;
    for (int k = 0; k < node->childCount(); ++k)
      if (visit(node->child(k)))
        any = true;
    node->setHidden(!any);
    return any;
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
  {
    QTreeWidgetItem *cat = m_tree->topLevelItem(i);
    const bool catVisible = visit(cat);
    cat->setHidden(!catVisible);
    if (filtering && catVisible)
      cat->setExpanded(true);
    else if (!filtering)
    {
      const bool isWellCategory = cat->text(0).startsWith(tr("井 ("));
      cat->setExpanded(!isWellCategory);
      for (int j = 0; j < cat->childCount(); ++j)
      {
        auto *child = cat->child(j);
        child->setExpanded(child->text(0).contains(QStringLiteral("综合柱状图")));
      }
    }
  }
}

// AUTOMOC：Q_OBJECT 头（非同名 basename）的 moc 显式编入本 TU——
// CMake AUTOMOC 只对同名 basename 头自动 moc（datalist.cpp→datalist.h），
// dataops 家族的部件头经此 include 挂进 paleo_ui（详见 docs/dataops/GAPS.md
// 「构建接线」节）。__has_include 守卫：lint 门（tools/check_tidy.py）只
// configure 不 build，moc 尚未生成时跳过；AUTOMOC 生成 moc 靠文本扫描，
// 不受预处理条件影响，真实构建照常编入。
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
