// 层：视图
#include "welltopsmergedialog.h"

#include "../../domain/welltopsedit.h"
#include "../paleotheme.h"

#include <QBrush>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{
// 冲突按全字段判定（sameValues 含 X/Y/Z/Time）——值列必须展示全部字段，
// 否则用户被要求裁决一个看不见差异的冲突（评审轮 2 Medium）。
// 单行 + 列宽自适应内容 + 同文 tooltip（固定行高下双行文本会被裁掉，轮 3 M1）。
QString valueText(const WellTopRecord &r)
{
  auto f = [](bool has, double v) {
    return has ? WellTopsEdit::formatDepth(v) : QStringLiteral("空");
  };
  return QStringLiteral("MD %1 TVD %2 X %3 Y %4 Z %5 T %6")
      .arg(f(r.hasMd, r.md), f(r.hasTvd, r.tvd), f(r.hasX, r.x), f(r.hasY, r.y),
           f(r.hasX || r.hasY, r.z), f(r.hasTime, r.timeMs));
}
} // namespace

WellTopsMergeDialog::WellTopsMergeDialog(const QVector<WellTopsEdit::MergeRow> &rows,
                                         QWidget *parent)
  : QDialog(parent)
  , m_rows(rows)
{
  setObjectName(QStringLiteral("wellTopsMergeDialog"));
  setWindowTitle(tr("合并再导入分层"));
  setModal(true);
  resize(900, 480);

  auto *root = new QVBoxLayout(this);
  auto *intro = new QLabel(
      tr("同层名任一字段不同 = 冲突（默认保留旧值）；仅新文件有的层位默认新增。"
         "逐行取舍后确认——结果先进入编辑表，保存才落库。"),
      this);
  intro->setObjectName(QStringLiteral("topsMergeIntro"));
  intro->setWordWrap(true);
  intro->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  root->addWidget(intro);

  m_table = new QTableWidget(m_rows.size(), 4, this);
  m_table->setObjectName(QStringLiteral("topsMergeTable"));
  m_table->setHorizontalHeaderLabels({tr("层名"), tr("当前值（旧）"), tr("导入值（新）"), tr("取舍")});
  m_table->verticalHeader()->hide();
  m_table->horizontalHeader()->setStretchLastSection(true);
  m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  PaleoTheme::applyDensityToViewTree(m_table);

  const auto &tk = PaleoTheme::tokens();
  for (int r = 0; r < m_rows.size(); ++r)
  {
    const WellTopsEdit::MergeRow &m = m_rows.at(r);
    auto *name = new QTableWidgetItem(m.topName);
    m_table->setItem(r, 0, name);
    auto *oldIt = new QTableWidgetItem(m.inOld ? valueText(m.oldRec) : tr("（无）"));
    oldIt->setFont(PaleoTheme::monoFont());
    oldIt->setToolTip(oldIt->text());
    m_table->setItem(r, 1, oldIt);
    auto *newIt = new QTableWidgetItem(m.inNew ? valueText(m.newRec) : tr("（无）"));
    newIt->setFont(PaleoTheme::monoFont());
    newIt->setToolTip(newIt->text());
    m_table->setItem(r, 2, newIt);

    auto *combo = new QComboBox(this);
    combo->setObjectName(QStringLiteral("topsMergeChoice%1").arg(QString::number(r)));
    const int role = int(WellTopsEdit::MergeRow::Resolution::KeepOld);
    const int take = int(WellTopsEdit::MergeRow::Resolution::TakeNew);
    const int removeOld = int(WellTopsEdit::MergeRow::Resolution::RemoveOld);
    if (m.inOld && m.inNew)
    {
      combo->addItem(tr("保留旧值"), role);
      combo->addItem(tr("采用新值"), take);
      if (!m.conflicts()) // 内容本就一致——取舍无实义，仍给开关
        combo->setItemData(0, tr("（两值一致）"), Qt::ToolTipRole);
    }
    else if (m.inOld)
    {
      combo->addItem(tr("保留旧行"), role);
      combo->addItem(tr("删除旧行"), removeOld);
    }
    else
    {
      combo->addItem(tr("新增"), take);
      combo->addItem(tr("跳过"), role);
    }
    combo->setCurrentIndex(m.resolution == WellTopsEdit::MergeRow::Resolution::TakeNew &&
                                   combo->findData(take) >= 0
                               ? combo->findData(take)
                               : 0);
    // 显示态即事实：构造方给的 resolution 与首显项不一致时，以首显为准回写
    //（resolvedRows 与界面永远同步）。
    m_rows[r].resolution =
        static_cast<WellTopsEdit::MergeRow::Resolution>(combo->currentData().toInt());
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this, r](int)
            {
              auto *c = qobject_cast<QComboBox *>(sender());
              if (c && r < m_rows.size())
              {
                m_rows[r].resolution =
                    static_cast<WellTopsEdit::MergeRow::Resolution>(c->currentData().toInt());
                rebuildSummary();
              }
            });
    m_table->setCellWidget(r, 3, combo);

    if (m.conflicts())
      for (int c = 0; c < 4; ++c)
        if (QTableWidgetItem *it = m_table->item(r, c))
        {
          it->setBackground(QBrush(tk.warningBg));
          it->setForeground(QBrush(tk.warningText));
        }
  }
  root->addWidget(m_table, 1);

  m_summary = new QLabel(this);
  m_summary->setObjectName(QStringLiteral("topsMergeSummary"));
  m_summary->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  root->addWidget(m_summary);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("并入编辑表"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);

  rebuildSummary();
}

QVector<WellTopsEdit::MergeRow> WellTopsMergeDialog::resolvedRows() const
{
  return m_rows;
}

void WellTopsMergeDialog::rebuildSummary()
{
  int keep = 0, take = 0, removed = 0, skippedAdd = 0;
  for (const WellTopsEdit::MergeRow &m : m_rows)
  {
    switch (m.resolution)
    {
      case WellTopsEdit::MergeRow::Resolution::KeepOld:
        if (m.inNew && !m.inOld)
          ++skippedAdd;
        else
          ++keep;
        break;
      case WellTopsEdit::MergeRow::Resolution::TakeNew:
        ++take;
        break;
      case WellTopsEdit::MergeRow::Resolution::RemoveOld:
        ++removed;
        break;
    }
  }
  m_summary->setText(tr("保留 %1 · 采用/新增 %2 · 删除旧行 %3 · 跳过新增 %4")
                         .arg(QString::number(keep), QString::number(take),
                              QString::number(removed), QString::number(skippedAdd)));
}
