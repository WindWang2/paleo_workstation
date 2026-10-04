// 层：视图
#include "welltopseditordialog.h"

#include "../../catalog/datacatalog.h"
#include "../../workflow/welltopseditorworkflow.h"
#include "../paleotheme.h"
#include "welltopsmergedialog.h"

#include <QBrush>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace
{
// 数值列编辑器：C locale + 有限小数；空单元格 = 缺失值（-99999 不手输）。
class NumericColumnDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &,
                        const QModelIndex &) const override
  {
    auto *edit = new QLineEdit(parent);
    auto *validator = new QDoubleValidator(-1.0e9, 1.0e9, 9, edit);
    validator->setLocale(QLocale::c());
    validator->setNotation(QDoubleValidator::StandardNotation);
    edit->setValidator(validator);
    return edit;
  }
};

QString numericCell(double v, bool has)
{
  return has ? WellTopsEdit::formatDepth(v) : QString();
}

// 单元格文本 → (值, 是否有效；空 = 缺失)。解析失败 → false。
bool parseNumericCell(const QString &text, bool *has, double *out)
{
  const QString t = text.trimmed();
  if (t.isEmpty())
  {
    *has = false;
    *out = 0.0;
    return true;
  }
  bool ok = false;
  const double v = QLocale::c().toDouble(t, &ok);
  if (!ok || !qIsFinite(v))
    return false;
  *has = true;
  *out = v;
  return true;
}

QString rowSummaryText(const WellTopRecord &r)
{
  return QStringLiteral("MD %1 · TVD %2")
      .arg(r.hasMd ? QString::number(r.md, 'f', 3) : QStringLiteral("空"),
           r.hasTvd ? QString::number(r.tvd, 'f', 3) : QStringLiteral("空"));
}

QString diffText(const WellTopsEdit::DiffSummary &d)
{
  return QStringLiteral("+%1 / −%2 / 改%3")
      .arg(QString::number(d.added), QString::number(d.removed), QString::number(d.changed));
}
} // namespace

WellTopsEditorDialog::WellTopsEditorDialog(DataCatalog *catalog, const QString &projectDir,
                                           const QString &assetId, QWidget *parent)
  : QDialog(parent)
  , m_catalog(catalog)
  , m_assetId(assetId)
  , m_workflow(std::make_unique<WellTopsEditorWorkflow>(catalog, projectDir))
{
  setObjectName(QStringLiteral("wellTopsEditorDialog"));
  setWindowTitle(tr("编辑分层"));
  setModal(true);
  resize(880, 560);
  buildUi();

  if (m_catalog)
  {
    const CatalogAsset asset = m_catalog->assetById(assetId);
    if (asset.id.isEmpty())
    {
      m_readOnly = true;
      setSummaryLine(tr("资产不存在：%1").arg(assetId));
    }
    else
      m_displayName = asset.displayName;
  }
  reloadAll();
}

WellTopsEditorDialog::~WellTopsEditorDialog() = default;

void WellTopsEditorDialog::buildUi()
{
  auto *root = new QVBoxLayout(this);

  // ---- 顶行：资产 · 井选择 · 校验 · 状态行 ----
  auto *topRow = new QHBoxLayout;
  m_assetLabel = new QLabel(this);
  m_assetLabel->setObjectName(QStringLiteral("topsAssetLabel"));
  m_assetLabel->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  topRow->addWidget(m_assetLabel);

  m_wellCombo = new QComboBox(this);
  m_wellCombo->setObjectName(QStringLiteral("topsWellCombo"));
  m_wellCombo->setAccessibleName(tr("选择要编辑的井"));
  connect(m_wellCombo, &QComboBox::currentTextChanged, this,
          &WellTopsEditorDialog::onWellChanged);
  topRow->addWidget(m_wellCombo);

  m_validateButton = new QPushButton(tr("校验"), this);
  m_validateButton->setObjectName(QStringLiteral("topsValidateButton"));
  connect(m_validateButton, &QPushButton::clicked, this, &WellTopsEditorDialog::onValidate);
  topRow->addWidget(m_validateButton);

  m_summaryLine = new QLabel(this);
  m_summaryLine->setObjectName(QStringLiteral("topsStatusLine"));
  topRow->addWidget(m_summaryLine, 1);
  root->addLayout(topRow);

  // ---- 分层表 ----
  m_table = new QTableWidget(0, ColCount, this);
  m_table->setObjectName(QStringLiteral("topsEditTable"));
  m_table->setHorizontalHeaderLabels(
      {tr("状态"), tr("层名"), tr("MD"), tr("TVD"), tr("X"), tr("Y"), tr("Z"), tr("Time(ms)")});
  m_table->verticalHeader()->hide();
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->setSelectionBehavior(QAbstractItemView::SelectItems);
  for (int c = ColMd; c < ColCount; ++c)
    m_table->setItemDelegateForColumn(c, new NumericColumnDelegate(this));
  PaleoTheme::applyDensityToViewTree(m_table);
  connect(m_table, &QTableWidget::itemChanged, this,
          [this](QTableWidgetItem *item)
          {
            if (!m_filling && item && item->column() != ColStatus)
              refreshRowStatus(item->row());
          });
  root->addWidget(m_table, 1);

  // ---- 行操作 ----
  auto *rowOps = new QHBoxLayout;
  auto *insertB = new QPushButton(tr("插入行"), this);
  insertB->setObjectName(QStringLiteral("topsInsertRowButton"));
  connect(insertB, &QPushButton::clicked, this, &WellTopsEditorDialog::onInsertRow);
  auto *deleteB = new QPushButton(tr("删除行"), this);
  deleteB->setObjectName(QStringLiteral("topsDeleteRowButton"));
  connect(deleteB, &QPushButton::clicked, this, &WellTopsEditorDialog::onDeleteRow);
  auto *upB = new QPushButton(tr("上移"), this);
  upB->setObjectName(QStringLiteral("topsMoveUpButton"));
  connect(upB, &QPushButton::clicked, this, [this] { onMoveRow(-1); });
  auto *downB = new QPushButton(tr("下移"), this);
  downB->setObjectName(QStringLiteral("topsMoveDownButton"));
  connect(downB, &QPushButton::clicked, this, [this] { onMoveRow(1); });
  auto *sortB = new QPushButton(tr("按深度排序"), this);
  sortB->setObjectName(QStringLiteral("topsSortButton"));
  sortB->setToolTip(tr("按 MD 从浅到深稳定排序（无 MD 的行排末尾）"));
  connect(sortB, &QPushButton::clicked, this, &WellTopsEditorDialog::onSortByDepth);
  QWidget *const rowOpWidgets[] = {insertB, deleteB, upB, downB, sortB};
  for (QWidget *w : rowOpWidgets)
    rowOps->addWidget(w);
  rowOps->addStretch(1);
  root->addLayout(rowOps);

  // ---- 校验结果 ----
  m_issueTitle = new QLabel(this);
  m_issueTitle->setObjectName(QStringLiteral("topsIssueTitle"));
  m_issueTitle->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  m_issueTitle->hide();
  root->addWidget(m_issueTitle);
  m_issueList = new QListWidget(this);
  m_issueList->setObjectName(QStringLiteral("topsIssueList"));
  m_issueList->setMaximumHeight(120);
  m_issueList->hide();
  connect(m_issueList, &QListWidget::itemClicked, this,
          [this](QListWidgetItem *item)
          {
            const int row = item->data(Qt::UserRole).toInt();
            if (row >= 0 && row < m_table->rowCount())
            {
              m_table->selectRow(row);
              m_table->setCurrentCell(row, ColTopName);
            }
          });
  root->addWidget(m_issueList);

  // ---- 底行：合并 / 批量 / 版本 · 保存 / 关闭 ----
  auto *bottom = new QHBoxLayout;
  m_mergeButton = new QPushButton(tr("从文件合并…"), this);
  m_mergeButton->setObjectName(QStringLiteral("topsMergeButton"));
  m_mergeButton->setToolTip(tr("再导入同井分层文件：逐行对比取舍，不默认覆盖"));
  connect(m_mergeButton, &QPushButton::clicked, this, &WellTopsEditorDialog::onMergeFromFile);
  bottom->addWidget(m_mergeButton);

  m_batchButton = new QToolButton(this);
  m_batchButton->setObjectName(QStringLiteral("topsBatchButton"));
  m_batchButton->setText(tr("批量修正"));
  m_batchButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_batchButton->setPopupMode(QToolButton::InstantPopup);
  auto *batchMenu = new QMenu(m_batchButton);
  QAction *renameAct = batchMenu->addAction(tr("层名重命名/统一…"));
  renameAct->setObjectName(QStringLiteral("topsBatchRenameAction"));
  QAction *shiftAct = batchMenu->addAction(tr("按基准整体位移…"));
  shiftAct->setObjectName(QStringLiteral("topsBatchShiftAction"));
  QAction *deleteAct = batchMenu->addAction(tr("按层删除…"));
  deleteAct->setObjectName(QStringLiteral("topsBatchDeleteAction"));
  connect(renameAct, &QAction::triggered, this, &WellTopsEditorDialog::onBatchRename);
  connect(shiftAct, &QAction::triggered, this, &WellTopsEditorDialog::onBatchShift);
  connect(deleteAct, &QAction::triggered, this, &WellTopsEditorDialog::onBatchDelete);
  m_batchButton->setMenu(batchMenu);
  bottom->addWidget(m_batchButton);

  m_versionButton = new QToolButton(this);
  m_versionButton->setObjectName(QStringLiteral("topsVersionButton"));
  m_versionButton->setText(tr("版本/回滚"));
  m_versionButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_versionButton->setPopupMode(QToolButton::InstantPopup);
  m_versionButton->setMenu(new QMenu(m_versionButton)); // 单实例，rebuildVersionMenu 原地重填
  bottom->addWidget(m_versionButton);

  bottom->addStretch(1);

  m_saveButton = new QPushButton(tr("保存（新版本）"), this);
  m_saveButton->setObjectName(QStringLiteral("topsSaveButton"));
  m_saveButton->setToolTip(tr("原子提交为该资产的新 DERIVED 版本；校验错误未清零时阻断"));
  connect(m_saveButton, &QPushButton::clicked, this, &WellTopsEditorDialog::onSave);
  bottom->addWidget(m_saveButton);
  auto *closeB = new QPushButton(tr("关闭"), this);
  closeB->setObjectName(QStringLiteral("topsCloseButton"));
  connect(closeB, &QPushButton::clicked, this, [this] { reject(); });
  bottom->addWidget(closeB);
  root->addLayout(bottom);
}

void WellTopsEditorDialog::reloadAll()
{
  m_filling = true;
  QVector<WellTopRecord> all;
  QString err;
  if (m_workflow && m_workflow->loadAllRows(m_assetId, &all, &err))
    m_allRows = all;
  else
  {
    m_allRows.clear();
    setSummaryLine(err);
  }

  const QString keepWell = m_wellCombo->currentText();
  m_wellCombo->blockSignals(true);
  m_wellCombo->clear();
  m_wellCombo->addItems(WellTopsEditorWorkflow::wellNamesIn(m_allRows));
  if (!keepWell.isEmpty())
  {
    const int idx = m_wellCombo->findText(keepWell);
    if (idx >= 0)
      m_wellCombo->setCurrentIndex(idx);
  }
  m_wellCombo->blockSignals(false);

  m_assetLabel->setText(tr("%1 · %2 行").arg(m_displayName.isEmpty() ? m_assetId : m_displayName,
                                             QString::number(m_allRows.size())));
  m_currentWell = m_wellCombo->currentText();
  m_baseline = WellTopsEditorWorkflow::rowsForWell(m_allRows, m_wellCombo->currentText());
  fillTableFrom(m_baseline);
  m_filling = false;
  rebuildVersionMenu();

  if (!m_readOnly && m_catalog && m_catalog->refusesWrites())
    m_readOnly = true;
  if (m_readOnly)
  {
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_saveButton->setEnabled(false);
    m_mergeButton->setEnabled(false);
    m_batchButton->setEnabled(false);
    if (m_catalog) // 资产缺失场景 reloadAll 已在 setSummaryLine 给过原因
      setSummaryLine(tr("catalog 只读——分层仅可查看"));
  }
}

void WellTopsEditorDialog::reject()
{
  if (m_confirmingReject)
  {
    QDialog::reject();
    return;
  }
  m_confirmingReject = true;
  if (confirmDiscard())
    QDialog::reject();
  m_confirmingReject = false;
}

void WellTopsEditorDialog::fillTableFrom(const QVector<WellTopRecord> &rows)
{
  m_table->setRowCount(0);
  for (const WellTopRecord &r : rows)
  {
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    auto *status = new QTableWidgetItem;
    status->setFlags(status->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(row, ColStatus, status);
    m_table->setItem(row, ColTopName, new QTableWidgetItem(r.topName));
    m_table->setItem(row, ColMd, new QTableWidgetItem(numericCell(r.md, r.hasMd)));
    m_table->setItem(row, ColTvd, new QTableWidgetItem(numericCell(r.tvd, r.hasTvd)));
    m_table->setItem(row, ColX, new QTableWidgetItem(numericCell(r.x, r.hasX)));
    m_table->setItem(row, ColY, new QTableWidgetItem(numericCell(r.y, r.hasY)));
    // Z 无独立判空标志：与 X/Y 同列组语义——X/Y 任一缺失时不显示（写侧同口径）。
    m_table->setItem(row, ColZ,
                     new QTableWidgetItem(numericCell(r.z, r.hasX && r.hasY)));
    m_table->setItem(row, ColTime, new QTableWidgetItem(numericCell(r.timeMs, r.hasTime)));
    for (int c = ColMd; c < ColCount; ++c)
      if (QTableWidgetItem *it = m_table->item(row, c))
      {
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        it->setFont(PaleoTheme::monoFont());
      }
    refreshRowStatus(row);
  }
}

QVector<QStringList> WellTopsEditorDialog::tableTexts() const
{
  QVector<QStringList> out;
  for (int r = 0; r < m_table->rowCount(); ++r)
  {
    QStringList rowTexts;
    for (int c = 0; c < ColCount; ++c)
      rowTexts.append(m_table->item(r, c) ? m_table->item(r, c)->text() : QString());
    out.append(rowTexts);
  }
  return out;
}

void WellTopsEditorDialog::applyTableTexts(const QVector<QStringList> &texts)
{
  m_filling = true;
  for (int r = 0; r < texts.size() && r < m_table->rowCount(); ++r)
    for (int c = 0; c < ColCount; ++c)
      if (QTableWidgetItem *it = m_table->item(r, c))
        it->setText(texts.at(r).at(c));
  m_filling = false;
  for (int r = 0; r < m_table->rowCount(); ++r)
    refreshRowStatus(r);
}

QVector<WellTopRecord> WellTopsEditorDialog::collectRows(QString *error) const
{
  QVector<WellTopRecord> out;
  const QString well = m_wellCombo->currentText();
  for (int row = 0; row < m_table->rowCount(); ++row)
  {
    WellTopRecord r;
    r.wellName = well;
    r.topName =
        m_table->item(row, ColTopName) ? m_table->item(row, ColTopName)->text().trimmed() : QString();
    bool dummyHas = false; // Z 无判空标志——空单元格=0，写侧以 X/Y 组决定出列
    const struct
    {
      int col;
      double *v;
      bool *has;
    } numericCols[] = {
        {ColMd, &r.md, &r.hasMd}, {ColTvd, &r.tvd, &r.hasTvd}, {ColX, &r.x, &r.hasX},
        {ColY, &r.y, &r.hasY},    {ColZ, &r.z, &dummyHas},     {ColTime, &r.timeMs, &r.hasTime},
    };
    for (const auto &c : numericCols)
    {
      const QString text =
          m_table->item(row, c.col) ? m_table->item(row, c.col)->text() : QString();
      if (!parseNumericCell(text, c.has, c.v))
      {
        if (error)
          *error = tr("第 %1 行「%2」列不是合法数值：%3")
                       .arg(QString::number(row + 1),
                            m_table->horizontalHeaderItem(c.col) ? m_table->horizontalHeaderItem(c.col)->text()
                                                                 : QString::number(c.col),
                            text);
        return QVector<WellTopRecord>();
      }
    }
    out.append(r);
  }
  return out;
}

bool WellTopsEditorDialog::isDirty() const
{
  QString err;
  const QVector<WellTopRecord> current = collectRows(&err);
  if (!err.isEmpty())
    return true; // 收集失败（编辑中间态）按有改动处理——不丢用户输入
  if (current.size() != m_baseline.size())
    return true;
  for (int i = 0; i < current.size(); ++i)
    if (!WellTopsEdit::sameTop(current.at(i), m_baseline.at(i)))
      return true;
  return false;
}

// 批量/回滚/合并都基于「最近保存的版本」计算——表内有未保存改动时先要求
// 用户落定（保存或放弃），不静默丢弃（诚实面）。
bool WellTopsEditorDialog::requireCleanTable(const QString &what)
{
  if (!isDirty())
    return true;
  QMessageBox::information(this, tr("先落定当前改动"),
                           tr("%1基于最近保存的版本计算。当前井的编辑表有未保存改动——"
                              "请先保存，或切换井时选择放弃。")
                               .arg(what));
  return false;
}

bool WellTopsEditorDialog::confirmDiscard()
{
  if (!isDirty())
    return true;
  return QMessageBox::question(this, tr("未保存的改动"),
                               tr("当前井分层有未保存改动，放弃并继续？"),
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No) == QMessageBox::Yes;
}

void WellTopsEditorDialog::onWellChanged()
{
  if (m_filling)
    return;
  if (!confirmDiscard())
  {
    // 回退到编辑表正在呈现的井（m_currentWell 是 combo 显示形——文件里的
    // 原始拼写可能与 trimmed 首见形不同，且空基线井取不到拼写）。
    m_wellCombo->blockSignals(true);
    const int idx = m_wellCombo->findText(m_currentWell);
    if (idx >= 0)
      m_wellCombo->setCurrentIndex(idx);
    m_wellCombo->blockSignals(false);
    return;
  }
  m_filling = true;
  m_currentWell = m_wellCombo->currentText();
  m_pendingMergeNote.clear(); // 换井即放弃未保存的合并溯源
  m_baseline = WellTopsEditorWorkflow::rowsForWell(m_allRows, m_currentWell);
  fillTableFrom(m_baseline);
  m_issueList->hide();
  m_issueTitle->hide();
  m_filling = false;
  setSummaryLine(tr("已切换到 %1（%2 行）").arg(m_currentWell,
                                               QString::number(m_table->rowCount())));
}


void WellTopsEditorDialog::onInsertRow()
{
  int row = m_table->currentRow() + 1;
  if (row <= 0)
    row = m_table->rowCount();
  m_table->insertRow(row);
  auto *status = new QTableWidgetItem;
  status->setFlags(status->flags() & ~Qt::ItemIsEditable);
  m_table->setItem(row, ColStatus, status);
  m_table->setItem(row, ColTopName, new QTableWidgetItem);
  for (int c = ColMd; c < ColCount; ++c)
  {
    auto *it = new QTableWidgetItem;
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    it->setFont(PaleoTheme::monoFont());
    m_table->setItem(row, c, it);
  }
  refreshRowStatus(row);
  m_table->setCurrentCell(row, ColTopName);
}

void WellTopsEditorDialog::onDeleteRow()
{
  const int row = m_table->currentRow();
  if (row < 0)
    return;
  m_table->removeRow(row);
  for (int r = 0; r < m_table->rowCount(); ++r)
    refreshRowStatus(r);
}

void WellTopsEditorDialog::onMoveRow(int delta)
{
  const int row = m_table->currentRow();
  const int target = row + delta;
  if (row < 0 || target < 0 || target >= m_table->rowCount())
    return;
  QVector<QStringList> reordered = tableTexts();
  reordered.swapItemsAt(row, target);
  applyTableTexts(reordered);
  m_table->setCurrentCell(target, ColTopName);
}

void WellTopsEditorDialog::onSortByDepth()
{
  QVector<QPair<double, int>> keyed; // (MD, 原行号)；无 MD = +inf 排末尾
  for (int r = 0; r < m_table->rowCount(); ++r)
  {
    bool has = false;
    double md = 0;
    parseNumericCell(m_table->item(r, ColMd) ? m_table->item(r, ColMd)->text() : QString(), &has,
                     &md);
    keyed.append({has ? md : std::numeric_limits<double>::infinity(), r});
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  const QVector<QStringList> texts = tableTexts();
  QVector<QStringList> reordered;
  reordered.reserve(texts.size());
  for (const auto &k : keyed)
    reordered.append(texts.at(k.second));
  applyTableTexts(reordered);
}

void WellTopsEditorDialog::refreshRowStatus(int row)
{
  QTableWidgetItem *status = m_table->item(row, ColStatus);
  if (!status)
    return;
  // 行记录现算（只读这一行的单元格）。
  WellTopRecord current;
  current.wellName = m_wellCombo->currentText();
  current.topName = m_table->item(row, ColTopName)
                        ? m_table->item(row, ColTopName)->text().trimmed()
                        : QString();
  bool badNumber = false;
  bool dummyHas = false;
  const struct
  {
    int col;
    double *v;
    bool *has;
  } numericCols[] = {
      {ColMd, &current.md, &current.hasMd},     {ColTvd, &current.tvd, &current.hasTvd},
      {ColX, &current.x, &current.hasX},        {ColY, &current.y, &current.hasY},
      {ColZ, &current.z, &dummyHas},            {ColTime, &current.timeMs, &current.hasTime},
  };
  for (const auto &c : numericCols)
  {
    const QString text = m_table->item(row, c.col) ? m_table->item(row, c.col)->text() : QString();
    if (!parseNumericCell(text, c.has, c.v))
      badNumber = true;
  }

  const WellTopRecord *baselineRec = nullptr;
  if (!current.topName.isEmpty())
    for (const WellTopRecord &b : m_baseline)
      if (b.topName.trimmed().toUpper() == current.topName.trimmed().toUpper())
      {
        baselineRec = &b;
        break;
      }

  const auto &tk = PaleoTheme::tokens();
  m_filling = true; // 改状态单元格不再触发 itemChanged
  if (badNumber || !baselineRec || !WellTopsEdit::sameTop(*baselineRec, current))
  {
    const bool isNew = !baselineRec && !badNumber;
    status->setText(isNew ? tr("新行") : tr("已改"));
    status->setForeground(QBrush(tk.warningText));
    status->setBackground(QBrush(tk.warningBg));
    status->setToolTip(badNumber ? tr("数值列有非法输入") : QString());
  }
  else
  {
    status->setText(QStringLiteral("—"));
    status->setForeground(QBrush(tk.textMuted));
    status->setBackground(QBrush());
    status->setToolTip(QString());
  }
  m_filling = false;
}

void WellTopsEditorDialog::onValidate()
{
  QString err;
  const QVector<WellTopRecord> rows = collectRows(&err);
  if (!err.isEmpty())
  {
    setSummaryLine(err);
    return;
  }
  const WellTopsEdit::ValidationContext ctx =
      WellTopsEditorWorkflow::contextFor(m_catalog, m_wellCombo->currentText());
  showIssues(WellTopsEdit::validate(rows, ctx));
}

void WellTopsEditorDialog::showIssues(const QVector<WellTopsEdit::Issue> &issues)
{
  int errors = 0, warnings = 0;
  m_issueList->clear();
  const auto &tk = PaleoTheme::tokens();
  for (const WellTopsEdit::Issue &issue : issues)
  {
    auto *item = new QListWidgetItem(
        QStringLiteral("[%1] %2").arg(WellTopsEdit::issueKindLabel(issue.kind), issue.message));
    item->setData(Qt::UserRole, issue.rowIndex);
    item->setForeground(QBrush(issue.isError() ? tk.errorText : tk.warningText));
    if (issue.isError())
      ++errors;
    else
      ++warnings;
    m_issueList->addItem(item);
  }
  m_issueTitle->setText(tr("校验结果：%1 个错误 · %2 个警告（点击条目定位行）")
                            .arg(QString::number(errors), QString::number(warnings)));
  m_issueTitle->setVisible(true);
  m_issueList->setVisible(true);
  if (errors > 0)
    setSummaryLine(tr("校验未通过：%1 个错误——保存被阻断").arg(QString::number(errors)));
  else if (warnings > 0)
    setSummaryLine(tr("校验通过（%1 个警告放行）").arg(QString::number(warnings)));
  else
    setSummaryLine(tr("校验通过：无问题"));
}

void WellTopsEditorDialog::onSave()
{
  QString err;
  const QVector<WellTopRecord> rows = collectRows(&err);
  if (!err.isEmpty())
  {
    QMessageBox::warning(this, tr("无法保存"), err);
    return;
  }
  const WellTopsEdit::ValidationContext ctx =
      WellTopsEditorWorkflow::contextFor(m_catalog, m_wellCombo->currentText());
  const QVector<WellTopsEdit::Issue> issues = WellTopsEdit::validate(rows, ctx);
  int errors = 0;
  for (const WellTopsEdit::Issue &i : issues)
    if (i.isError())
      ++errors;
  if (errors > 0)
  {
    showIssues(issues);
    QMessageBox::warning(
        this, tr("无法保存"),
        tr("存在 %1 个校验错误，修正后才能保存（见校验结果列表）。").arg(QString::number(errors)));
    return;
  }

  const QString editKind = m_pendingMergeNote.isEmpty() ? QString() : QStringLiteral("merge");
  const auto out = m_workflow->commitWellRows(m_assetId, m_wellCombo->currentText(), rows,
                                              m_pendingMergeNote, editKind);
  if (!out.ok)
  {
    QMessageBox::warning(this, tr("保存失败"), out.error);
    return;
  }
  m_pendingMergeNote.clear();
  if (out.unchanged)
  {
    setSummaryLine(tr("内容与当前版本一致——未产生新版本"));
    return;
  }
  setSummaryLine(tr("已保存为 v%1（%2）").arg(QString::number(out.newVersionNumber),
                                              diffText(out.diff)));
  reloadAll();
}

void WellTopsEditorDialog::onMergeFromFile()
{
  if (!requireCleanTable(tr("合并")))
    return;
  const QString path = QFileDialog::getOpenFileName(
      this, tr("选择要合并的分层文件"), QString(), tr("分层文件 (*.dat);;所有文件 (*)"));
  if (path.isEmpty())
    return;
  QVector<WellTopsEdit::MergeRow> mergeRows;
  QString err;
  if (!m_workflow->loadMergeRows(m_assetId, m_wellCombo->currentText(), path, &mergeRows, &err))
  {
    QMessageBox::warning(this, tr("无法合并"), err);
    return;
  }
  WellTopsMergeDialog dlg(mergeRows, this);
  if (dlg.exec() != QDialog::Accepted)
    return;

  // 合并结果进编辑表待审——保存才落库（诚实面：不默认覆盖）。
  const QVector<WellTopRecord> merged = WellTopsEdit::applyMerge(dlg.resolvedRows());
  m_filling = true;
  fillTableFrom(merged);
  m_filling = false;
  const WellTopsEdit::DiffSummary d = WellTopsEdit::diff(m_baseline, merged);
  m_pendingMergeNote = tr("合并自 %1：%2").arg(QDir::toNativeSeparators(path), diffText(d));
  setSummaryLine(tr("合并结果已进表（%1）——检查后点「保存（新版本）」").arg(diffText(d)));
}

bool WellTopsEditorDialog::commitBatch(QVector<WellTopRecord> rows, const QString &editKind,
                                       const QString &confirmText)
{
  if (!requireCleanTable(tr("批量修正")))
    return false;
  // 与单井保存同口径：批量结果先过校验器（错误级阻断；警告放行）。
  {
    const QStringList errors = WellTopsEditorWorkflow::validateAllWells(m_catalog, rows);
    if (!errors.isEmpty())
    {
      const int kMaxShown = 20;
      QStringList shown = errors.mid(0, kMaxShown);
      if (errors.size() > kMaxShown)
        shown << tr("……共 %1 个错误").arg(QString::number(errors.size()));
      QMessageBox::warning(this, tr("批量修正被校验阻断"),
                           tr("批量结果存在错误级问题，未提交：\n%1")
                               .arg(shown.join(QLatin1Char('\n'))));
      return false;
    }
  }
  if (QMessageBox::question(this, tr("批量修正确认"), confirmText, QMessageBox::Ok |
                          QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
    return false;
  const auto out = m_workflow->commitAllRows(m_assetId, rows, editKind, QString());
  if (!out.ok)
  {
    QMessageBox::warning(this, tr("批量修正失败"), out.error);
    return false;
  }
  if (out.unchanged)
  {
    setSummaryLine(tr("批量操作无实际变化——未产生新版本"));
    return true;
  }
  setSummaryLine(tr("已提交 v%1（%2）").arg(QString::number(out.newVersionNumber), diffText(out.diff)));
  reloadAll();
  return true;
}

void WellTopsEditorDialog::onBatchRename()
{
  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("topsBatchRenameDialog"));
  dlg.setWindowTitle(tr("层名重命名/统一（全文件所有井）"));
  auto *lay = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout;
  auto *oldEdit = new QLineEdit(&dlg);
  oldEdit->setObjectName(QStringLiteral("topsRenameOldEdit"));
  auto *newEdit = new QLineEdit(&dlg);
  newEdit->setObjectName(QStringLiteral("topsRenameNewEdit"));
  form->addRow(tr("旧层名"), oldEdit);
  form->addRow(tr("新层名"), newEdit);
  lay->addLayout(form);
  auto *preview = new QLabel(&dlg);
  preview->setObjectName(QStringLiteral("topsBatchPreviewLabel"));
  preview->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  lay->addWidget(preview);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  const auto updatePreview = [&]
  {
    QVector<WellTopRecord> copy = m_allRows;
    const int n = WellTopsEdit::applyRename(&copy, oldEdit->text(), newEdit->text());
    preview->setText(tr("将影响 %1 行").arg(QString::number(n)));
  };
  connect(oldEdit, &QLineEdit::textChanged, this, updatePreview);
  connect(newEdit, &QLineEdit::textChanged, this, updatePreview);
  updatePreview();
  if (dlg.exec() != QDialog::Accepted)
    return;
  QVector<WellTopRecord> copy = m_allRows;
  const int n = WellTopsEdit::applyRename(&copy, oldEdit->text(), newEdit->text());
  if (n == 0)
  {
    setSummaryLine(tr("没有匹配「%1」的层名——未提交").arg(oldEdit->text()));
    return;
  }
  commitBatch(std::move(copy), QStringLiteral("batch-rename"),
              tr("把全文件 %1 行层名「%2」统一为「%3」，提交为新版本？")
                  .arg(QString::number(n), oldEdit->text(), newEdit->text()));
}

void WellTopsEditorDialog::onBatchShift()
{
  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("topsBatchShiftDialog"));
  dlg.setWindowTitle(tr("按基准整体位移（全文件所有井）"));
  auto *lay = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout;
  auto *spin = new QDoubleSpinBox(&dlg);
  spin->setObjectName(QStringLiteral("topsShiftSpin"));
  spin->setRange(-99999.0, 99999.0);
  spin->setDecimals(3);
  spin->setSuffix(tr(" m"));
  spin->setValue(0.0);
  form->addRow(tr("位移量（MD/TVD/Z 同加）"), spin);
  lay->addLayout(form);
  auto *preview = new QLabel(&dlg);
  preview->setObjectName(QStringLiteral("topsBatchPreviewLabel"));
  preview->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  lay->addWidget(preview);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  const auto updatePreview = [&]
  {
    QVector<WellTopRecord> copy = m_allRows;
    const int n = WellTopsEdit::applyShift(&copy, spin->value());
    preview->setText(tr("将影响 %1 行").arg(QString::number(n)));
  };
  connect(spin, &QDoubleSpinBox::valueChanged, this, updatePreview);
  updatePreview();
  if (dlg.exec() != QDialog::Accepted)
    return;
  QVector<WellTopRecord> copy = m_allRows;
  const int n = WellTopsEdit::applyShift(&copy, spin->value());
  if (n == 0)
  {
    setSummaryLine(tr("位移量为 0——未提交"));
    return;
  }
  commitBatch(std::move(copy), QStringLiteral("batch-shift"),
              tr("全文件 %1 行深度整体位移 %2 m，提交为新版本？")
                  .arg(QString::number(n), QString::number(spin->value(), 'f', 3)));
}

void WellTopsEditorDialog::onBatchDelete()
{
  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("topsBatchDeleteDialog"));
  dlg.setWindowTitle(tr("按层删除（全文件所有井）"));
  auto *lay = new QVBoxLayout(&dlg);
  auto *form = new QFormLayout;
  auto *nameEdit = new QLineEdit(&dlg);
  nameEdit->setObjectName(QStringLiteral("topsDeleteNameEdit"));
  form->addRow(tr("要删除的层名"), nameEdit);
  lay->addLayout(form);
  auto *preview = new QLabel(&dlg);
  preview->setObjectName(QStringLiteral("topsBatchPreviewLabel"));
  preview->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  lay->addWidget(preview);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);
  const auto updatePreview = [&]
  {
    QVector<WellTopRecord> copy = m_allRows;
    const int n = WellTopsEdit::applyDelete(&copy, nameEdit->text());
    preview->setText(tr("将删除 %1 行").arg(QString::number(n)));
  };
  connect(nameEdit, &QLineEdit::textChanged, this, updatePreview);
  updatePreview();
  if (dlg.exec() != QDialog::Accepted)
    return;
  QVector<WellTopRecord> copy = m_allRows;
  const int n = WellTopsEdit::applyDelete(&copy, nameEdit->text());
  if (n == 0)
  {
    setSummaryLine(tr("没有匹配「%1」的层名——未提交").arg(nameEdit->text()));
    return;
  }
  commitBatch(std::move(copy), QStringLiteral("batch-delete"),
              tr("删除全文件 %1 行层名「%2」，提交为新版本？")
                  .arg(QString::number(n), nameEdit->text()));
}

void WellTopsEditorDialog::onRollbackMenu(QAction *action)
{
  const QString targetId = action->data().toString();
  if (targetId.isEmpty())
    return;
  if (!requireCleanTable(tr("回滚")))
    return;
  const QString label = action->text();
  if (QMessageBox::question(
          this, tr("回滚确认"),
          tr("回滚到「%1」？\n将以该版本的完整内容产生一个新版本（当前版本保留在历史中）。")
              .arg(label),
          QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok)
    return;
  const auto out = m_workflow->rollbackTo(m_assetId, targetId);
  if (!out.ok)
  {
    QMessageBox::warning(this, tr("回滚失败"), out.error);
    return;
  }
  setSummaryLine(tr("已回滚为新版本 v%1（内容 = %2）").arg(QString::number(out.newVersionNumber),
                                                           label));
  reloadAll();
}

void WellTopsEditorDialog::rebuildVersionMenu()
{
  QMenu *menu = m_versionButton->menu();
  menu->setObjectName(QStringLiteral("topsVersionMenu"));
  menu->clear();
  const QVector<WellTopsEditorWorkflow::VersionInfo> history =
      m_workflow ? m_workflow->versionHistory(m_assetId) : QVector<WellTopsEditorWorkflow::VersionInfo>();
  const QString currentId =
      m_catalog ? m_catalog->currentVersion(m_assetId).id : QString();
  for (const WellTopsEditorWorkflow::VersionInfo &v : history)
  {
    QString label = QStringLiteral("v%1").arg(QString::number(v.versionNumber));
    if (v.editKind.isEmpty())
      label += tr(" · 导入（%1）").arg(v.stage);
    else if (v.editKind == QLatin1String("rollback"))
      label += tr(" · 回滚");
    else if (v.editKind.startsWith(QLatin1String("batch-")))
      label += tr(" · 批量（%1）").arg(v.editKind.mid(6));
    else if (v.editKind == QLatin1String("merge"))
      label += tr(" · 合并");
    else
      label += tr(" · 编辑%1").arg(v.editWell.isEmpty() ? QString() : QStringLiteral(" ") + v.editWell);
    if (v.stale)
      label += tr("（过时）");
    QAction *act = menu->addAction(label);
    act->setData(v.id);
    if (v.id == currentId)
    {
      act->setEnabled(false);
      act->setText(label + tr(" · 当前"));
    }
    else
      connect(act, &QAction::triggered, this, [this, act] { onRollbackMenu(act); });
  }
  if (menu->actions().isEmpty())
    menu->addAction(tr("（无版本）"))->setEnabled(false);
  m_versionButton->setMenu(menu);
}

void WellTopsEditorDialog::setSummaryLine(const QString &text)
{
  m_summaryLine->setText(text);
}
