// 层：视图
// ui/pages/dataopspanelops — D1 多选批量操作 + 右键菜单。
//   · 批量挂接目标实体选择对话框（D1.4）
//   · 批量改类型（确认 + 失败明细）（D1.5）
//   · 可回收清单对话框（软删/恢复/清空）（D1.6）
//   · 批量导出清单 CSV/JSON（D1.7）
//   · 批量打开预览（前几项进标签，超出提示）（D1.8）
//   · 上下文菜单矩阵（D1.3：单选/多选/异构选中分流）+ 单项操作
#pragma once

#include <QCheckBox>
#include <QClipboard>
#include <QHeaderView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include "../paleotheme.h"
#include "dataops/dataopsexport.h"
#include "dataops/dataopsmodel.h"

namespace paleo::dataops
{

// ---- D1.4 目标实体选择对话框 --------------------------------------------------
// 单选（转义歧义）：井实体列表 + 搜索框；确认后返回 entityId。
class EntityPickerDialog : public QDialog
{
  Q_OBJECT
public:
  explicit EntityPickerDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("entityPickerDialog"));
    setWindowTitle(tr("选择目标实体"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("entityPickerSearch"));
    m_search->setPlaceholderText(tr("搜索实体名"));
    lay->addWidget(m_search);
    m_list = new QTableWidget(0, 2, this);
    m_list->setObjectName(QStringLiteral("entityPickerList"));
    m_list->setHorizontalHeaderLabels({tr("名称"), tr("类型")});
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->verticalHeader()->setVisible(false);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_list, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_search, &QLineEdit::textChanged, this, [this] { refilter(); });
    connect(m_list, &QTableWidget::cellDoubleClicked, this, &QDialog::accept);
    lay->addWidget(box);
  }

  // 实体全量装填（entityType 过滤 + override 显示名）。
  void loadEntities(DataCatalog *cat, const EntityOverrideStore &overrides,
                    const QString &entityType)
  {
    m_rows.clear();
    m_list->setRowCount(0);
    if (!cat)
      return;
    for (const CatalogEntity &e : cat->entities(entityType))
    {
      const QString name = overrides.displayName(e);
      const QString shown = name.isEmpty() ? e.id : name;
      const int r = m_list->rowCount();
      m_list->insertRow(r);
      auto *n = new QTableWidgetItem(shown);
      n->setData(Qt::UserRole, e.id);
      n->setFlags(n->flags() & ~Qt::ItemIsEditable);
      m_list->setItem(r, 0, n);
      auto *t = new QTableWidgetItem(e.entityType);
      t->setFlags(t->flags() & ~Qt::ItemIsEditable);
      m_list->setItem(r, 1, t);
      m_rows.append({shown, e.entityType, e.id});
    }
    if (m_list->rowCount() > 0)
      m_list->selectRow(0);
    refilter();
  }
  QString selectedEntityId() const
  {
    const QList<QTableWidgetItem *> sel = m_list->selectedItems();
    if (sel.isEmpty())
      return QString();
    return sel.front()->data(Qt::UserRole).toString();
  }

private:
  struct Row
  {
    QString name, type, id;
  };
  void refilter()
  {
    const QString needle = m_search->text().trimmed();
    for (int r = 0; r < m_list->rowCount(); ++r)
    {
      const bool hit = needle.isEmpty() ||
                       m_list->item(r, 0)->text().contains(needle, Qt::CaseInsensitive) ||
                       m_list->item(r, 1)->text().contains(needle, Qt::CaseInsensitive);
      m_list->setRowHidden(r, !hit);
    }
  }
  QLineEdit *m_search = nullptr;
  QTableWidget *m_list = nullptr;
  QVector<Row> m_rows;
};

// ---- D1.5 批量改类型（确认 + 失败明细）----------------------------------------
class BatchTypeDialog : public QDialog
{
  Q_OBJECT
public:
  explicit BatchTypeDialog(const QStringList &assetNames, const QStringList &typeVocab,
                           QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("batchTypeDialog"));
    setWindowTitle(tr("批量改类型"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    auto *head = new QLabel(tr("将 %1 个资产的类型改为：").arg(assetNames.size()), this);
    lay->addWidget(head);
    m_names = new QLabel(assetNames.join(QStringLiteral("、")), this);
    m_names->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(m_names,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_names);
    m_combo = new QComboBox(this);
    m_combo->setObjectName(QStringLiteral("batchTypeCombo"));
    m_combo->addItems(typeVocab);
    lay->addWidget(m_combo);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(m_note,
                                      [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(m_note);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void setNote(const QString &n) { m_note->setText(n); }
  QString chosenType() const { return m_combo->currentText(); }
  int chosenIndex() const { return m_combo->currentIndex(); }

private:
  QLabel *m_names = nullptr;
  QComboBox *m_combo = nullptr;
  QLabel *m_note = nullptr;
};

// 改型失败明细对话框（D1.5 失败路径：列出未改成的资产 + 原因）。
inline void showBatchFailureDetail(QWidget *parent, const QString &title,
                                   const QStringList &failures)
{
  QMessageBox box(parent);
  box.setObjectName(QStringLiteral("batchFailureDialog"));
  box.setWindowTitle(title);
  box.setIcon(QMessageBox::Warning);
  box.setText(QObject::tr("部分操作未完成（%1 项）：").arg(failures.size()));
  box.setDetailedText(failures.join(QLatin1Char('\n')));
  box.setStandardButtons(QMessageBox::Ok);
  box.exec();
}

// ---- D1.6 可回收清单对话框 ----------------------------------------------------
class RecycleBinDialog : public QDialog
{
  Q_OBJECT
public:
  explicit RecycleBinDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("recycleBinDialog"));
    setWindowTitle(tr("可回收清单（软删资产）"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    m_table = new QTableWidget(0, 4, this);
    m_table->setObjectName(QStringLiteral("recycleBinTable"));
    m_table->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("移除时间"), tr("原因")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(m_table, 1);
    auto *row = new QWidget(this);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    m_restore = new QPushButton(tr("恢复选中"), row);
    m_restore->setObjectName(QStringLiteral("recycleRestoreButton"));
    m_restoreAll = new QPushButton(tr("全部恢复"), row);
    m_restoreAll->setObjectName(QStringLiteral("recycleRestoreAllButton"));
    m_purge = new QPushButton(tr("清空记录"), row);
    m_purge->setObjectName(QStringLiteral("recyclePurgeButton"));
    m_close = new QPushButton(tr("关闭"), row);
    rl->addWidget(m_restore);
    rl->addWidget(m_restoreAll);
    rl->addStretch(1);
    rl->addWidget(m_purge);
    rl->addWidget(m_close);
    lay->addWidget(row);
    connect(m_restore, &QPushButton::clicked, this, [this] { emit restoreRequested(selectedIds()); });
    connect(m_restoreAll, &QPushButton::clicked, this, [this] { emit restoreAllRequested(); });
    connect(m_purge, &QPushButton::clicked, this, [this] {
      // D5.5：清空记录不可撤销（历史移除时间丢失）——显式确认并说明。
      if (QMessageBox::question(this, tr("清空可回收清单"),
                                tr("清空后这些软删记录将不可恢复。\n（catalog 中的资产记录不受影响）\n继续？")) ==
          QMessageBox::Yes)
        emit purgeAllRequested();
    });
    connect(m_close, &QPushButton::clicked, this, &QDialog::reject);
  }

  void loadEntries(const QVector<RecycleEntry> &entries)
  {
    m_table->setRowCount(0);
    for (const RecycleEntry &e : entries)
    {
      const int r = m_table->rowCount();
      m_table->insertRow(r);
      auto *n = new QTableWidgetItem(e.displayName);
      n->setData(Qt::UserRole, e.assetId);
      n->setFlags(n->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 0, n);
      auto *t = new QTableWidgetItem(e.type);
      t->setFlags(t->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 1, t);
      auto *d = new QTableWidgetItem(e.removedAt.toString(QStringLiteral("MM-dd hh:mm")));
      d->setFlags(d->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 2, d);
      auto *why = new QTableWidgetItem(e.reason);
      why->setFlags(why->flags() & ~Qt::ItemIsEditable);
      m_table->setItem(r, 3, why);
    }
    m_restore->setEnabled(m_table->rowCount() > 0);
    m_restoreAll->setEnabled(m_table->rowCount() > 0);
  }
  QStringList selectedIds() const
  {
    QStringList out;
    for (QTableWidgetItem *it : m_table->selectedItems())
    {
      const QString id = it->data(Qt::UserRole).toString();
      if (!id.isEmpty() && !out.contains(id))
        out << id;
    }
    return out;
  }

signals:
  void restoreRequested(const QStringList &assetIds); // NOLINT(readability-inconsistent-declaration-parameter-name)
  void restoreAllRequested();
  void purgeAllRequested();

private:
  QTableWidget *m_table = nullptr;
  QPushButton *m_restore = nullptr;
  QPushButton *m_restoreAll = nullptr;
  QPushButton *m_purge = nullptr;
  QPushButton *m_close = nullptr;
};

// ---- D1.7 批量导出清单 ---------------------------------------------------------
// 行快照 → CSV/JSON 写文件；返回写出路径（空 = 用户取消/失败）。
inline QString exportRowsToFile(QWidget *parent, const QVector<AssetRowInfo> &rows)
{
  if (rows.isEmpty())
    return QString();
  const QString base = QStringLiteral("paleo_export_%1")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")));
  const QString csvPath = QFileDialog::getSaveFileName(
      parent, QObject::tr("导出清单（CSV）"), base + QStringLiteral(".csv"),
      QObject::tr("CSV 清单 (*.csv);;JSON 清单 (*.json)"));
  if (csvPath.isEmpty())
    return QString();
  const bool json = csvPath.endsWith(QLatin1String(".json"), Qt::CaseInsensitive);
  ExportFields f; // 全字段（路径/类型/版本/归属 + 标签/大小/时间）
  const QByteArray content =
      json ? exportJson(rows, f) : exportCsv(rows, f).toUtf8();
  if (!writeExportFile(csvPath, content))
  {
    QMessageBox::warning(parent, QObject::tr("导出失败"),
                         QObject::tr("无法写入文件：%1").arg(csvPath));
    return QString();
  }
  return csvPath;
}

// ---- D1.8 批量打开预览 ---------------------------------------------------------
// 前 maxTabs 项逐个发 assetActivated（标签由壳开）；超出 → 提示条文本。
// 返回 (openedIds, overflowCount)。
inline QPair<QStringList, int> batchOpenPreviewPlan(const QStringList &assetIds,
                                                    int maxTabs = 8)
{
  QStringList open;
  for (const QString &id : assetIds)
  {
    if (open.size() >= maxTabs)
      break;
    open << id;
  }
  return {open, assetIds.size() - open.size()};
}

// ---- D1.3 上下文菜单矩阵 -------------------------------------------------------
// 单资产 / 多资产 / 异构（资产+实体）/ 实体 四态的菜单构造。
// 返回动作 key 列表（供测试断言菜单面）；key → 调用方接线。
struct ContextMenuSpec
{
  bool hasAssets = false;
  bool hasEntities = false;
  int assetCount = 0;
  int entityCount = 0;
  bool singleAssetUnresolved = false; // 单选且该资产有未决链接
  bool singleAssetResolved = false;   // 单选且有已决链接
  bool singleAssetIsHorizon = false;  // 单选且是层位散点资产（网格化入口）
  bool hasRecycleEntries = false;
};

inline QStringList contextMenuActions(const ContextMenuSpec &spec)
{
  QStringList acts;
  if (spec.assetCount == 0 && spec.entityCount == 0)
    return acts;
  if (spec.hasAssets)
  {
    // 资产面（mixed 时给公共子集——资产操作；实体专属操作不给）。
    acts << QStringLiteral("openPreview");
    if (spec.singleAssetIsHorizon)
      acts << QStringLiteral("gridHorizon"); // goal/gridding-surface-ops：层位网格化
    if (spec.assetCount > 1)
      acts << QStringLiteral("openPreviewAll");
    if (spec.singleAssetUnresolved || spec.assetCount > 1)
      acts << QStringLiteral("attachToEntity");
    if (spec.singleAssetResolved)
      acts << QStringLiteral("detachLink") << QStringLiteral("setPrimary")
           << QStringLiteral("editRole");
    if (spec.singleAssetResolved || spec.assetCount > 1)
      acts << QStringLiteral("transferLink");
    acts << QStringLiteral("addTag") << QStringLiteral("changeType");
    if (spec.assetCount > 1)
      acts << QStringLiteral("exportManifest") << QStringLiteral("removeSoft");
    acts << QStringLiteral("showInFolder");
  }
  if (spec.hasEntities && !spec.hasAssets)
  {
    acts << QStringLiteral("renameEntity") << QStringLiteral("editCoords")
         << QStringLiteral("deleteEntity");
  }
  else if (spec.hasEntities && spec.hasAssets)
  {
    acts << QStringLiteral("focusEntities");
  }
  acts << QStringLiteral("selectAll") << QStringLiteral("invertSelection");
  if (spec.hasRecycleEntries)
    acts << QStringLiteral("showRecycleBin");
  return acts;
}

} // namespace paleo::dataops

namespace paleo::dataops {
// ---- D4.7 挂接角色编辑 ------------------------------------------------------------
// 词表来自 roleRegistry（工程自定义可扩）。应用 = 新角色链接 addLink +
// 旧链接保留（catalog 无删除/更新 API——确认对话框明示不可撤销，D5.5）。
class RoleEditDialog : public QDialog
{
  Q_OBJECT
public:
  explicit RoleEditDialog(QWidget *parent = nullptr)
    : QDialog(parent)
  {
    setObjectName(QStringLiteral("roleEditDialog"));
    setWindowTitle(tr("编辑挂接角色"));
    setModal(true);
    auto *lay = new QVBoxLayout(this);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    lay->addWidget(m_info);
    m_combo = new QComboBox(this);
    m_combo->setObjectName(QStringLiteral("roleEditCombo"));
    lay->addWidget(m_combo);
    m_warn = new QLabel(this);
    m_warn->setObjectName(QStringLiteral("roleEditWarn"));
    m_warn->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(m_warn, [] {
      return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().warningText.name().toUpper());
    });
    lay->addWidget(m_warn);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
  }
  void loadRole(const QString &role, const QStringList &vocab)
  {
    m_old = role;
    m_combo->clear();
    m_combo->addItems(vocab);
    const int idx = m_combo->findText(role);
    if (idx >= 0)
      m_combo->setCurrentIndex(idx);
    m_info->setText(tr("当前角色：%1").arg(role));
    m_warn->setText(tr("注意：角色变更会新增一条新角色关联；旧角色关联保留"
                       "（目录暂无链接删除接口）。此操作不可撤销。"));
  }
  QString oldRole() const { return m_old; }
  QString newRole() const { return m_combo->currentText(); }

private:
  QLabel *m_info = nullptr;
  QComboBox *m_combo = nullptr;
  QLabel *m_warn = nullptr;
  QString m_old;
};

} // namespace paleo::dataops
