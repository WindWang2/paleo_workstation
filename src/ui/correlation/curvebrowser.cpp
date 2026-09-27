// 层：视图
#include "curvebrowser.h"

#include <QAbstractItemView>
#include <QColor>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QSignalBlocker>
#include <QTreeWidget>

// ui/correlation/ — CurveBrowser implementation.
//
// Widget choice — QTreeWidget with three columns (曲线/单位/描述) over a
// rich-text QListWidget row: LAS metadata is genuinely columnar. Units are
// short and set in their own typeface, descriptions run long and elide in
// their own stretch column, and per-column fonts (mono mnemonics) come
// free. A QListWidget row would mean hand-assembled HTML strings that
// re-wrap at every width and cannot align units vertically. The cost — a
// header row's height — is spent as the 8pt muted caption DEEP asks for.
// Native QTreeWidget chrome only (DESIGN.md: no custom-painted controls).

namespace
{
  constexpr int kMnemonicRole = Qt::UserRole + 1;

  // DESIGN.md tokens (colors / typography).
  const QColor kText(QStringLiteral("#24303E"));
  const QColor kTextMuted(QStringLiteral("#5D6E80"));
  const QColor kPrimary(QStringLiteral("#1B73D0")); // checked/selected state only

  // 9pt body / 8pt secondary labels, OS-DPI-friendly pointSize.
  QFont pointFont(int pt)
  {
    QFont f;
    f.setPointSize(pt);
    return f;
  }

  // Mnemonics are data — JetBrains Mono for the tabular, instrument feel
  // (DESIGN.md "数值/坐标/深度" rule); TypeWriter hint covers hosts
  // without the vendored face.
  QFont mnemonicFont()
  {
    QFont f(QStringLiteral("JetBrains Mono"));
    f.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(9);
    return f;
  }

  QString rowToolTip(const LasCurve &c)
  {
    QStringList parts;
    if (!c.unit.isEmpty())
      parts << c.unit;
    if (!c.descr.isEmpty())
      parts << c.descr;
    return parts.join(QStringLiteral(" · "));
  }
} // namespace

CurveBrowser::CurveBrowser(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("curveBrowser"));
  setAccessibleName(tr("曲线浏览"));

  auto *lay = new QGridLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // Well identity: 8pt muted one-liner so the user always knows which
  // well the checks act on. Hidden until a well is set.
  m_wellLabel = new QLabel(this);
  m_wellLabel->setObjectName(QStringLiteral("wellLabel"));
  m_wellLabel->setFont(pointFont(8));
  m_wellLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted.name()));
  m_wellLabel->setVisible(false);
  lay->addWidget(m_wellLabel, 0, 0);

  m_list = new QTreeWidget(this);
  m_list->setObjectName(QStringLiteral("curveList"));
  m_list->setAccessibleName(tr("曲线清单"));
  m_list->setFrameShape(QFrame::NoFrame);          // embedded — DEEP, no chrome
  m_list->setRootIsDecorated(false);
  m_list->setIndentation(0);                       // rows flush at column edge
  m_list->setUniformRowHeights(true);
  m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  m_list->setSelectionMode(QAbstractItemView::NoSelection); // checks are the interaction
  m_list->setHeaderLabels({tr("曲线"), tr("单位"), tr("描述")});
  m_list->header()->setSectionsMovable(false);
  m_list->header()->setSectionsClickable(false);
  m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_list->header()->setFont(pointFont(8));         // 8pt muted caption row
  QPalette headerPal = m_list->header()->palette();
  headerPal.setColor(QPalette::WindowText, kTextMuted);
  m_list->header()->setPalette(headerPal);
  lay->addWidget(m_list, 1, 0);

  // §42.4 empty state — warm guidance instead of a blank list, stacked in
  // the list's grid cell so appearing/disappearing never shifts geometry.
  m_emptyLabel = new QLabel(tr("导入 LAS 后选择曲线"), this);
  m_emptyLabel->setObjectName(QStringLiteral("browserEmptyLabel"));
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setFont(pointFont(9));
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted.name()));
  m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  lay->addWidget(m_emptyLabel, 1, 0);
  m_emptyLabel->setVisible(true);

  // The single toggle path: user clicks on the indicator and programmatic
  // setChecked() both land in itemChanged -> onItemChanged -> signal.
  connect(m_list, &QTreeWidget::itemChanged, this, &CurveBrowser::onItemChanged);
}

void CurveBrowser::setCurves(const QString &wellId, const QList<LasCurve> &curves)
{
  m_wellId = wellId;
  const QStringList keepChecked = checkedMnemonics();

  {
    QSignalBlocker block(m_list); // relisting must not emit toggles
    m_list->clear();
    for (const LasCurve &c : curves)
    {
      auto *row = new QTreeWidgetItem(m_list);
      row->setText(0, c.name);
      row->setData(0, kMnemonicRole, c.name);
      row->setText(1, c.unit);
      row->setText(2, c.descr);
      row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
      const bool on = keepChecked.contains(c.name); // survivors keep their state
      row->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
      row->setFont(0, mnemonicFont());
      row->setForeground(0, on ? kPrimary : kText);
      row->setFont(1, pointFont(8));
      row->setForeground(1, kTextMuted);
      row->setFont(2, pointFont(9));
      row->setForeground(2, kTextMuted);
      const QString tip = rowToolTip(c);
      if (!tip.isEmpty())
      {
        row->setToolTip(0, tip); // full text on hover even where elided
        row->setToolTip(2, tip);
      }
    }
    m_rows = m_list->topLevelItemCount();
  }

  m_wellLabel->setText(tr("井：%1").arg(wellId));
  m_wellLabel->setVisible(!wellId.isEmpty());
  m_emptyLabel->setVisible(m_rows == 0);
}

QStringList CurveBrowser::mnemonics() const
{
  QStringList out;
  for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    out << m_list->topLevelItem(i)->data(0, kMnemonicRole).toString();
  return out;
}

QStringList CurveBrowser::checkedMnemonics() const
{
  QStringList out;
  for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    if (m_list->topLevelItem(i)->checkState(0) == Qt::Checked)
      out << m_list->topLevelItem(i)->data(0, kMnemonicRole).toString();
  return out;
}

bool CurveBrowser::isChecked(const QString &mnemonic) const
{
  const QTreeWidgetItem *row = itemFor(mnemonic);
  return row && row->checkState(0) == Qt::Checked;
}

void CurveBrowser::setChecked(const QString &mnemonic, bool on)
{
  QTreeWidgetItem *row = itemFor(mnemonic);
  if (!row)
    return; // unknown mnemonic: no-op, no signal
  if ((row->checkState(0) == Qt::Checked) == on)
    return; // already in the requested state: idempotent silence
  row->setCheckState(0, on ? Qt::Checked : Qt::Unchecked); // fires itemChanged
}

QTreeWidgetItem *CurveBrowser::itemFor(const QString &mnemonic) const
{
  for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    if (m_list->topLevelItem(i)->data(0, kMnemonicRole).toString() == mnemonic)
      return m_list->topLevelItem(i);
  return nullptr;
}

void CurveBrowser::onItemChanged(QTreeWidgetItem *item, int column)
{
  Q_UNUSED(column);
  const QString mnemonic = item->data(0, kMnemonicRole).toString();
  if (mnemonic.isEmpty())
    return; // not a curve row (defensive)
  const bool on = item->checkState(0) == Qt::Checked;

  {
    // Cosmetic: checked rows carry the primary accent on their mnemonic —
    // the only place primary is allowed (selection/checked state). Blocked
    // so the recolor cannot re-enter as a second toggle.
    QSignalBlocker block(m_list);
    item->setForeground(0, on ? kPrimary : kText);
  }
  emit mnemonicToggled(m_wellId, mnemonic, on);
}
