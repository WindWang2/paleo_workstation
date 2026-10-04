// 层：视图
#include "storagegovernancedialog.h"
#include "../paleotheme.h"
#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

namespace {
// Value-backed table: Qt paints only visible rows, no per-record widgets/items.
class RowsModel final : public QAbstractTableModel {
public:
  QStringList headers;
  QVector<QStringList> rows;
  QStringList identities;
  explicit RowsModel(QObject *parent) : QAbstractTableModel(parent) {}
  int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : rows.size(); }
  int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : headers.size(); }
  QVariant data(const QModelIndex &i, int role) const override {
    if (!i.isValid() || i.row() >= rows.size()) return {};
    if (role == Qt::FontRole && headers.value(i.column()) == StorageGovernanceDialog::tr("体积")) return PaleoTheme::monoFont();
    if (role == Qt::UserRole) return identities.value(i.row());
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return rows.at(i.row()).value(i.column());
    return {};
  }
  QVariant headerData(int n, Qt::Orientation o, int role) const override {
    return o == Qt::Horizontal && role == Qt::DisplayRole ? QVariant(headers.value(n)) : QVariant();
  }
};
QTableView *table(QWidget *parent, const char *name, const QString &accessible) {
  auto *t = new QTableView(parent); t->setObjectName(QLatin1String(name));
  t->setAccessibleName(accessible);
  t->setSelectionBehavior(QAbstractItemView::SelectRows);
  t->setSelectionMode(QAbstractItemView::ExtendedSelection);
  t->setEditTriggers(QAbstractItemView::NoEditTriggers);
  t->verticalHeader()->hide();
  t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
  t->horizontalHeader()->setStretchLastSection(true);
  return t;
}
void fill(QTableView *view, const QStringList &headers, const QVector<QStringList> &rows,
          const QStringList &ids = {}) {
  auto *model = new RowsModel(view); model->headers = headers; model->rows = rows; model->identities = ids;
  auto *previous = view->model(); view->setModel(model); if (previous) previous->deleteLater();
  view->horizontalHeader()->setStretchLastSection(headers.size() >= 4);
  view->horizontalHeader()->setSectionResizeMode(0, headers.size() >= 4 ? QHeaderView::Interactive : QHeaderView::Stretch);
  if (headers.size() >= 4) { view->setColumnWidth(0, 200); view->setColumnWidth(1, 160); }
}
QString bytes(qint64 n) {
  return n < 0 ? StorageGovernanceDialog::tr("未知") : StorageGovernanceDialog::tr("%1 B").arg(n);
}
QStringList selected(QTableView *view) {
  QStringList ids;
  if (view->selectionModel())
    for (const auto &i : view->selectionModel()->selectedRows()) ids << i.data(Qt::UserRole).toString();
  return ids;
}
}
StorageGovernanceDialog::StorageGovernanceDialog(QWidget *parent) : QDialog(parent) {
  setObjectName(QStringLiteral("storageGovernanceDialog"));
  setWindowTitle(tr("存储治理台")); setAccessibleName(windowTitle());
  setWindowModality(Qt::WindowModal); resize(900, 560);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd,
                            PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  m_summary = new QLabel(tr("尚未扫描；受管目录覆盖与 SHA 复验分别报告。"), this);
  m_summary->setObjectName(QStringLiteral("storageSummary")); m_summary->setAccessibleName(tr("存储汇总"));
  m_summary->setWordWrap(true); layout->addWidget(m_summary);
  m_tabs = new QTabWidget(this); m_tabs->setObjectName(QStringLiteral("storageTabs"));
  m_tabs->setAccessibleName(tr("存储治理分类"));
  m_entities = table(m_tabs, "storageByEntity", tr("按实体汇总体积"));
  m_types = table(m_tabs, "storageByType", tr("按类型汇总体积"));
  m_orphans = table(m_tabs, "storageOrphans", tr("未引用文件清单"));
  m_stale = table(m_tabs, "storageStale", tr("过时衍生版本清单"));
  m_tabs->addTab(m_entities, tr("按实体")); m_tabs->addTab(m_types, tr("按类型"));
  m_tabs->addTab(m_orphans, tr("未引用文件")); m_tabs->addTab(m_stale, tr("过时衍生版本"));
  layout->addWidget(m_tabs, 1);
  m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("storageStatus"));
  m_status->setAccessibleName(tr("存储治理状态")); m_status->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_status);
  m_progress = new QProgressBar(this); m_progress->setObjectName(QStringLiteral("storageProgress"));
  m_progress->setAccessibleName(tr("存储治理进度")); m_progress->hide(); layout->addWidget(m_progress);
  auto *row = new QHBoxLayout; row->setSpacing(PaleoTheme::tokens().spacingSm);
  const auto button = [this, row](const QString &text, const char *name) {
    auto *b = new QPushButton(text, this); b->setObjectName(QLatin1String(name)); b->setAccessibleName(text);
    row->addWidget(b); return b;
  };
  m_scan = button(tr("扫描存储"), "storageScanButton");
  m_preview = button(tr("预览选中项回收…"), "storagePreviewButton"); m_preview->setEnabled(false);
  m_preview->setToolTip(tr("扫描完整后，选中未引用文件或过时版本，再查看数量、体积与影响版本。"));
  m_sha = button(tr("复验外链 SHA"), "storageShaButton");
  m_cancel = button(tr("取消"), "storageCancelButton"); m_cancel->setEnabled(false);
  row->addStretch(1);
  auto *close = button(tr("关闭"), "storageCloseButton"); layout->addLayout(row);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_scan, &QPushButton::clicked, this, &StorageGovernanceDialog::scanRequested);
  connect(m_cancel, &QPushButton::clicked, this, &StorageGovernanceDialog::cancelRequested);
  connect(m_sha, &QPushButton::clicked, this, &StorageGovernanceDialog::verifyShaRequested);
  connect(m_preview, &QPushButton::clicked, this, [this] {
    emit previewRequested(selected(m_stale), selected(m_orphans));
  });
}
void StorageGovernanceDialog::setReport(const paleo::storage::Report &r) {
  QVector<QStringList> rows;
  QHash<QString, QString> names;
  for (const auto &e : r.source.entities) names.insert(e.id, e.name);
  auto keys = r.bytesByEntity.keys(); keys.sort();
  for (const auto &id : keys) rows.append({id.isEmpty() ? tr("未挂接实体") : names.value(id, id), id, bytes(r.bytesByEntity.value(id))});
  fill(m_entities, {tr("实体"), tr("身份"), tr("体积")}, rows); rows.clear();
  keys = r.bytesByType.keys(); keys.sort();
  for (const auto &type : keys) rows.append({type, bytes(r.bytesByType.value(type))});
  fill(m_types, {tr("资产类型"), tr("体积")}, rows); rows.clear();
  QStringList ids;
  for (const auto &f : r.orphans) { rows.append({f.relativePath, bytes(f.sizeBytes)}); ids << f.relativePath; }
  fill(m_orphans, {tr("未被任何版本引用的文件"), tr("体积")}, rows, ids); rows.clear(); ids.clear();
  for (const auto &v : r.stale) {
    rows.append({v.assetName, v.version.id, bytes(v.file.sizeBytes), v.reason.isEmpty() ? tr("未记录原因") : v.reason});
    ids << v.version.id;
  }
  fill(m_stale, {tr("资产"), tr("版本"), tr("体积"), tr("过时原因")}, rows, ids);
  m_summary->setText(tr("版本逻辑体积 %1；大小未知 %2 个版本。按实体去重链接计量，多实体共享会重复计入。\n"
                        "扫描范围：%3；未覆盖：%4。未引用文件仅表示没有版本引用，未判断用途。")
    .arg(bytes(r.versionBytes)).arg(r.unknownSizes).arg(r.scannedRoots.join(QStringLiteral("、")))
    .arg(r.uncovered.isEmpty() ? (r.complete ? tr("无") : tr("扫描未完成")) : r.uncovered.join(QStringLiteral("、"))));
  m_preview->setProperty("paleo.complete", r.complete); m_preview->setEnabled(r.complete);
}
void StorageGovernanceDialog::setPreview(const paleo::storage::Preview &p) {
  auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName(QStringLiteral("storageCleanupPreview"));
  dialog->setWindowTitle(tr("确认回收预览")); dialog->setAccessibleName(dialog->windowTitle());
  dialog->setWindowModality(Qt::WindowModal); dialog->resize(780, 440);
  auto *layout = new QVBoxLayout(dialog);
  layout->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd,
                            PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *summary = new QLabel(tr("未引用文件 %1 个；过时版本 %2 个；影响版本 %3 个；拟释放 %4。\n"
                                "外链源与保留版本共享的文件不会回收。确认后删除不可撤销。")
    .arg(p.orphanFiles.size()).arg(p.versionIds.size()).arg(p.affectedVersions).arg(bytes(p.bytes)), dialog);
  summary->setObjectName(QStringLiteral("storagePreviewSummary")); summary->setAccessibleName(tr("回收预览汇总"));
  summary->setWordWrap(true); layout->addWidget(summary);
  auto *view = table(dialog, "storagePreviewFiles", tr("回收预览明细"));
  QVector<QStringList> rows;
  for (const auto &f : p.files) rows.append({f.relativePath, bytes(f.sizeBytes)});
  for (const auto &id : p.versionIds) rows.append({tr("版本记录 %1").arg(id), QString()});
  fill(view, {tr("回收对象"), tr("体积")}, rows); layout->addWidget(view, 1);
  auto *blocked = new QLabel(p.blocked.join(QStringLiteral("\n")), dialog);
  blocked->setObjectName(QStringLiteral("storagePreviewBlocked")); blocked->setAccessibleName(tr("回收阻断原因"));
  blocked->setWordWrap(true); layout->addWidget(blocked);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("确认回收"));
  buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("storageConfirmButton"));
  buttons->button(QDialogButtonBox::Ok)->setAccessibleName(tr("确认回收"));
  buttons->button(QDialogButtonBox::Ok)->setEnabled(p.valid);
  buttons->button(QDialogButtonBox::Ok)->setToolTip(p.valid ? tr("按预览执行回收") : tr("预览为空、未完成或有引用阻断，不能回收"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("storagePreviewCancelButton"));
  buttons->button(QDialogButtonBox::Cancel)->setAccessibleName(tr("取消回收"));
  buttons->setObjectName(QStringLiteral("storagePreviewButtons"));
  buttons->setAccessibleName(tr("回收确认操作"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog] { dialog->accept(); emit confirmRequested(); });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(dialog, &QDialog::rejected, this, &StorageGovernanceDialog::cancelRequested);
  dialog->open();
}
void StorageGovernanceDialog::setBusy(bool busy, bool cancellable) {
  m_scan->setEnabled(!busy); m_sha->setEnabled(!busy);
  m_preview->setEnabled(!busy && m_preview->property("paleo.complete").toBool());
  m_cancel->setEnabled(busy && cancellable); m_progress->setVisible(busy);
  m_tabs->setEnabled(!busy);
}
void StorageGovernanceDialog::setProgress(int done, int total, const QString &path) {
  m_progress->setRange(0, total); m_progress->setValue(done);
  m_status->setText(tr("已处理 %1：%2").arg(done).arg(path));
}
void StorageGovernanceDialog::setMessage(const QString &text) { m_status->setText(text); }
