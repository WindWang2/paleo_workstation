#include "curvebrowser.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

// Wave-base stub: listing + check toggles are final-quality; subtask B
// refines presentation (unit/description columns, DESIGN.md typography)
// and relist state-keeping.

namespace
{
  constexpr int kMnemonicRole = Qt::UserRole + 1;
}

CurveBrowser::CurveBrowser(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  auto *title = new QLabel(tr("曲线浏览"), this);
  lay->addWidget(title);

  m_list = new QListWidget(this);
  m_list->setObjectName(QStringLiteral("curveList"));
  lay->addWidget(m_list, 1);

  connect(m_list, &QListWidget::itemChanged, this, &CurveBrowser::onItemChanged);
}

void CurveBrowser::setCurves(const QString &wellId, const QList<LasCurve> &curves)
{
  m_wellId = wellId;
  const QStringList keepChecked = checkedMnemonics();

  QSignalBlocker block(m_list); // relisting must not emit toggles
  m_list->clear();
  m_rows = 0;
  for (const LasCurve &c : curves)
  {
    auto *item = new QListWidgetItem(c.name, m_list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(keepChecked.contains(c.name) ? Qt::Checked : Qt::Unchecked);
    item->setData(kMnemonicRole, c.name);
    if (!c.descr.isEmpty())
      item->setToolTip(c.descr);
    ++m_rows;
  }
}

QStringList CurveBrowser::mnemonics() const
{
  QStringList out;
  for (int i = 0; i < m_list->count(); ++i)
    out << m_list->item(i)->data(kMnemonicRole).toString();
  return out;
}

QStringList CurveBrowser::checkedMnemonics() const
{
  QStringList out;
  for (int i = 0; i < m_list->count(); ++i)
    if (m_list->item(i)->checkState() == Qt::Checked)
      out << m_list->item(i)->data(kMnemonicRole).toString();
  return out;
}

bool CurveBrowser::isChecked(const QString &mnemonic) const
{
  for (int i = 0; i < m_list->count(); ++i)
    if (m_list->item(i)->data(kMnemonicRole).toString() == mnemonic)
      return m_list->item(i)->checkState() == Qt::Checked;
  return false;
}

void CurveBrowser::setChecked(const QString &mnemonic, bool on)
{
  for (int i = 0; i < m_list->count(); ++i)
    if (m_list->item(i)->data(kMnemonicRole).toString() == mnemonic)
    {
      m_list->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked); // fires itemChanged
      return;
    }
}

void CurveBrowser::onItemChanged(QListWidgetItem *item)
{
  emit mnemonicToggled(m_wellId, item->data(kMnemonicRole).toString(),
                       item->checkState() == Qt::Checked);
}
