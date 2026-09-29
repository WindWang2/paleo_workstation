// 层：视图
// ui/pages/dataopsviews — D7 列表形态与性能 + D2.7 高亮委托。
//   · FlatAssetModel：QAbstractTableModel，canFetchMore/fetchMore 批量装载
//     （每批 256）——10k 资产不一次性建 item（D7.3）
//   · AssetVirtualView：QTableView 包 FlatAssetModel（大数据态视图页）
//   · AssetIconView：QListWidget IconMode（D7.1 图标态）
//   · AssetGroupTree：按类型/实体/标签/版本分组的平表（D7.6）
//   · HighlightDelegate：搜索命中字符高亮自绘（D2.7）
//   · ColumnConfig：列显隐/顺序/宽持久化（D7.2）
//   · 列头漏斗：列内联过滤（D7.8，excel 风）
#pragma once

#include <QAbstractTableModel>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>

#include "../paleotheme.h"
#include "dataops/dataopsfilter.h"
#include "dataops/dataopsmodel.h"

namespace paleo::dataops
{

// ---- D7.3 FlatAssetModel（批量 fetch）---------------------------------------
class FlatAssetModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  enum Column
  {
    ColName = 0,
    ColType,
    ColEntities,
    ColTags,
    ColSize,
    ColVersion,
    ColTime,
    ColCount
  };

  static constexpr int kBatchSize = 256;

  explicit FlatAssetModel(QObject *parent = nullptr)
    : QAbstractTableModel(parent)
  {
  }

  // 全量行一次灌入（行快照已在调用方过滤/排序好），可见性由 fetchMore 控制。
  void setRows(const QVector<AssetRowInfo> &rows)
  {
    beginResetModel();
    m_rows = rows;
    m_visible = qMin(rows.size(), kBatchSize); // 首屏一批
    endResetModel();
  }
  const QVector<AssetRowInfo> &allRows() const { return m_rows; }
  int visibleCount() const { return m_visible; }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override
  {
    return parent.isValid() ? 0 : m_visible;
  }
  int columnCount(const QModelIndex &parent = QModelIndex()) const override
  {
    return parent.isValid() ? 0 : ColCount;
  }
  bool canFetchMore(const QModelIndex &parent) const override
  {
    return !parent.isValid() && m_visible < m_rows.size();
  }
  void fetchMore(const QModelIndex &parent) override
  {
    if (parent.isValid())
      return;
    const int add = qMin(kBatchSize, m_rows.size() - m_visible);
    if (add <= 0)
      return;
    beginInsertRows(QModelIndex(), m_visible, m_visible + add - 1);
    m_visible += add;
    endInsertRows();
  }

  QVariant data(const QModelIndex &idx, int role) const override
  {
    if (!idx.isValid() || idx.row() >= m_visible)
      return QVariant();
    const AssetRowInfo &r = m_rows.at(idx.row());
    if (role == Qt::UserRole)
      return r.assetId;
    if (role == Qt::DisplayRole)
    {
      switch (idx.column())
      {
        case ColName: return r.displayName;
        case ColType: return r.effectiveType;
        case ColEntities: return r.entityNames.join(QStringLiteral("、"));
        case ColTags: return r.tags.join(QStringLiteral("、"));
        case ColSize: return r.sizeBytes > 0 ? QString::number(r.sizeBytes) : QString();
        case ColVersion: return QStringLiteral("v%1").arg(r.currentVersionNo);
        case ColTime: return r.lastModified.toString(QStringLiteral("yyyy-MM-dd"));
      }
    }
    else if (role == Qt::TextAlignmentRole)
    {
      if (idx.column() == ColSize || idx.column() == ColVersion)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    }
    else if (role == Qt::ToolTipRole)
    {
      return tr("%1\n类型 %2 · 版本 v%3 · 标签 %4")
          .arg(r.displayName, r.effectiveType)
          .arg(r.currentVersionNo)
          .arg(r.tags.isEmpty() ? tr("无") : r.tags.join(QStringLiteral("、")));
    }
    return QVariant();
  }

  QVariant headerData(int section, Qt::Orientation o, int role) const override
  {
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
      return QAbstractTableModel::headerData(section, o, role);
    switch (section)
    {
      case ColName: return tr("名称");
      case ColType: return tr("类型");
      case ColEntities: return tr("关联");
      case ColTags: return tr("标签");
      case ColSize: return tr("大小");
      case ColVersion: return tr("版本");
      case ColTime: return tr("时间");
    }
    return QVariant();
  }

  QString assetIdAt(int row) const
  {
    return row >= 0 && row < m_visible ? m_rows.at(row).assetId : QString();
  }

private:
  QVector<AssetRowInfo> m_rows;
  int m_visible = 0;
};

// 虚拟滚动视图（QTableView + 模型批量 fetch；D7.3 验收载体）。
class AssetVirtualView : public QTableView
{
  Q_OBJECT
public:
  explicit AssetVirtualView(QWidget *parent = nullptr)
    : QTableView(parent)
  {
    setObjectName(QStringLiteral("assetVirtualTable"));
    setAccessibleName(tr("资产虚拟列表"));
    setModel(&m_model);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    verticalHeader()->setVisible(false);
    horizontalHeader()->setStretchLastSection(false);
    setWordWrap(false);
  }
  ~AssetVirtualView() override { setModel(nullptr); }
  FlatAssetModel *flatModel() { return &m_model; }

  // 滚动到底触发 fetchMore（QTableView 自带 fetchMore 感知，这里显式暴露
  // 给测试/快捷键「跳到末尾」）。
  void fetchAllBatches()
  {
    while (m_model.canFetchMore(QModelIndex()))
      m_model.fetchMore(QModelIndex());
  }

private:
  FlatAssetModel m_model;
};

// ---- D7.1 图标态（QListWidget IconMode）-------------------------------------
class AssetIconView : public QListWidget
{
  Q_OBJECT
public:
  explicit AssetIconView(QWidget *parent = nullptr)
    : QListWidget(parent)
  {
    setObjectName(QStringLiteral("assetIconView"));
    setAccessibleName(tr("资产图标视图"));
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setIconSize(QSize(28, 28));
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSpacing(6);
    setWordWrap(true);
  }
  void loadRows(const QVector<AssetRowInfo> &rows)
  {
    clear();
    for (const AssetRowInfo &r : rows)
    {
      auto *it = new QListWidgetItem(r.displayName, this);
      it->setData(Qt::UserRole, r.assetId);
      it->setToolTip(tr("类型 %1 · 版本 v%2").arg(r.effectiveType).arg(r.currentVersionNo));
    }
  }
};

// ---- D7.6 分组平表（类型/实体/标签/版本分组）--------------------------------
class AssetGroupTree : public QTreeWidget
{
  Q_OBJECT
public:
  enum class GroupBy { Type, Entity, Tag, Version };
  explicit AssetGroupTree(QWidget *parent = nullptr)
    : QTreeWidget(parent)
  {
    setObjectName(QStringLiteral("assetGroupTree"));
    setAccessibleName(tr("分组资产列表"));
    setColumnCount(2);
    setHeaderLabels({tr("分组"), tr("数量 / 说明")});
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    header()->setSectionResizeMode(0, QHeaderView::Stretch);
  }

  void setGroupBy(GroupBy g) { m_group = g; }
  GroupBy groupBy() const { return m_group; }

  void loadRows(const QVector<AssetRowInfo> &rows)
  {
    clear();
    // 组键 → (组名, 计数, 资产节点)；资产可多组（多标签/多实体）。
    QMap<QString, QTreeWidgetItem *> groups;
    for (const AssetRowInfo &r : rows)
    {
      QStringList keys = groupKeys(r);
      if (keys.isEmpty())
        keys << tr("（未分组）");
      for (const QString &k : keys)
      {
        QTreeWidgetItem *grp = groups.value(k);
        if (!grp)
        {
          grp = new QTreeWidgetItem(this);
          grp->setText(0, k);
          grp->setData(0, Qt::UserRole + 2, QStringLiteral("group"));
          grp->setExpanded(true);
          groups.insert(k, grp);
        }
        auto *leaf = new QTreeWidgetItem(grp);
        leaf->setText(0, r.displayName);
        leaf->setData(0, Qt::UserRole, r.assetId);
        leaf->setData(0, Qt::UserRole + 2, QStringLiteral("asset"));
        leaf->setText(1, r.effectiveType);
      }
    }
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
      if (it.value())
        it.value()->setText(1, tr("%1 项").arg(it.value()->childCount()));
  }

private:
  QStringList groupKeys(const AssetRowInfo &r) const
  {
    switch (m_group)
    {
      case GroupBy::Type:
        return {r.effectiveType.isEmpty() ? tr("未知类型") : r.effectiveType};
      case GroupBy::Entity:
        return r.entityNames.isEmpty() ? QStringList{} : r.entityNames;
      case GroupBy::Tag:
        return r.tags;
      case GroupBy::Version:
        return {QStringLiteral("v%1").arg(r.currentVersionNo)};
    }
    return {};
  }
  GroupBy m_group = GroupBy::Type;
};

// ---- D2.7 搜索命中高亮委托 ---------------------------------------------------
// 命中区间用 primary 色加粗（DESIGN.md 交互蓝——选中/焦点/命中同一语义族）。
class HighlightDelegate : public QStyledItemDelegate
{
  Q_OBJECT
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void setNeedle(const QString &n) { m_needle = n; }
  QString needle() const { return m_needle; }

  void paint(QPainter *p, const QStyleOptionViewItem &opt,
             const QModelIndex &idx) const override
  {
    // 文本经基类画底/选中态，再叠命中高亮——保持主题一致。
    QStyledItemDelegate::paint(p, opt, idx);
    if (m_needle.isEmpty() || !opt.rect.isValid())
      return;
    const QString text = idx.data(Qt::DisplayRole).toString();
    if (text.isEmpty())
      return;
    const QList<QPair<int, int>> ranges = matchRanges(text, m_needle);
    if (ranges.isEmpty())
      return;
    QStyleOptionViewItem o = opt;
    initStyleOption(&o, idx); // 取 elide 后实际文本
    const QString elided = o.fontMetrics.elidedText(
        text, o.textElideMode, o.rect.width() - 8);
    // 命中在 elide 可见段内才画：偏移按 elided 前缀近似（保守——只画前段命中）。
    p->save();
    p->setClipRect(o.rect);
    QFont f = o.font;
    f.setBold(true);
    p->setFont(f);
    p->setPen(PaleoTheme::tokens().primary);
    int x = o.rect.left() + 4;
    int drawn = 0;
    for (const auto &rg : ranges)
    {
      if (rg.first < drawn)
        continue; // 重叠跳过
      const int segLen = rg.first - drawn;
      if (segLen > 0)
        x += p->fontMetrics().horizontalAdvance(elided.left(drawn + segLen)) -
             p->fontMetrics().horizontalAdvance(elided.left(drawn));
      const QString hit = elided.mid(rg.first, rg.second);
      if (hit.isEmpty())
        break;
      p->drawText(x, o.rect.y(), p->fontMetrics().horizontalAdvance(hit),
                  o.rect.height(), Qt::AlignVCenter, hit);
      x += p->fontMetrics().horizontalAdvance(hit);
      drawn = rg.first + rg.second;
      if (drawn >= elided.size())
        break;
    }
    p->restore();
  }

private:
  QString m_needle;
};

// ---- D7.2 列配置（显隐/顺序/宽持久化）---------------------------------------
struct ColumnState
{
  int logicalIndex = 0;
  bool visible = true;
  int width = -1; // -1 = 未自定义（保留交互宽）
};
inline void saveColumnState(const QString &viewKey, QHeaderView *header)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("dataops/cols/") + viewKey,
             QString::fromUtf8(header->saveState().toBase64()));
}
inline bool restoreColumnState(const QString &viewKey, QHeaderView *header)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QString b64 =
      s.value(QStringLiteral("dataops/cols/") + viewKey).toString();
  if (b64.isEmpty())
    return false;
  header->restoreState(QByteArray::fromBase64(b64.toUtf8()));
  return true;
}

// 列配置对话框（显隐 + 顺序上下移 + 宽重置）。
class ColumnConfigDialog : public QDialog
{
  Q_OBJECT
public:
  ColumnConfigDialog(const QStringList &labels, const QList<bool> &visible,
                     QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("columnConfigDialog"));
    setWindowTitle(tr("配置列"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("勾选显示列；上下移调整顺序"), this);
    lay->addWidget(hint);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("columnConfigList"));
    for (int i = 0; i < labels.size(); ++i)
    {
      auto *it = new QListWidgetItem(labels.at(i), m_list);
      it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable |
                   Qt::ItemIsDragEnabled);
      it->setCheckState(visible.value(i, true) ? Qt::Checked : Qt::Unchecked);
      it->setData(Qt::UserRole, i); // 原逻辑列号
    }
    lay->addWidget(m_list, 1);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    auto *up = new QPushButton(tr("上移"), row);
    up->setObjectName(QStringLiteral("columnUpButton"));
    auto *down = new QPushButton(tr("下移"), row);
    down->setObjectName(QStringLiteral("columnDownButton"));
    connect(up, &QPushButton::clicked, this, [this] { moveRow(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveRow(+1); });
    rl->addWidget(up);
    rl->addWidget(down);
    rl->addStretch(1);
    lay->addWidget(row);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  // 返回 (逻辑列号, visible) 有序对——顺序 = 列表行序。
  QList<QPair<int, bool>> result() const
  {
    QList<QPair<int, bool>> out;
    for (int i = 0; i < m_list->count(); ++i)
    {
      QListWidgetItem *it = m_list->item(i);
      out.append({it->data(Qt::UserRole).toInt(),
                  it->checkState() == Qt::Checked});
    }
    return out;
  }

private:
  void moveRow(int delta)
  {
    const int row = m_list->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_list->count())
      return;
    QListWidgetItem *it = m_list->takeItem(row);
    m_list->insertItem(to, it);
    m_list->setCurrentRow(to);
  }
  QListWidget *m_list = nullptr;
};

// ---- D7.8 列内联过滤（excel 风列头漏斗）--------------------------------------
// 列头右键 → 该列的取值清单（勾选保留）；QTableWidget 形态按 setRowHidden
// 落地（与既有 applyListFilter 同一机制，条件并入 FilterGroup 维度之外
// 的「列面」——这里独立保存，应用时 AND）。
class ColumnFunnel
{
public:
  // 打开漏斗菜单（在 header 上）；返回是否应用了新过滤。
  static bool execForColumn(QTableWidget *table, int column, QWidget *parent)
  {
    if (!table || column < 0)
      return false;
    // 收集该列去重值（跳过空态指引行）。
    QStringList values;
    for (int r = 0; r < table->rowCount(); ++r)
    {
      const QTableWidgetItem *it = table->item(r, column);
      if (!it || it->data(Qt::UserRole).toString().isEmpty())
        continue;
      if (!values.contains(it->text()))
        values << it->text();
    }
    values.sort();
    QMenu menu(parent);
    menu.setObjectName(QStringLiteral("columnFunnelMenu"));
    menu.setWindowTitle(QObject::tr("列过滤"));
    QList<QAction *> acts;
    for (const QString &v : values)
    {
      QAction *a = menu.addAction(v);
      a->setCheckable(true);
      a->setChecked(true);
      acts.append(a);
    }
    if (acts.isEmpty())
      return false;
    menu.exec(QCursor::pos());
    // 应用：未勾选的值 → 隐藏对应行。
    QSet<QString> keep;
    for (QAction *a : acts)
      if (a->isChecked())
        keep.insert(a->text());
    for (int r = 0; r < table->rowCount(); ++r)
    {
      const QTableWidgetItem *it = table->item(r, column);
      if (!it || it->data(Qt::UserRole).toString().isEmpty())
        continue;
      table->setRowHidden(r, !keep.contains(it->text()));
    }
    return true;
  }
};

} // namespace paleo::dataops
