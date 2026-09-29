// 层：视图
#include "datalist.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../paleoicons.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include "../../domain/importrows.h"
#include <QButtonGroup>
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
    it->setForeground(PaleoTheme::tokens().textMuted); // text-muted（现取随主题）
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
    it->setForeground(PaleoTheme::tokens().textMuted); // text-muted（现取随主题）
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
    // catalog 在 <projectDir>/artifacts/metadata/ → 退两级到工程目录。
    const QString projectDir = QFileInfo(QFileInfo(cp).dir().absolutePath())
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
}

DataListPanel::DataListPanel(QWidget *parent)
  : QWidget(parent)
{
  setMinimumWidth(0);
  auto *lay = panelLayout(this);

  // 三段各自成件（objectName 见头文件），壳按 ribbon 布局重新安放。
  const auto section = [this](const char *name) {
    auto *w = new QWidget(this);
    w->setObjectName(QLatin1String(name));
    w->setMinimumWidth(0);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
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
  lay->addWidget(importSection);
  lay->addSpacing(16); // spacing.md between groups

  // 列表面表头：标题 + 折叠/展开 + 视图切换
  auto *header = new QWidget(listSection);
  header->setMinimumWidth(0);
  auto *hl = new QHBoxLayout(header);
  hl->setContentsMargins(0, 0, 0, 0);
  hl->setSpacing(4);
  hl->addWidget(caption(tr("数据列表"), header));
  hl->addStretch(1);

  auto *expandBtn = new QToolButton(header);
  expandBtn->setObjectName(QStringLiteral("expandAllTreeButton"));
  expandBtn->setToolTip(tr("全部展开"));
  expandBtn->setAccessibleName(tr("全部展开"));
  expandBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionExpandTree.svg")));
  if (expandBtn->icon().isNull())
    expandBtn->setText(QStringLiteral("▼"));
  expandBtn->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 2px 4px;"));
  hl->addWidget(expandBtn);

  auto *collapseBtn = new QToolButton(header);
  collapseBtn->setObjectName(QStringLiteral("collapseAllTreeButton"));
  collapseBtn->setToolTip(tr("全部折叠"));
  collapseBtn->setAccessibleName(tr("全部折叠"));
  collapseBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionCollapseTree.svg")));
  if (collapseBtn->icon().isNull())
    collapseBtn->setText(QStringLiteral("▶"));
  collapseBtn->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 2px 4px;"));
  hl->addWidget(collapseBtn);

  auto *btnGroup = new QButtonGroup(header);
  auto *treeBtn = new QToolButton(header);
  treeBtn->setObjectName(QStringLiteral("treeViewButton"));
  treeBtn->setText(tr("树形"));
  treeBtn->setCheckable(true);
  treeBtn->setChecked(true);
  treeBtn->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 2px 6px;"));
  auto *tableBtn = new QToolButton(header);
  tableBtn->setObjectName(QStringLiteral("listViewButton"));
  tableBtn->setText(tr("列表"));
  tableBtn->setCheckable(true);
  tableBtn->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 2px 6px;"));
  btnGroup->addButton(treeBtn, 0);
  btnGroup->addButton(tableBtn, 1);
  hl->addWidget(treeBtn);
  hl->addWidget(tableBtn);
  listLay->addWidget(header);

  // 搜索 + 类型筛选 + 计数
  auto *searchRow = new QWidget(listSection);
  searchRow->setMinimumWidth(0);
  auto *srl = new QHBoxLayout(searchRow);
  srl->setContentsMargins(0, 0, 0, 0);
  srl->setSpacing(4);

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
  connect(search, &QLineEdit::textChanged, this, [this] { applyListFilter(); });
  connect(typeFilter, &QComboBox::currentIndexChanged, this, [this] { applyListFilter(); });

  // T31「查看未决」过滤条：过滤开启时露出一行，带「清除过滤」。
  auto *filterBar = new QWidget(listSection);
  filterBar->setObjectName(QStringLiteral("unresolvedFilterBar"));
  filterBar->hide();
  auto *fl = new QHBoxLayout(filterBar);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(4);
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

  m_tree = new QTreeWidget(m_viewStack);
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
  m_tree->setAnimated(true);
  m_tree->setAlternatingRowColors(true);
  // 树 chrome 走 token（活体注册随主题）；选中底浅色保留 #E6F0FA 原值、
  // 暗色换 surfaceAltRaised；选中字浅色 primary / 暗色 primaryText（可读性）。
  PaleoTheme::applyThemedStyleSheet(m_tree, [] {
    const bool dark = PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark;
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QTreeWidget { border: 1px solid %1; background: %2; } "
               "QTreeWidget::item { padding: 3px 0; } "
               "QTreeWidget::item:selected { background-color: %3; color: %4; }")
        .arg(t.border.name().toUpper(), t.surface.name().toUpper(),
             dark ? t.surfaceAltRaised.name().toUpper() : QStringLiteral("#E6F0FA"),
             (dark ? t.primaryText : t.primary).name().toUpper());
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
  table->installEventFilter(this);
  if (table->viewport())
    table->viewport()->installEventFilter(this);
  refreshAssetEmptyState(table, QString());

  m_viewStack->addWidget(m_tree);  // 0: 树形
  m_viewStack->addWidget(table);   // 1: 列表
  listLay->addWidget(m_viewStack, 1);
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

  applyListFilter();   // 空表也写计数

  // 列表选中一条资产 → 中央预览标签（预览部件由 shell 持有，重选聚焦语义
  // 由 DataPreviewTabs 实现）。
  connect(table, &QTableWidget::itemSelectionChanged, this, [this, table]() {
    const QList<QTableWidgetItem *> sel = table->selectedItems();
    if (sel.isEmpty())
      return;
    const QString assetId = sel.front()->data(Qt::UserRole).toString();
    if (!assetId.isEmpty())
      emit assetActivated(assetId);
  });
}
bool DataListPanel::eventFilter(QObject *watched, QEvent *event)
{
  if (event && event->type() == QEvent::Resize)
  {
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
  if (!table)
    return;
  PreviewDocService *svc = m_doc;
  if (!svc)
    return;
  DataCatalog *cat = svc->catalog();
  const QVector<EntityAssetLink> allLinks = cat->links();
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
  // T31「查看未决」：过滤开启时只留仍有未决链接的资产行。
  const bool unresolvedOnly =
      property("paleo.page.filterUnresolved").toBool();
  for (const CatalogAsset &a : cat->assets())
  {
    if (unresolvedOnly)
    {
      bool anyUnresolved = false;
      for (const EntityAssetLink &l : allLinks)
        if (l.assetId == a.id && l.unresolved)
        {
          anyUnresolved = true;
          break;
        }
      if (!anyUnresolved)
        continue;
    }
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
    table->setItem(r, 1, new QTableWidgetItem(a.type));

    // ---- 关联列 ----------------------------------------------------------
    QStringList parts;    // item 文本（控件行也保留，便于检索与断言）
    QStringList resolved; // 控件行回显的实体名/「参考」
    QStringList notes;    // 未决备注（徽标 tooltip：候选名在这里）
    // 身份而非下标（T28）：动作在点击时刻按这些键重扫 links()。
    QString unresolvedRole, unresolvedEntityType; // 本资产第一条未决链接的身份
    const UndoRecord *undoRecord = nullptr;       // 本资产可撤销的挂接记录
    QString promotableRole, promotableEntityId;   // 已决非主链接（「设为主版本」）
    for (int i = 0; i < allLinks.size(); ++i)
    {
      const EntityAssetLink &l = allLinks.at(i);
      if (l.assetId != a.id)
        continue;
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
    bl->setContentsMargins(4, 1, 4, 1);
    bl->setSpacing(4);
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
        return QStringLiteral(
                   "background: %1; color: %2; border: 1px solid %3;"
                   "border-radius: 3px; padding: 0 6px;")
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
      combo->addItem(isWell ? tr("（选择井）") : tr("（选择实体）"), QString());
      for (const CatalogEntity &e : cat->entities(unresolvedEntityType))
        combo->addItem(e.name.isEmpty() ? e.id : e.name, e.id);
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
      cf->setContentsMargins(4, 1, 4, 1);
      cf->setSpacing(4);
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
      auto *primary = new QPushButton(tr("设为主版本"), browse);
      primary->setObjectName(QStringLiteral("setPrimaryButton"));
      primary->setToolTip(tr("同角色旧版本 — 把这条关联设为主关联"));
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
  refreshAssetEmptyState(
      table, unresolvedOnly ? tr("没有未决资产 — 全部资产都已挂接") : QString());

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
  refreshAssetTree();
  applyListFilter();
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

  // 1. 井 (Wells)
  QVector<CatalogEntity> wells = cat->entities(QStringLiteral("well"));
  std::sort(wells.begin(), wells.end(), [](const CatalogEntity &a, const CatalogEntity &b) {
    return naturalNameSort(a.name, b.name);
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
    wellItem->setText(0, w.name);
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
    std::sort(sortedLinks.begin(), sortedLinks.end(), [roleOrder](const EntityAssetLink &a, const EntityAssetLink &b) {
      return roleOrder(a.role) < roleOrder(b.role);
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
      sub->setText(0, QStringLiteral("%1 (%2)").arg(a.displayName, roleDisplay));
      sub->setText(1, detailDisplay);
      if (l.unresolved)
      {
        sub->setForeground(0, PaleoTheme::tokens().warning); // 待复核色（现取随主题）
        sub->setText(1, tr("未决关联"));
      }
    }
  }

  // 资产挂在 auxiliary 实体（reference 角色）上 = 辅助资料，不进测区井/测井分组。
  const auto linkedToAuxEntity = [cat](const QString &assetId) {
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    {
      if (!l.entityId.isEmpty() &&
          cat->entityById(l.entityId).entityType == QLatin1String("auxiliary"))
        return true;
    }
    return false;
  };

  // 2. 测井 (Well Logs: 综合柱状图 + 测井曲线)
  QList<CatalogAsset> logAssets;
  QList<CatalogAsset> compositeAssets;
  for (const CatalogAsset &a : cat->assets())
  {
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
    if (a.type == QLatin1String("seismic"))
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
    if (a.type == QLatin1String("horizon"))
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
}
void DataListPanel::applyListFilter()
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  const auto *search = findChild<QLineEdit *>(QStringLiteral("assetSearchEdit"));
  const auto *typeFilter = findChild<QComboBox *>(QStringLiteral("assetTypeFilter"));
  const QString needle = search ? search->text().trimmed() : QString();
  const QString type = typeFilter ? typeFilter->currentData().toString() : QString();
  int total = 0, shown = 0;
  for (int r = 0; r < table->rowCount(); ++r)
  {
    const QTableWidgetItem *name = table->item(r, 0);
    // 空态指引行（无 UserRole）不算数据，也永远不藏。
    if (!name || name->data(Qt::UserRole).toString().isEmpty())
    {
      table->setRowHidden(r, false);
      continue;
    }
    ++total;
    const auto text = [table, r](int c) {
      const QTableWidgetItem *it = table->item(r, c);
      return it ? it->text() : QString();
    };
    const bool typeOk = type.isEmpty() || text(1) == type;
    const bool textOk = needle.isEmpty() ||
                        text(0).contains(needle, Qt::CaseInsensitive) ||
                        text(1).contains(needle, Qt::CaseInsensitive) ||
                        text(2).contains(needle, Qt::CaseInsensitive);
    const bool visible = typeOk && textOk;
    table->setRowHidden(r, !visible);
    shown += visible ? 1 : 0;
  }
  if (auto *count = findChild<QLabel *>(QStringLiteral("assetCountLabel")))
    count->setText(shown == total ? tr("共 %1 条").arg(total)
                                  : tr("显示 %1 / 共 %2 条").arg(shown).arg(total));

  // 树形视图过滤
  if (m_tree)
  {
    const bool filtering = !needle.isEmpty() || !type.isEmpty();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    {
      QTreeWidgetItem *catItem = m_tree->topLevelItem(i);
      bool catVisible = false;
      for (int j = 0; j < catItem->childCount(); ++j)
      {
        QTreeWidgetItem *subItem = catItem->child(j);
        bool subVisible = false;
        if (subItem->childCount() > 0)
        {
          for (int k = 0; k < subItem->childCount(); ++k)
          {
            QTreeWidgetItem *leaf = subItem->child(k);
            const bool leafMatch = (needle.isEmpty() || leaf->text(0).contains(needle, Qt::CaseInsensitive) || leaf->text(1).contains(needle, Qt::CaseInsensitive));
            leaf->setHidden(!leafMatch);
            if (leafMatch)
              subVisible = true;
          }
          const bool subSelfMatch = (needle.isEmpty() || subItem->text(0).contains(needle, Qt::CaseInsensitive) || subItem->text(1).contains(needle, Qt::CaseInsensitive));
          if (subSelfMatch)
          {
            subVisible = true;
            for (int k = 0; k < subItem->childCount(); ++k)
              subItem->child(k)->setHidden(false);
          }
        }
        else
        {
          subVisible = (needle.isEmpty() || subItem->text(0).contains(needle, Qt::CaseInsensitive) || subItem->text(1).contains(needle, Qt::CaseInsensitive));
        }
        subItem->setHidden(!subVisible);
        if (subVisible)
        {
          catVisible = true;
          if (filtering)
            subItem->setExpanded(true);
        }
      }
      catItem->setHidden(!catVisible);
      if (filtering && catVisible)
        catItem->setExpanded(true);
      else if (!filtering)
      {
        const bool isWellCategory = catItem->text(0).startsWith(tr("井 ("));
        catItem->setExpanded(!isWellCategory);
        for (int j = 0; j < catItem->childCount(); ++j)
        {
          auto *child = catItem->child(j);
          const bool expandChild = child->text(0).contains(QStringLiteral("综合柱状图"));
          child->setExpanded(expandChild);
        }
      }
    }
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
  if (wanted.isEmpty())
    return;
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
  table->selectionModel()->select(
      sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (firstHit)
    table->scrollToItem(firstHit);
}
void DataListPanel::selectAssetInViews(const QString &assetId)
{
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
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