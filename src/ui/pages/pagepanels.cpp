#include "pagepanels.h"

#include "../paleotheme.h" // DESIGN.md token 出口：胶囊/mono 数字面

#include "../../ai/onnxpredictionservice.h" // ORT-free header; runtimeAvailable 调用受 PALEO_HAVE_ORT 保护
#include "../../catalog/datacatalog.h"
#include "../../catalog/entityview.h" // p5a：entityDataView 角色槽门面（纯查询）
#include "../../io/dataimportservice.h"
#include "../../io/arearules.h"
#include "../../domain/mappinghorizons.h"
#include "../../workflow/workflows.h"    // signal names + ValidationWorkflow::validate
#include "../../qgis/qgislayerservice.h" // declared() — forward-declares Qgs*, none included
#include "../../metadata/layermanifest.h" // LayerDeclaration fields
#include "../../domain/types.h"          // ValidationIssue fields

#include <QButtonGroup>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "../paleoicons.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// pagepanels.h fixes the public shape of these classes and declares no data
// members, so service bindings ride on dynamic QObject properties (same idiom
// as workflows.cpp) and child widgets are located by objectName.
// ---------------------------------------------------------------------------
namespace
{
  const char kLayersProp[] = "paleo.page.layers"; // QObject* (QgisLayerService)
  const char kWfProp[]     = "paleo.page.wf";     // QObject* (page workflow)

  // 纯 Qt 折叠段控件（DESIGN.md 浅灰 surface-alt，浅边框，▼/▶ 开合）
  class CollapsibleSection : public QWidget
  {
  public:
    explicit CollapsibleSection(const QString &title, QWidget *parent = nullptr)
      : QWidget(parent)
      , m_title(title)
    {
      auto *lay = new QVBoxLayout(this);
      lay->setContentsMargins(0, 0, 0, 0);
      lay->setSpacing(4);

      m_toggle = new QToolButton(this);
      m_toggle->setText(QStringLiteral("▼  ") + title);
      m_toggle->setCheckable(true);
      m_toggle->setChecked(true);
      m_toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
      m_toggle->setStyleSheet(QStringLiteral(
          "QToolButton { "
          "  font-weight: 600; "
          "  font-size: 8.5pt; "
          "  color: #24303E; "
          "  background: #EDF1F5; "
          "  border: 1px solid #DFE5EC; "
          "  border-radius: 4px; "
          "  padding: 4px 8px; "
          "  text-align: left; "
          "} "
          "QToolButton:hover { background: #E2E8F0; }"));
      m_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

      m_container = new QWidget(this);
      auto *cl = new QVBoxLayout(m_container);
      cl->setContentsMargins(4, 2, 4, 4);
      cl->setSpacing(4);

      lay->addWidget(m_toggle);
      lay->addWidget(m_container);

      QObject::connect(m_toggle, &QToolButton::toggled, this, [this](bool checked) {
        m_container->setVisible(checked);
        m_toggle->setText((checked ? QStringLiteral("▼  ") : QStringLiteral("▶  ")) + m_title);
      });
    }

    QWidget *container() const { return m_container; }
    QVBoxLayout *containerLayout() const { return static_cast<QVBoxLayout *>(m_container->layout()); }
    void setExpanded(bool exp) { m_toggle->setChecked(exp); }

  private:
    QString m_title;
    QToolButton *m_toggle = nullptr;
    QWidget *m_container = nullptr;
  };

  // DESIGN.md `label` token: 8pt muted captions mark panel groups.
  QLabel *caption(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
    return l;
  }

  QVBoxLayout *panelLayout(QWidget *page)
  {
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
    lay->setSpacing(8);
    return lay;
  }

  template <typename T>
  T *child(QObject *root, const char *name)
  {
    return root->findChild<T *>(QLatin1String(name));
  }

  // §42.4/T31: an empty asset table shows a centered guidance row — never a
  // blank panel. Copy names the next concrete step (导入工区文件夹 first).
  void refreshAssetEmptyState(QTableWidget *t, const QString &text)
  {
    if (t->rowCount() > 0)
      return;
    t->insertRow(0);
    auto *it = new QTableWidgetItem(text.isEmpty()
                                        ? DataPage::tr("还没有数据资产 — 先导入工区文件夹，"
                                                       "或用上方按钮导入单个文件")
                                        : text);
    it->setFlags(Qt::NoItemFlags);
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
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
    it->setForeground(QColor(QStringLiteral("#5D6E80"))); // text-muted
    return it;
  }

  QString severityText(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Info:    return ValidatePage::tr("信息");
      case ValidationIssue::Error:   return ValidatePage::tr("错误");
      case ValidationIssue::Warning: // fall through
      default:                       return ValidatePage::tr("警告");
    }
  }

  // T27 胶囊化：级别/状态文字走 DESIGN.md status-tag（浅底深字胶囊），
  // 不再用 setForeground 彩色裸文字（对比度 2.3–3.3:1 不达标）。
  // 信息是中性事实（不占语义色）；错误/警告走各自语义 token。
  PaleoTheme::CapsuleKind severityCapsule(ValidationIssue::Severity s)
  {
    switch (s)
    {
      case ValidationIssue::Error: return PaleoTheme::CapsuleKind::Error;
      case ValidationIssue::Warning: return PaleoTheme::CapsuleKind::Warning;
      case ValidationIssue::Info: // fall through
      default: return PaleoTheme::CapsuleKind::Neutral;
    }
  }

  PaleoTheme::CapsuleKind residualCapsule(const QString &status)
  {
    if (status == QLatin1String("pass"))
      return PaleoTheme::CapsuleKind::Success;
    if (status == QLatin1String("exceed") || status == QLatin1String("warn"))
      return PaleoTheme::CapsuleKind::Warning;
    return PaleoTheme::CapsuleKind::Neutral; // 未计算
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
} // namespace

// ---------------------------------------------------------------------------
// DataPage — 数据管理
// ---------------------------------------------------------------------------
DataPage::DataPage(QWidget *parent)
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
  m_entitySection = section("entityViewSection");
  auto *entityLay = static_cast<QVBoxLayout *>(m_entitySection->layout());

  importLay->addWidget(caption(tr("数据导入"), importSection));
  const struct { const char *name; const char *text; const char *kind; const char *desc; }
      kImports[] = {
    {"importWells", QT_TR_NOOP("导入井数据"), "wells",
     QT_TR_NOOP("选择单个井位/测井/分层文件入库")},
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
  expandBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionExpandTree.svg")));
  if (expandBtn->icon().isNull())
    expandBtn->setText(QStringLiteral("▼"));
  expandBtn->setStyleSheet(QStringLiteral("font-size: 8pt; padding: 2px 4px;"));
  hl->addWidget(expandBtn);

  auto *collapseBtn = new QToolButton(header);
  collapseBtn->setObjectName(QStringLiteral("collapseAllTreeButton"));
  collapseBtn->setToolTip(tr("全部折叠"));
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
  count->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
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
  filterText->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
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
  m_tree->header()->setMinimumSectionSize(20);
  m_tree->setHeaderLabels({tr("数据导航"), tr("类型 / 描述")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_tree->setAnimated(true);
  m_tree->setAlternatingRowColors(true);
  m_tree->setStyleSheet(QStringLiteral(
      "QTreeWidget { border: 1px solid #DFE5EC; background: #FFFFFF; } "
      "QTreeWidget::item { padding: 3px 0; } "
      "QTreeWidget::item:selected { background-color: #E6F0FA; color: #1B73D0; }"));

  connect(btnGroup, &QButtonGroup::idClicked, this, [this](int id) {
    if (m_viewStack)
      m_viewStack->setCurrentIndex(id);
  });

  auto *table = new QTableWidget(0, 3, m_viewStack);
  table->setObjectName(QStringLiteral("assetTable"));
  table->setAccessibleName(tr("资产列表"));
  table->setMinimumWidth(0);
  table->horizontalHeader()->setMinimumSectionSize(20);
  // §4 预览壳重排：预览移到共享地图下方（不再挂右栏）；第三列由「来源」改为
  // 「关联」——已决链接写实体名、未决给「未决」徽标+挂接控件、参考资产写「参考」。
  table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("关联")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  refreshAssetEmptyState(table, QString());

  m_viewStack->addWidget(m_tree);  // 0: 树形
  m_viewStack->addWidget(table);   // 1: 列表
  listLay->addWidget(m_viewStack, 1);
  lay->addWidget(listSection, 1);

  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
    if (!item)
      return;
    const QString assetId = item->data(0, Qt::UserRole).toString();
    const QString wellId = item->data(0, Qt::UserRole + 1).toString();
    const QString nodeType = item->data(0, Qt::UserRole + 2).toString();
    const QString lineMode = item->data(0, Qt::UserRole + 3).toString();

    if (nodeType == QLatin1String("seismic_line"))
    {
      emit seismicLineActivated(assetId, lineMode);
      return;
    }
    if (!wellId.isEmpty() && !assetId.isEmpty())
    {
      emit assetWellActivated(assetId, wellId);
      selectAsset(assetId);
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
      // 双击井节点：打开第一条关联资产（如 A1.Las 测井曲线或井位）
      for (int i = 0; i < item->childCount(); ++i)
      {
        QTreeWidgetItem *child = item->child(i);
        const QString cid = child->data(0, Qt::UserRole).toString();
        if (!cid.isEmpty())
        {
          emit assetWellActivated(cid, wellId);
          selectAsset(cid);
          break;
        }
      }
    }
    else
    {
      item->setExpanded(!item->isExpanded());
    }
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
    if (!assetId.isEmpty())
    {
      selectAsset(assetId);
    }
    else if (!wellId.isEmpty())
    {
      selectAssetsForEntities({wellId});
      emit wellSelected(wellId);
    }
  });

  // ---- p5a：实体角色槽与资产属性视图 ----
  lay->addSpacing(16); // spacing.md between groups
  entityLay->addWidget(caption(tr("数据属性与设置"), m_entitySection));
  auto *viewEmpty = new QLabel(m_entitySection);
  viewEmpty->setObjectName(QStringLiteral("entityViewEmptyLabel"));
  viewEmpty->setWordWrap(true);
  viewEmpty->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  entityLay->addWidget(viewEmpty);

  auto *viewContent = new QWidget(m_entitySection);
  viewContent->setObjectName(QStringLiteral("entityViewContent"));
  auto *vcl = new QVBoxLayout(viewContent);
  vcl->setContentsMargins(0, 0, 0, 0);
  vcl->setSpacing(6);

  auto *entityHeader = new QLabel(viewContent);
  entityHeader->setObjectName(QStringLiteral("entityViewHeader"));
  entityHeader->setStyleSheet(QStringLiteral(
      "QLabel { "
      "  background: #EDF1F5; "
      "  color: #1B73D0; "
      "  font-weight: 600; "
      "  font-size: 10pt; "
      "  padding: 6px 10px; "
      "  border-radius: 4px; "
      "  border: 1px solid #DFE5EC; "
      "}"));
  vcl->addWidget(entityHeader);

  auto *scroll = new QScrollArea(viewContent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *scrollContainer = new QWidget(scroll);
  auto *sl = new QVBoxLayout(scrollContainer);
  sl->setContentsMargins(0, 0, 0, 0);
  sl->setSpacing(8);

  const auto addRow = [](QFormLayout *fl, const QString &label, const char *valName) -> QLabel * {
    auto *lbl = new QLabel(label);
    lbl->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
    auto *val = new QLabel(QStringLiteral("—"));
    val->setObjectName(QLatin1String(valName));
    val->setStyleSheet(QStringLiteral("color: #24303E; font-size: 8.5pt; font-weight: 500;"));
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fl->addRow(lbl, val);
    return val;
  };

  // 1. 基本信息
  auto *secBasic = new CollapsibleSection(tr("基本信息"), scrollContainer);
  secBasic->setObjectName(QStringLiteral("secBasic"));
  auto *formBasic = new QFormLayout(secBasic->container());
  formBasic->setContentsMargins(4, 2, 4, 4);
  formBasic->setSpacing(4);
  addRow(formBasic, tr("名称:"), "propName");
  addRow(formBasic, tr("类型:"), "propType");
  addRow(formBasic, tr("格式:"), "propFormat");
  auto *pathVal = addRow(formBasic, tr("路径:"), "propPath");
  pathVal->setWordWrap(true);
  addRow(formBasic, tr("当前版本:"), "propVersion");
  addRow(formBasic, tr("状态:"), "propStatus");
  sl->addWidget(secBasic);

  // 2. 空间与几何
  auto *secSpatial = new CollapsibleSection(tr("空间与几何"), scrollContainer);
  secSpatial->setObjectName(QStringLiteral("secSpatial"));
  auto *formSpatial = new QFormLayout(secSpatial->container());
  formSpatial->setContentsMargins(4, 2, 4, 4);
  formSpatial->setSpacing(4);
  addRow(formSpatial, tr("坐标系:"), "propCrs");
  auto *coordVal = addRow(formSpatial, tr("坐标/范围:"), "propCoord");
  coordVal->setWordWrap(true);
  addRow(formSpatial, tr("深度/时间:"), "propZRange");
  addRow(formSpatial, tr("采样/规格:"), "propGrid");
  sl->addWidget(secSpatial);

  // 3. 业务角色与关联
  auto *secRoles = new CollapsibleSection(tr("业务角色与关联"), scrollContainer);
  secRoles->setObjectName(QStringLiteral("secRoles"));
  auto *rl = secRoles->containerLayout();
  auto *roleSummary = new QLabel(secRoles->container());
  roleSummary->setObjectName(QStringLiteral("propRoleSummary"));
  roleSummary->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt; margin-bottom: 2px;"));
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
  detailsText->setStyleSheet(QStringLiteral("color: #24303E; font-size: 8.5pt;"));
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
  missing->setWordWrap(true);
  missing->hide(); // 悬空血缘诊断只在 missingSources 非空时出现
  derLay->addWidget(missing);
  sl->addWidget(secDerived);

  sl->addStretch(1);
  scroll->setWidget(scrollContainer);
  vcl->addWidget(scroll, 1);
  entityLay->addWidget(viewContent, 1);
  lay->addWidget(m_entitySection, 1);
  refreshEntityView(); // 初始空态（未选实体）：指引行，不留白板
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

void DataPage::refreshAssetTable()
{
  refreshEntityView(); // p5a：catalog.changed() 接到本槽——实体视图一并重取
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  if (!table)
    return;
  auto *svc = qobject_cast<DataImportService *>(property("paleo.page.importsvc").value<QObject *>());
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
      names->setStyleSheet(QStringLiteral("color: #24303E;"));
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
      // §4 未决徽标：bg #FFF4E0 / text #24303E / border #F29900（DESIGN.md）。
      badge->setStyleSheet(QStringLiteral(
          "background: #FFF4E0; color: #24303E; border: 1px solid #F29900;"
          "border-radius: 3px; padding: 0 6px;"));
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
                const EntityAssetLink link = ls.at(idx);
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
      typeFilter->addItem(t, t);
    typeFilter->setCurrentIndex(qMax(0, typeFilter->findData(keep)));
  }
  refreshAssetTree();
  applyListFilter();
}

namespace
{
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

void DataPage::refreshAssetTree()
{
  if (!m_tree)
    return;
  m_tree->clear();
  auto *svc = qobject_cast<DataImportService *>(property("paleo.page.importsvc").value<QObject *>());
  if (!svc)
    return;
  DataCatalog *cat = svc->catalog();
  if (!cat)
    return;

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
  wellRoot->setExpanded(true);

  for (const CatalogEntity &w : wells)
  {
    auto *wellItem = new QTreeWidgetItem(wellRoot);
    wellItem->setText(0, w.name);
    wellItem->setData(0, Qt::UserRole + 1, w.id);
    wellItem->setData(0, Qt::UserRole + 2, QStringLiteral("well"));
    wellItem->setIcon(0, PaleoIcons::qgisTheme(QStringLiteral("mIconPointLayer.svg")));
    if (w.hasSurface)
      wellItem->setText(1, QStringLiteral("X: %1, Y: %2").arg(QString::number(w.surfaceX, 'f', 1)).arg(QString::number(w.surfaceY, 'f', 1)));

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
        sub->setForeground(0, QColor(QStringLiteral("#F29900")));
        sub->setText(1, tr("未决关联"));
      }
    }
  }

  // 2. 地震 (Seismic)
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

  // 3. 层位 (Horizons)
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
    if (a.displayName.contains(QStringLiteral("D61")))
      hItem->setText(1, tr("目标层位 · 411×641 网格"));
    else
      hItem->setText(1, tr("层位网格"));
  }

  // 4. 辅助资料 (Auxiliary)
  QList<CatalogAsset> auxAssets;
  for (const CatalogAsset &a : cat->assets())
  {
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

void DataPage::applyListFilter()
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
        catItem->setExpanded(true);
        for (int j = 0; j < catItem->childCount(); ++j)
          catItem->child(j)->setExpanded(false);
      }
    }
  }
}

// p5a：当前选中实体的角色槽数据视图与资产属性视图。entityDataView 是纯查询门面——这里
// 同样只读 catalog（assetById/currentVersion 解析展示名与版本号），不写
// 任何东西；页面刷新统一走 catalog.changed() → refreshAssetTable() → 本槽。
void DataPage::refreshEntityView()
{
  QWidget *root = m_entitySection;
  if (!root)
    return;
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

  auto *svc = qobject_cast<DataImportService *>(
      property("paleo.page.importsvc").value<QObject *>());
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  const QString entityId = property("paleo.page.entityId").toString();
  const QString assetId = property("paleo.page.assetId").toString();

  // 空态：工程未开
  if (!cat || !cat->isOpen())
  {
    empty->setText(tr("工程还没打开 — 打开工程后在地图上点选实体，"
                      "这里显示它的角色槽数据全貌"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 实体与资产均未选：指引下一步（地图/列表点选）
  if (entityId.isEmpty() && assetId.isEmpty())
  {
    empty->setText(tr("在地图上点选实体（如井），这里按角色词表显示"
                      "它的数据全貌与派生产物"));
    empty->setVisible(true);
    content->setVisible(false);
    return;
  }

  // 情况 A：选中了实体
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

    const QString title = view.entity.name.isEmpty() ? view.entity.id : view.entity.name;
    header->setText(QStringLiteral("%1  (%2)").arg(title, tr("井实体")));

    // 1. 基本信息
    if (propName) propName->setText(title);
    if (propType) propType->setText(tr("井 (Well)"));
    if (propFormat) propFormat->setText(tr("工程实体记录"));
    if (propPath) propPath->setText(tr("受管工程目录"));
    if (propVersion) propVersion->setText(tr("v1"));
    if (propStatus) propStatus->setText(tr("正常 · 已接入"));

    // 2. 空间与几何
    if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
    if (propCoord)
    {
      if (view.entity.hasSurface)
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
    if (propGrid) propGrid->setText(tr("单井测量与轨迹"));

    // 3. 业务角色与关联
    if (propRoleSummary)
      propRoleSummary->setText(tr("关联资产槽位（全 9 槽词表枚举）"));

    roleTable->setRowCount(0);
    for (const RoleSlot &slot : view.roleSlots)
    {
      const int r = roleTable->rowCount();
      roleTable->insertRow(r);
      const bool slotEmpty = slot.primary.assetId.isEmpty() && slot.members.isEmpty() &&
                             slot.unresolved.isEmpty();
      auto *roleItem = new QTableWidgetItem(
          slot.def.display.isEmpty() ? slot.def.role : slot.def.display);
      roleItem->setFlags(roleItem->flags() & ~Qt::ItemIsEditable);
      if (slotEmpty)
        roleItem->setForeground(QColor(QStringLiteral("#5D6E80"))); // 空槽灰字
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
    return;
  }

  // 情况 B：选中了纯资产（例如地震体、层位、独立文件）
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
  const CatalogVersion v = cat->currentVersion(a.id);

  QString typeDisplay = a.type;
  QString formatDisplay = tr("未知格式");
  if (a.type == QLatin1String("seismic"))
  {
    typeDisplay = tr("三维地震数据体 (3D Seismic)");
    formatDisplay = tr("SEG-Y rev1.0 (IEEE/IBM FP32)");
  }
  else if (a.type == QLatin1String("horizon"))
  {
    typeDisplay = tr("解释层位 (Horizon Grid)");
    formatDisplay = tr("CPS-3 / ZMAP ASCII");
  }
  else if (a.type == QLatin1String("well_log"))
  {
    typeDisplay = tr("测井曲线 (Well Log)");
    formatDisplay = tr("CWLS LAS 2.0");
  }
  else if (a.type == QLatin1String("boundary"))
  {
    typeDisplay = tr("工区边界 (Boundary)");
    formatDisplay = tr("GeoJSON 矢量");
  }
  else if (a.type == QLatin1String("auxiliary") || a.type == QLatin1String("document"))
  {
    typeDisplay = tr("辅助参考资料");
    formatDisplay = a.displayName.section(QLatin1Char('.'), -1).toUpper();
  }

  header->setText(QStringLiteral("%1  (%2)").arg(a.displayName, a.type));

  // 1. 基本信息
  if (propName) propName->setText(a.displayName);
  if (propType) propType->setText(typeDisplay);
  if (propFormat) propFormat->setText(formatDisplay);
  if (propPath) propPath->setText(v.path.isEmpty() ? tr("—") : v.path);
  if (propVersion) propVersion->setText(v.versionNumber > 0 ? tr("v%1").arg(v.versionNumber) : tr("v1"));
  if (propStatus) propStatus->setText(tr("就绪 · 可预览"));

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
      const double dt = survey.sampleIntervalUs > 0 ? survey.sampleIntervalUs / 1000.0 : 2.0;
      propZRange->setText(tr("双程旅行时 0.0 ~ 3000.0 ms (采样间隔 %1 ms)").arg(dt, 0, 'f', 1));
    }
    if (propGrid) propGrid->setText(tr("多道地震数据体 · 40,000 道"));
  }
  else if (a.type == QLatin1String("horizon"))
  {
    if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
    if (propCoord) propCoord->setText(tr("工区构造层位面网格"));
    if (propZRange) propZRange->setText(tr("双程时间 / 构造深度 (TWT)"));
    if (propGrid) propGrid->setText(tr("411 × 641 网格节点 (步长 25m)"));
  }
  else
  {
    if (propCrs) propCrs->setText(QStringLiteral("EPSG:4544 / CGCS2000"));
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
    details << tr("资产标识: %1").arg(a.id);
    details << tr("显示名称: %1").arg(a.displayName);
    if (!v.path.isEmpty())
      details << tr("存储位置: %1").arg(v.path);
    if (a.type == QLatin1String("seismic"))
    {
      details << tr("数据类型: 地震振幅数据体 (SEG-Y)");
      details << tr("道头定义: Inline 189-192, Xline 193-196, CDP 21-24");
      details << tr("振幅动态范围: 浮点连续振幅");
    }
    else if (a.type == QLatin1String("horizon"))
    {
      details << tr("层位属性: 构造解释层面");
      details << tr("数据格式: 规则网格插值曲面");
    }
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
  missing->hide();
}

void DataPage::setUnresolvedFilter(bool on)
{
  setProperty("paleo.page.filterUnresolved", on);
  if (auto *bar = findChild<QWidget *>(QStringLiteral("unresolvedFilterBar")))
    bar->setVisible(on);
  refreshAssetTable();
}

void DataPage::selectAssetsForEntities(const QStringList &entityIds)
{
  // p5a：首个选中 id 驱动实体角色槽视图；空选择清回空态（地图取消点选）。
  setProperty("paleo.page.entityId",
              entityIds.isEmpty() ? QString() : entityIds.front());
  setProperty("paleo.page.assetId", QString());
  refreshEntityView();
  auto *table = findChild<QTableWidget *>(QStringLiteral("assetTable"));
  auto *svc = qobject_cast<DataImportService *>(
      property("paleo.page.importsvc").value<QObject *>());
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

void DataPage::selectAsset(const QString &assetId)
{
  setProperty("paleo.page.assetId", assetId);
  auto *svc = qobject_cast<DataImportService *>(property("paleo.page.importsvc").value<QObject *>());
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  QString matchedEntity;
  if (cat && !assetId.isEmpty())
  {
    for (const EntityAssetLink &l : cat->linksForAsset(assetId))
    {
      if (!l.entityId.isEmpty() && !l.unresolved)
      {
        matchedEntity = l.entityId;
        break;
      }
    }
  }
  setProperty("paleo.page.entityId", matchedEntity);
  refreshEntityView();

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

// ---------------------------------------------------------------------------
// PredictPage — ①智能预测
// ---------------------------------------------------------------------------
PredictPage::PredictPage(PredictionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("算法"), this));
  auto *algos = new QComboBox(this);
  algos->setObjectName(QStringLiteral("algoCombo"));
  lay->addWidget(algos);

  // ONNX params area — visible only for onnx:* algorithms
  auto *paramsArea = new QWidget(this);
  paramsArea->setObjectName(QStringLiteral("onnxParamsArea"));
  auto *paramsLay = new QVBoxLayout(paramsArea);
  paramsLay->setContentsMargins(0, 0, 0, 0);
  paramsLay->setSpacing(8);

  paramsLay->addWidget(caption(tr("输入数据 (逗号分隔浮点数)"), paramsArea));
  auto *inputEdit = new QLineEdit(paramsArea);
  inputEdit->setObjectName(QStringLiteral("onnxInputEdit"));
  inputEdit->setPlaceholderText(QStringLiteral("例如: 0.0, 1.0"));
  paramsLay->addWidget(inputEdit);

  paramsLay->addWidget(caption(tr("输入形状 (逗号分隔整数)"), paramsArea));
  auto *shapeEdit = new QLineEdit(paramsArea);
  shapeEdit->setObjectName(QStringLiteral("onnxShapeEdit"));
  shapeEdit->setPlaceholderText(QStringLiteral("例如: 1, 1"));
  paramsLay->addWidget(shapeEdit);

  paramsLay->addWidget(caption(tr("输入名称"), paramsArea));
  auto *nameEdit = new QLineEdit(paramsArea);
  nameEdit->setObjectName(QStringLiteral("onnxInputNameEdit"));
  nameEdit->setText(QStringLiteral("x"));
  paramsLay->addWidget(nameEdit);

  // 输出网格是工区合同（PROJECT_AREA_PLAN §3）：标定层位栅格 + 同一套
  // geotransform，行/列/像元来自 project_area.json 的 onnx_grid——尺寸
  // 不符时 workflow 拒绝写盘。文案随工程参数刷新（onnxGridCaption）。
  auto *onnxCap = caption(
      tr("输出固定为 %1 工区网格 %2×%3")
          .arg(AreaRules::active().targetHorizon)
          .arg(AreaRules::active().onnxGrid.rows)
          .arg(AreaRules::active().onnxGrid.cols),
      paramsArea);
  onnxCap->setObjectName(QStringLiteral("onnxGridCaption"));
  paramsLay->addWidget(onnxCap);

  paramsArea->hide();
  lay->addWidget(paramsArea);

  const auto updateVisibility = [algos, paramsArea] {
    const QString alg = algos->currentData().toString();
    paramsArea->setVisible(alg.startsWith(QLatin1String("onnx:")));
  };
  connect(algos, &QComboBox::activated, this, updateVisibility);
  connect(algos, &QComboBox::currentIndexChanged, this, updateVisibility);

  auto *run = new QPushButton(tr("运行预测"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this, horizons, algos] {
    const QString horizon = horizons->currentText();
    const QString algId = algos->currentData().toString();
    QVariantMap params;
    if (algId.startsWith(QLatin1String("onnx:")))
    {
      params = parseInputParams();
      if (params.isEmpty())
        return;
    }
    emit runRequested(horizon, algId, params);
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  lay->addStretch(1);

  if (wf) // workflow feedback lands on the status label
  {
    setAlgorithms(wf->availableAlgorithms());
    connect(wf, &PredictionWorkflow::predictionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("预测完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &PredictionWorkflow::predictionFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
#if PALEO_HAVE_ORT
    // 诚实可用性：服务恒绑定，但运行库缺失时 onnx:* 不会出现在算法列表里；
    // 在这里写明原因，而不是静默少列。
    if (!PaleoOnnxService::runtimeAvailable())
      status->setText(tr("ONNX 运行时不可用（vendor/onnxruntime 缺少运行库）"));
#endif
  }
  Q_UNUSED(layers); // reserved: shell lists horizons from the layer service
}

void PredictPage::setHorizons(const QStringList &horizons)
{
  if (auto *combo = child<QComboBox>(this, "horizonCombo"))
  {
    combo->clear();
    combo->addItems(horizons);
  }
}

void PredictPage::setAlgorithms(const QStringList &algIds)
{
  if (auto *combo = child<QComboBox>(this, "algoCombo"))
  {
    combo->clear();
    for (const QString &id : algIds)
    {
      QString display = id;
      if (id.startsWith(QLatin1String("onnx:")))
        display = tr("%1 (ONNX)").arg(id.mid(5));
      combo->addItem(display, id);
    }
    if (auto *paramsArea = child<QWidget>(this, "onnxParamsArea"))
    {
      const QString cur = combo->currentData().toString();
      paramsArea->setVisible(cur.startsWith(QLatin1String("onnx:")));
    }
  }
}

QVariantMap PredictPage::parseInputParams()
{
  auto *status = child<QLabel>(this, "statusLabel");
  auto *inputEdit = child<QLineEdit>(this, "onnxInputEdit");
  auto *shapeEdit = child<QLineEdit>(this, "onnxShapeEdit");
  auto *nameEdit = child<QLineEdit>(this, "onnxInputNameEdit");

  if (!inputEdit || !shapeEdit || !nameEdit)
    return {};

  const QString inStr = inputEdit->text().trimmed();
  if (inStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  const QStringList inParts = inStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (inParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入数据不能为空"));
    return {};
  }
  QVariantList inList;
  for (const QString &p : inParts)
  {
    bool ok = false;
    const float val = p.trimmed().toFloat(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入数据包含非法浮点数: %1").arg(p.trimmed()));
      return {};
    }
    inList.append(val);
  }

  const QString shapeStr = shapeEdit->text().trimmed();
  if (shapeStr.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  const QStringList shapeParts = shapeStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
  if (shapeParts.isEmpty())
  {
    if (status)
      status->setText(tr("输入形状不能为空"));
    return {};
  }
  QVariantList shapeList;
  for (const QString &p : shapeParts)
  {
    bool ok = false;
    const qint64 val = p.trimmed().toLongLong(&ok);
    if (!ok)
    {
      if (status)
        status->setText(tr("输入形状包含非法整数: %1").arg(p.trimmed()));
      return {};
    }
    shapeList.append(val);
  }

  QString nameStr = nameEdit->text().trimmed();
  if (nameStr.isEmpty())
    nameStr = QStringLiteral("x");

  QVariantMap params;
  params.insert(QStringLiteral("input"), inList);
  params.insert(QStringLiteral("shape"), shapeList);
  params.insert(QStringLiteral("inputName"), nameStr);
  return params;
}

// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素
// ---------------------------------------------------------------------------
ConstraintPage::ConstraintPage(ConstraintWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("约束"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("constraintList"));
  list->setAccessibleName(tr("约束列表"));
  lay->addWidget(list, 1);

  auto *spin = new QSpinBox(this);
  spin->setObjectName(QStringLiteral("faciesCodeSpin"));
  spin->setRange(0, 9999);
  spin->setAccessibleName(tr("相代码"));
  lay->addWidget(spin);

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox(this);
  shape->setObjectName(QStringLiteral("shapeCombo"));
  shape->addItem(tr("约束线"), QStringLiteral("line"));
  shape->addItem(tr("约束多边形"), QStringLiteral("polygon"));
  shape->addItem(tr("约束矩形"), QStringLiteral("rect"));
  shape->addItem(tr("约束点"), QStringLiteral("point"));
  shape->addItem(tr("约束圆"), QStringLiteral("circle"));
  shape->addItem(tr("约束椭圆"), QStringLiteral("ellipse"));
  lay->addWidget(shape);

  auto *draw = new QPushButton(tr("绘制约束"), this);
  draw->setObjectName(QStringLiteral("drawButton"));
  lay->addWidget(draw);
  connect(draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested(horizons->currentText(),
                                 shape->currentData().toString(), spin->value());
  });

  auto *field = new QLineEdit(QStringLiteral("z"), this);
  field->setObjectName(QStringLiteral("idwField"));
  field->setPlaceholderText(tr("井属性字段"));
  field->setAccessibleName(tr("插值字段"));
  lay->addWidget(field);

  auto *cell = new QDoubleSpinBox(this);
  cell->setObjectName(QStringLiteral("idwCellSize"));
  cell->setRange(0.0001, 1.0e9);
  cell->setDecimals(4);
  cell->setValue(1.0);
  cell->setAccessibleName(tr("像元大小"));
  lay->addWidget(cell);

  auto *idw = new QPushButton(tr("插值"), this);
  idw->setObjectName(QStringLiteral("runIdwButton"));
  lay->addWidget(idw);
  connect(idw, &QPushButton::clicked, this, [this, horizons] {
    emit runIdwRequested(horizons->currentText());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // 镜像到 ConstraintWorkflow 的 paleo.thickness.* 动态属性；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  lay->addSpacing(16); // spacing.md
  // 厚度样本的层位/基面名随工程参数（AreaRules targetHorizon + 有序集合的
  // 下一界面）——文案在工程打开时由 refreshAreaParamLabels 重写。
  auto *thCap = caption(
      tr("%1→%2 厚度样本")
          .arg(AreaRules::active().targetHorizon,
               baseHorizonFor(AreaRules::active().targetHorizon)),
      this);
  thCap->setObjectName(QStringLiteral("thicknessCaption"));
  lay->addWidget(thCap);
  auto *thTable = new QTableWidget(0, 4, this);
  thTable->setObjectName(QStringLiteral("thicknessTable"));
  thTable->setAccessibleName(tr("厚度样本表"));
  thTable->setHorizontalHeaderLabels(
      {tr("井名"), tr("%1 TVD").arg(AreaRules::active().targetHorizon),
       tr("%1 TVD").arg(baseHorizonFor(AreaRules::active().targetHorizon)),
       tr("层间速度或原因")});
  thTable->verticalHeader()->setVisible(false);
  thTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(thTable, 1);
  auto *thHint = new QLabel(this);
  thHint->setObjectName(QStringLiteral("thicknessHint"));
  thHint->setWordWrap(true);
  thHint->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  lay->addWidget(thHint);

  if (wf) // workflow feedback lands on the status label
  {
    setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));
    connect(wf, &ConstraintWorkflow::constraintAdded, status,
            [status](const QString &id) { status->setText(tr("已添加约束 %1").arg(id)); });
    connect(wf, &ConstraintWorkflow::factorDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("单因素完成：%1 → %2").arg(h, layerId));
            });
  }
}

void ConstraintPage::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  refreshThicknessSamples();
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>(this, "thicknessTable");
  auto *hint = child<QLabel>(this, "thicknessHint");
  if (!table)
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  const QVariantList rows =
      wf ? wf->property("paleo.thickness.samples").toList() : QVariantList();
  const QString message =
      wf ? wf->property("paleo.thickness.message").toString() : QString();

  table->setRowCount(0);
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const int r = table->rowCount();
    table->insertRow(r);
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    const QString tvdTop = m.contains(QStringLiteral("tvd_top"))
                               ? QString::number(m.value(QStringLiteral("tvd_top")).toDouble(), 'f', 1)
                               : QStringLiteral("—");
    const QString tvdBase = m.contains(QStringLiteral("tvd_base"))
                                ? QString::number(m.value(QStringLiteral("tvd_base")).toDouble(), 'f', 1)
                                : QStringLiteral("—");
    // 贡献井 → 层间速度（m/s）；否则 → 原因文案。
    const QString last = m.value(QStringLiteral("contributing")).toBool()
                             ? tr("%1 m/s").arg(m.value(QStringLiteral("vint")).toDouble(), 0, 'f', 0)
                             : m.value(QStringLiteral("reason")).toString();
    auto *itTop = new QTableWidgetItem(tvdTop);
    auto *itBase = new QTableWidgetItem(tvdBase);
    auto *itV = new QTableWidgetItem(last);
    for (auto *it : {name, itTop, itBase, itV})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(r, 0, name);
    table->setItem(r, 1, itTop);
    table->setItem(r, 2, itBase);
    table->setItem(r, 3, itV);
  }
  if (hint)
    hint->setText(message);
}

// ---------------------------------------------------------------------------
// ComposePage — ③综合编图
// ---------------------------------------------------------------------------
ComposePage::ComposePage(CompositionWorkflow *wf, QgisLayerService *layers, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kLayersProp, QVariant::fromValue(static_cast<QObject *>(layers)));

  auto *lay = panelLayout(this);

  // ---- wave/mapping-pipeline 阶段C+E：D61 编图链 / 导出 / 版本按钮块 ----
  lay->addWidget(caption(tr("编图链（等时差 × 层间速度 → 等厚图）"), this));
  // D8 厚度触发：命名「生成 <层位> 等厚图」，未选层位禁用并写原因；使能态
  // 由壳经 setThicknessHorizon 跟 activeHorizon 联动。
  auto *chain = new QPushButton(tr("生成等厚图"), this);
  chain->setObjectName(QStringLiteral("thicknessChainButton"));
  chain->setEnabled(false);
  chain->setToolTip(tr("先在顶部层位 chip 选择层位"));
  chain->setAccessibleName(tr("生成等厚图"));
  connect(chain, &QPushButton::clicked, this, [this] { emit thicknessChainRequested(); });
  lay->addWidget(chain);

  auto *exportPdf = new QPushButton(tr("导出层位图 PDF"), this);
  exportPdf->setObjectName(QStringLiteral("exportPdfButton"));
  exportPdf->setAccessibleName(tr("导出层位图 PDF"));
  connect(exportPdf, &QPushButton::clicked, this, [this] { emit exportPdfRequested(); });
  lay->addWidget(exportPdf);

  auto *saveVersion = new QPushButton(tr("保存版本"), this);
  saveVersion->setObjectName(QStringLiteral("saveVersionButton"));
  saveVersion->setAccessibleName(tr("保存版本"));
  connect(saveVersion, &QPushButton::clicked, this, [this] { emit saveVersionRequested(); });
  lay->addWidget(saveVersion);

  auto *publish = new QPushButton(tr("发布"), this);
  publish->setObjectName(QStringLiteral("publishButton"));
  publish->setAccessibleName(tr("发布"));
  publish->setEnabled(false); // 发布门：PDF 能导出之后再暴露（shell 开闸）
  publish->setToolTip(tr("导出 PDF 后再保存")); // 门控原因（阶段E：PDF 先行）
  connect(publish, &QPushButton::clicked, this, [this] { emit publishRequested(); });
  lay->addWidget(publish);

  // 版本状态标注（已发布 / 编辑中 / 无版本），跟着 shell 的发布门刷新走。
  auto *publishState = new QLabel(this);
  publishState->setObjectName(QStringLiteral("publishStateLabel"));
  publishState->setAccessibleName(tr("版本发布状态"));
  publishState->setWordWrap(true);
  lay->addWidget(publishState);
  lay->addSpacing(16); // spacing.md between groups

  lay->addWidget(caption(tr("单因素图层"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("factorList"));
  list->setAccessibleName(tr("单因素图层列表"));
  lay->addWidget(list, 1);

  auto *fuse = new QPushButton(tr("合成编图"), this);
  fuse->setObjectName(QStringLiteral("fuseButton"));
  lay->addWidget(fuse);
  connect(fuse, &QPushButton::clicked, this, [this, list] {
    QStringList ids;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked)
        ids << list->item(i)->data(Qt::UserRole).toString();
    emit fuseRequested(ids);
  });

  lay->addWidget(caption(tr("沉积相面"), this));
  auto *rasterCombo = new QComboBox(this);
  rasterCombo->setObjectName(QStringLiteral("faciesRasterCombo"));
  rasterCombo->setAccessibleName(tr("待转面的栅格"));
  lay->addWidget(rasterCombo);

  auto *minArea = new QDoubleSpinBox(this);
  minArea->setObjectName(QStringLiteral("minAreaSpin"));
  minArea->setAccessibleName(tr("碎屑面积阈值"));
  minArea->setDecimals(4);
  minArea->setRange(0.0, 1.0e9);
  minArea->setSingleStep(1.0);
  minArea->setPrefix(tr("最小面积 "));
  lay->addWidget(minArea);

  auto *simplify = new QDoubleSpinBox(this);
  simplify->setObjectName(QStringLiteral("simplifySpin"));
  simplify->setAccessibleName(tr("边界简化容差"));
  simplify->setDecimals(4);
  simplify->setRange(0.0, 1.0e9);
  simplify->setSingleStep(1.0);
  simplify->setPrefix(tr("简化容差 "));
  lay->addWidget(simplify);

  auto *polygonize = new QPushButton(tr("转为相多边形"), this);
  polygonize->setObjectName(QStringLiteral("polygonizeButton"));
  polygonize->setAccessibleName(tr("转为相多边形"));
  lay->addWidget(polygonize);
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo, minArea, simplify] {
    const QString layerId = rasterCombo->currentData().toString();
    if (layerId.isEmpty())
    {
      if (auto *status = child<QLabel>(this, "statusLabel"))
        status->setText(tr("还没有可转面的栅格 — 先运行预测或合成编图"));
      return;
    }
    emit polygonizeRequested(layerId, minArea->value(), simplify->value());
  });
  // autoplan §5C：厚度栅格不是相编码；工程里没有相编码栅格时明确说明，
  // 而不是笼统的「还没有可转面的栅格」。
  connect(polygonize, &QPushButton::clicked, this, [this, rasterCombo] {
    if (!rasterCombo->currentData().toString().isEmpty())
      return;
    auto *status = child<QLabel>(this, "statusLabel");
    auto *layers = qobject_cast<QgisLayerService *>(
        property(kLayersProp).value<QObject *>());
    QVector<LayerDeclaration> declared;
    if (status && layers && layers->tryDeclared(&declared))
    {
      const bool anyRaster = std::any_of(
          declared.cbegin(), declared.cend(), [](const LayerDeclaration &d) {
            return d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
          });
      if (anyRaster)
        status->setText(tr("没有相编码栅格，这一工区不从厚度生成相"));
    }
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);
  if (wf)
  {
    connect(wf, &CompositionWorkflow::compositionDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("合成完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsReady, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("相多边形完成：%1 → %2").arg(h, layerId));
            });
    connect(wf, &CompositionWorkflow::faciesPolygonsFailed, status,
            [status](const QString &, const QString &error) { status->setText(error); });
  }
  // A raster declared after this page exists (e.g. a fresh ONNX prediction)
  // still lands in the combo; the declaration set is the source of truth.
  if (layers)
    connect(layers, &QgisLayerService::layerDeclared, this,
            [this](const QString &) { refreshFactors(); });

  refreshFactors();
}

void ComposePage::setPublishEnabled(bool enabled)
{
  setPublishState(enabled, -1, -1); // 旧调用形态：不带残差评估
}

void ComposePage::setPublishState(bool hasPdf, int covered, int total)
{
  auto *btn = child<QPushButton>(this, "publishButton");
  if (!btn)
    return;
  // covered<0 = 调用方没评估残差 → 不拿残差卡门；其余情形须 total>0 且全
  // 覆盖（每口井都有残差或原因，§177）。
  const bool residualsOk = covered < 0 || (total > 0 && covered == total);
  btn->setEnabled(hasPdf && residualsOk);
  QString tip;
  if (!hasPdf)
    tip = tr("导出 PDF 后再保存");
  else if (!residualsOk)
    tip = total > 0 ? tr("还有 %1/%2 口井没有残差或原因 — 先在验证页运行验证")
                        .arg(total - covered)
                        .arg(total)
                    : tr("先在验证页运行验证");
  btn->setToolTip(tip);
}

void ComposePage::setVersionState(int version, bool published)
{
  if (auto *label = child<QLabel>(this, "publishStateLabel"))
  {
    // T27：版本状态胶囊——已发布=绿、编辑中=橙、无版本=中性「未计算」类。
    label->setText(version <= 0 ? tr("还没有保存的版本")
                   : published  ? tr("已发布 · v%1").arg(version)
                                : tr("编辑中 · v%1").arg(version));
    label->setStyleSheet(PaleoTheme::capsuleStyleSheet(
        version <= 0 ? PaleoTheme::CapsuleKind::Neutral
                     : (published ? PaleoTheme::CapsuleKind::Success
                                  : PaleoTheme::CapsuleKind::Warning)));
  }
  if (auto *save = child<QPushButton>(this, "saveVersionButton"))
  {
    save->setText(published ? tr("保存新版本") : tr("保存版本"));
    // tooltip 写明改名原因；ribbon 镜像靠 ToolTipChange 顺带同步文案。
    save->setToolTip(published ? tr("v%1 已发布、快照只读 — 继续保存会产生新版本").arg(version)
                               : tr("把当前编图保存为版本快照"));
  }
}

void ComposePage::setThicknessHorizon(const QString &horizon)
{
  auto *chain = child<QPushButton>(this, "thicknessChainButton");
  if (!chain)
    return;
  chain->setEnabled(!horizon.isEmpty());
  chain->setText(horizon.isEmpty() ? tr("生成等厚图")
                                  : tr("生成 %1 等厚图").arg(horizon));
  chain->setAccessibleName(chain->text());
  chain->setToolTip(horizon.isEmpty()
                        ? tr("先在顶部层位 chip 选择层位")
                        : tr("等时差（双向 ms）× 层间速度 IDW → 厚度栅格"));
}

void ComposePage::refreshFactors()
{
  auto *list = child<QListWidget>(this, "factorList");
  if (!list)
    return;
  auto *layers = qobject_cast<QgisLayerService *>(
      property(kLayersProp).value<QObject *>());
  // Manifest read failure keeps current content — an empty declaration set
  // here would silently blank the page.
  QVector<LayerDeclaration> declared;
  if (!layers || !layers->tryDeclared(&declared))
    return;
  list->clear();
  for (const LayerDeclaration &d : declared)
  {
    if (d.group != QLatin1String("04_SingleFactor"))
      continue;
    auto *it = new QListWidgetItem(d.layerId, list);
    it->setData(Qt::UserRole, d.layerId);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(Qt::Unchecked);
  }

  auto *combo = child<QComboBox>(this, "faciesRasterCombo");
  if (!combo)
    return;
  const QString previous = combo->currentData().toString();
  combo->clear();
  for (const LayerDeclaration &d : declared)
  {
    const bool raster = d.type.compare(QLatin1String("raster"), Qt::CaseInsensitive) == 0;
    const bool groupOk = d.group == QLatin1String("03_Composite") ||
                         d.group == QLatin1String("01_Prediction") ||
                         d.group == QLatin1String("02_Prediction") ||
                         d.group == QLatin1String("03_Predict");
    const bool idOk = d.layerId.startsWith(QLatin1String("composite.")) ||
                      d.layerId.startsWith(QLatin1String("pred.")) ||
                      d.layerId.startsWith(QLatin1String("predict."));
    if (!raster || (!groupOk && !idOk))
      continue;
    combo->addItem(d.layerId, d.layerId);
  }
  const int keep = combo->findData(previous);
  if (keep >= 0)
    combo->setCurrentIndex(keep);
}

// ---------------------------------------------------------------------------
// ValidatePage — ④验证
// ---------------------------------------------------------------------------
ValidatePage::ValidatePage(ValidationWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));

  auto *lay = panelLayout(this);
  auto *run = new QPushButton(tr("运行验证"), this);
  run->setObjectName(QStringLiteral("runButton"));
  lay->addWidget(run);
  connect(run, &QPushButton::clicked, this, [this] { populate(); });

  // autoplan §5C：D61 残差表 —— 每口井一行（井名/残差或原因/阈值），
  // 状态字+颜色（通过 #43A047 / 超过阈值 #F29900 / 未计算 #5D6E80）。
  // 计数行在表头；还没跑时面板写「还没有计算 <标定层位> 残差」。
  auto *resSummary =
      new QLabel(tr("还没有计算 %1 残差").arg(AreaRules::active().targetHorizon), this);
  resSummary->setObjectName(QStringLiteral("residualSummaryLabel"));
  lay->addWidget(resSummary);
  auto *resCap =
      caption(tr("%1 时间残差").arg(AreaRules::active().targetHorizon), this);
  resCap->setObjectName(QStringLiteral("residualCaption"));
  lay->addWidget(resCap);
  auto *resTable = new QTableWidget(0, 3, this);
  resTable->setObjectName(QStringLiteral("residualTable"));
  resTable->setAccessibleName(tr("%1 残差表").arg(AreaRules::active().targetHorizon));
  resTable->setHorizontalHeaderLabels({tr("井名"), tr("残差或原因"), tr("阈值")});
  resTable->verticalHeader()->setVisible(false);
  resTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(resTable, 1);
  // T24：残差行双击与问题行同一条 locateRequested——列 0 上挂
  // layerId/WKT/payload（populate 写入），三视图按同一载荷联动。
  connect(resTable, &QTableWidget::itemDoubleClicked, this, [this, resTable](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = resTable->item(it->row(), 0);
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  auto *table = new QTableWidget(0, 4, this);
  table->setObjectName(QStringLiteral("issueTable"));
  table->setAccessibleName(tr("验证问题列表"));
  table->setHorizontalHeaderLabels({tr("级别"), tr("代码"), tr("信息"), tr("图层")});
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(table, 1);
  connect(table, &QTableWidget::itemDoubleClicked, this, [this, table](QTableWidgetItem *it) {
    if (!it)
      return;
    auto *first = table->item(it->row(), 0); // issue data lives on column 0
    if (first)
      emit locateRequested(first->data(Qt::UserRole).toString(),
                           first->data(Qt::UserRole + 1).toString(),
                           first->data(Qt::UserRole + 2).toMap());
  });

  // 「在数据页看这条剖面」（预览壳重排）：问题行/残差行的载荷里带 inline
  // 测线号才可用；点击把整份 payload 原样发给 shell（切数据页+聚焦该测线）。
  // 两张表共用一颗钮——armed 载荷记在按钮属性上，最近一次选中的表生效。
  auto *openSection = new QPushButton(tr("在数据页看这条剖面"), this);
  openSection->setObjectName(QStringLiteral("openSeismicSectionButton"));
  openSection->setAccessibleName(tr("在数据页看这条剖面"));
  openSection->setEnabled(false);
  lay->addWidget(openSection);
  const auto hasSection = [](const QVariantMap &p) {
    return p.value(QStringLiteral("inline"), -1).toInt() >= 0;
  };
  const auto armSectionFrom = [openSection, hasSection](QTableWidget *src) {
    int row = src ? src->currentRow() : -1;
    if (row < 0 && src)
    {
      const QList<QTableWidgetItem *> sel = src->selectedItems();
      if (!sel.isEmpty())
        row = sel.front()->row();
    }
    auto *first = (src && row >= 0) ? src->item(row, 0) : nullptr;
    const QVariantMap p = first ? first->data(Qt::UserRole + 2).toMap() : QVariantMap();
    openSection->setProperty("armedPayload", p);
    openSection->setEnabled(hasSection(p));
  };
  connect(table, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, table]() { armSectionFrom(table); });
  connect(resTable, &QTableWidget::itemSelectionChanged, openSection,
          [armSectionFrom, resTable]() { armSectionFrom(resTable); });
  connect(openSection, &QPushButton::clicked, this,
          [this, openSection, hasSection]() {
            const QVariantMap p = openSection->property("armedPayload").toMap();
            if (hasSection(p))
              emit seismicSectionRequested(p);
          });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  lay->addWidget(status);
  if (wf)
    connect(wf, &ValidationWorkflow::validationDone, status,
            [status](int n) { status->setText(tr("发现 %1 个问题").arg(n)); });
}

void ValidatePage::populate()
{
  auto *table = child<QTableWidget>(this, "issueTable");
  if (!table)
    return;
  table->setRowCount(0);
  auto *wf = qobject_cast<ValidationWorkflow *>(
      property(kWfProp).value<QObject *>());
  if (!wf)
    return;
  const QList<ValidationIssue> issues = wf->validate();
  for (const ValidationIssue &v : issues)
  {
    const int row = table->rowCount();
    table->insertRow(row);
    auto *sev = new QTableWidgetItem(); // 级别文字进胶囊控件（T27）
    sev->setData(Qt::UserRole, v.layerId);       // locate intent reads these
    sev->setData(Qt::UserRole + 1, v.wktLocation);
    // 三视图联动载荷：wellId/horizon/inline/time_ms（非残差问题不含井字段）。
    QVariantMap payload = v.details;
    if (!v.wellId.isEmpty())
      payload.insert(QStringLiteral("wellId"), v.wellId);
    if (!v.horizon.isEmpty())
      payload.insert(QStringLiteral("horizon"), v.horizon);
    sev->setData(Qt::UserRole + 2, payload);
    sev->setFlags(sev->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 0, sev);
    table->setCellWidget(row, 0,
                         PaleoTheme::capsuleLabel(severityText(v.severity),
                                                  severityCapsule(v.severity), table));
    auto *code = new QTableWidgetItem(v.code);
    auto *msg = new QTableWidgetItem(v.message);
    auto *layer = new QTableWidgetItem(v.layerId);
    for (auto *it : {code, msg, layer})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, 1, code);
    table->setItem(row, 2, msg);
    table->setItem(row, 3, layer);
  }

  // ---- D61 逐井残差表（autoplan §5C）--------------------------------------
  auto *resTable = child<QTableWidget>(this, "residualTable");
  auto *resSummary = child<QLabel>(this, "residualSummaryLabel");
  if (!resTable)
    return;
  resTable->setRowCount(0);
  const QVariantList rows = wf->lastResidualRows();
  if (rows.isEmpty())
  {
    // 空残差表两种含义要分清（T25）：D61 栅格缺失/打不开时工作流会
    // 发 RASTER_MISSING 问题——摘要行复述它的原因，不冒充「没跑过」。
    QString rasterReason;
    for (const ValidationIssue &v : issues)
      if (v.code == QLatin1String("RASTER_MISSING"))
        rasterReason = v.message;
    if (resSummary)
      resSummary->setText(rasterReason.isEmpty()
                              ? tr("还没有计算 %1 残差")
                                    .arg(AreaRules::active().targetHorizon)
                              : rasterReason);
    return;
  }
  fillResidualTable(resTable, rows);
  if (resSummary)
  {
    int nExceed = 0;
    for (const QVariant &v : rows)
      if (v.toMap().value(QStringLiteral("status")).toString() == QLatin1String("exceed"))
        ++nExceed;
    const double thr = rows.first().toMap()
                           .value(QStringLiteral("threshold_ms"), 10.0)
                           .toDouble();
    resSummary->setText(tr("%1 口超过 %2 ms").arg(nExceed).arg(thr, 0, 'f', 0));
  }
}

void ValidatePage::fillResidualTable(QTableWidget *resTable, const QVariantList &rows)
{
  if (!resTable || rows.isEmpty())
    return;
  const double thr = rows.first().toMap()
                         .value(QStringLiteral("threshold_ms"), 10.0)
                         .toDouble();
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const QString status = m.value(QStringLiteral("status")).toString();
    const int r = resTable->rowCount();
    resTable->insertRow(r);
    QString word;
    if (status == QLatin1String("pass"))
      word = tr("通过");
    else if (status == QLatin1String("exceed"))
      word = tr("超过阈值");
    else if (status == QLatin1String("warn"))
      word = tr("警告");
    else
      word = tr("未计算"); // 中性胶囊：无栅格/未跑，不占语义色
    const double residualMs = m.value(QStringLiteral("residual_ms")).toDouble();
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    // T24：残差行与问题行共用三视图联动载荷——列 0 挂 layerId/POINT WKT/
    // payload（wellId/horizon/well_x/well_y/inline/time_ms）。采样点是分层
    // X/Y（缺省井口），缺坐标的行不填 (0,0)（threewaylocator 只认成对字段）。
    name->setData(Qt::UserRole, m.value(QStringLiteral("layer_id")).toString());
    const bool hasXY = m.contains(QStringLiteral("x")) && m.contains(QStringLiteral("y"));
    if (hasXY)
      name->setData(Qt::UserRole + 1,
                    QStringLiteral("POINT(%1 %2)")
                        .arg(m.value(QStringLiteral("x")).toDouble())
                        .arg(m.value(QStringLiteral("y")).toDouble()));
    QVariantMap payload;
    payload.insert(QStringLiteral("wellId"), m.value(QStringLiteral("well_id")).toString());
    payload.insert(QStringLiteral("horizon"), m.value(QStringLiteral("horizon")).toString());
    payload.insert(QStringLiteral("well_name"), m.value(QStringLiteral("well_name")).toString());
    payload.insert(QStringLiteral("inline"), m.value(QStringLiteral("inline"), -1).toInt());
    if (hasXY)
    {
      payload.insert(QStringLiteral("well_x"), m.value(QStringLiteral("x")));
      payload.insert(QStringLiteral("well_y"), m.value(QStringLiteral("y")));
    }
    if (m.contains(QStringLiteral("time_ms")))
      payload.insert(QStringLiteral("time_ms"), m.value(QStringLiteral("time_ms")));
    if (m.contains(QStringLiteral("residual_ms")))
      payload.insert(QStringLiteral("residual_ms"), m.value(QStringLiteral("residual_ms")));
    name->setData(Qt::UserRole + 2, payload);
    auto *val = new QTableWidgetItem(); // 文本进胶囊+mono 值控件（T27/T32）
    // 状态胶囊 + 数值 mono 面：残差数字右对齐等宽（DESIGN.md mono token）。
    auto *cell = new QWidget(resTable);
    auto *hl = new QHBoxLayout(cell);
    hl->setContentsMargins(4, 1, 4, 1);
    hl->setSpacing(4);
    hl->addWidget(PaleoTheme::capsuleLabel(word, residualCapsule(status), cell));
    if (m.contains(QStringLiteral("residual_ms")))
    {
      auto *num = new QLabel(tr("%1 ms").arg(residualMs, 0, 'f', 1), cell);
      num->setFont(PaleoTheme::monoFont());
      num->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      num->setStyleSheet(QStringLiteral("color: #24303E;"));
      hl->addWidget(num);
    }
    else if (!m.value(QStringLiteral("reason")).toString().isEmpty())
    {
      auto *reason = new QLabel(m.value(QStringLiteral("reason")).toString(), cell);
      reason->setWordWrap(true);
      reason->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
      hl->addWidget(reason, 1);
    }
    hl->addStretch(1);
    auto *thrItem = new QTableWidgetItem(tr("%1 ms").arg(thr, 0, 'f', 0));
    thrItem->setFont(PaleoTheme::monoFont()); // 阈值列也是数字面
    thrItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    for (auto *it : {name, val, thrItem})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    resTable->setItem(r, 0, name);
    resTable->setItem(r, 1, val);
    resTable->setItem(r, 2, thrItem);
    resTable->setCellWidget(r, 1, cell);
  }
}
