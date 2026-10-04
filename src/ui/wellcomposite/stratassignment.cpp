// 层：视图
#include "../paleotheme.h"
#include "stratassignment.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include "chronostratcolors.h"

namespace WellComposite
{

namespace StratAssign
{

QStringList unrecognizedLayerNames(const QVector<StratigraphyInterval> &intervals)
{
  QStringList names;
  for (const auto &si : intervals)
  {
    if (si.system.isEmpty() && si.series.isEmpty() && !si.formation.isEmpty() &&
        !names.contains(si.formation))
    {
      names << si.formation;
    }
  }
  return names;
}

QVector<StratigraphyInterval> applyAssignments(const QVector<StratigraphyInterval> &intervals,
                                                const QList<StratAssignment> &assignments)
{
  auto out = intervals;
  for (auto &si : out)
  {
    for (const auto &a : assignments)
    {
      if (si.formation != a.layerName)
        continue;
      // 只填空，不覆盖已识别（程序不猜也不越权改）
      if (si.system.isEmpty() && !a.system.isEmpty())
        si.system = a.system;
      if (si.series.isEmpty() && !a.series.isEmpty())
        si.series = a.series;
      if (!a.formation.isEmpty() && si.formation != a.formation)
        si.formation = a.formation; // 组名规范化（如 A → 指派的真实组名）
      if (!a.member.isEmpty())
        si.series = si.series.isEmpty() ? a.member : si.series; // 段信息并入展示列
    }
  }
  return out;
}

} // namespace StratAssign

// ----------------------------------------------------------------------------
// StratAssignmentDialog
// ----------------------------------------------------------------------------
StratAssignmentDialog::StratAssignmentDialog(const QStringList &layerNames,
                                             const QList<StratAssignment> &existing, QWidget *parent)
  : QDialog(parent), m_layerNames(layerNames)
{
  setObjectName(QStringLiteral("wellCompositeStratAssignDialog"));
  setWindowTitle(tr("地层单元指派"));
  setMinimumSize(QSize(560, 320));

  // 既有指派预填
  m_assignments.clear();
  for (const QString &name : layerNames)
  {
    StratAssignment found;
    found.layerName = name;
    for (const auto &e : existing)
      if (e.layerName == name)
        found = e;
    m_assignments << found;
  }

  auto *root = new QVBoxLayout(this);
  auto *hint = new QLabel(tr("未识别层名需显式指派系/统/组/段（程序不自动猜测）。指派保存到 sidecar，不改源数据。"), this);
  hint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(hint, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + PaleoTheme::metricStyleSheet(QStringLiteral(" font-size: {typography.label}pt;"));
  });
  root->addWidget(hint);

  m_table = new QTableWidget(layerNames.size(), 5, this);
  m_table->setHorizontalHeaderLabels({tr("层名"), tr("系"), tr("统"), tr("组"), tr("段")});
  m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  m_table->verticalHeader()->hide();

  const QStringList systems = ChronostratColors::systems();
  const QStringList formations = ChronostratColors::formationVocabulary();
  for (int r = 0; r < layerNames.size(); ++r)
  {
    auto *nameItem = new QTableWidgetItem(layerNames.at(r));
    nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(r, 0, nameItem);

    auto *sysCombo = new QComboBox(m_table);
    sysCombo->addItem(QString(), QString());
    for (const QString &s : systems)
      sysCombo->addItem(s, s);
    m_table->setCellWidget(r, 1, sysCombo);

    auto *serCombo = new QComboBox(m_table);
    serCombo->addItem(QString(), QString());
    m_table->setCellWidget(r, 2, serCombo);

    auto *formCombo = new QComboBox(m_table);
    formCombo->setEditable(true);
    formCombo->addItem(QString());
    for (const QString &f : formations)
      formCombo->addItem(f);
    m_table->setCellWidget(r, 3, formCombo);

    auto *memberEdit = new QComboBox(m_table);
    memberEdit->setEditable(true);
    memberEdit->addItem(QString());
    m_table->setCellWidget(r, 4, memberEdit);

    // 系选择级联填充统词表
    connect(sysCombo, &QComboBox::currentIndexChanged, this, [this, sysCombo, serCombo]() {
      const QString sys = sysCombo->currentData().toString();
      serCombo->blockSignals(true);
      serCombo->clear();
      serCombo->addItem(QString(), QString());
      for (const QString &s : ChronostratColors::seriesOfSystem(sys))
        serCombo->addItem(s, s);
      serCombo->blockSignals(false);
    });

    rebuildRow(r);
  }
  root->addWidget(m_table, 1);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void StratAssignmentDialog::rebuildRow(int row)
{
  if (row < 0 || row >= m_assignments.size())
    return;
  const StratAssignment &a = m_assignments.at(row);
  if (auto *sys = qobject_cast<QComboBox *>(m_table->cellWidget(row, 1)))
  {
    const int idx = sys->findData(a.system);
    sys->setCurrentIndex(idx >= 0 ? idx : 0);
    // 触发统词表级联
    emit sys->currentIndexChanged(idx >= 0 ? idx : 0);
  }
  if (auto *ser = qobject_cast<QComboBox *>(m_table->cellWidget(row, 2)))
  {
    const int idx = ser->findData(a.series);
    ser->setCurrentIndex(idx >= 0 ? idx : 0);
  }
  if (auto *form = qobject_cast<QComboBox *>(m_table->cellWidget(row, 3)))
    form->setCurrentText(a.formation);
  if (auto *mem = qobject_cast<QComboBox *>(m_table->cellWidget(row, 4)))
    mem->setCurrentText(a.member);
}

void StratAssignmentDialog::setRowAssignment(int row, const QString &system, const QString &series,
                                             const QString &formation, const QString &member)
{
  if (row < 0 || row >= m_assignments.size())
    return;
  StratAssignment &a = m_assignments[row];
  a.system = system;
  a.series = series;
  a.formation = formation;
  a.member = member;
  rebuildRow(row);
}

void StratAssignmentDialog::accept()
{
  // 从控件收集
  for (int r = 0; r < m_layerNames.size() && r < m_assignments.size(); ++r)
  {
    StratAssignment &a = m_assignments[r];
    if (auto *sys = qobject_cast<QComboBox *>(m_table->cellWidget(r, 1)))
      a.system = sys->currentData().toString();
    if (auto *ser = qobject_cast<QComboBox *>(m_table->cellWidget(r, 2)))
      a.series = ser->currentData().toString();
    if (auto *form = qobject_cast<QComboBox *>(m_table->cellWidget(r, 3)))
      a.formation = form->currentText().trimmed();
    if (auto *mem = qobject_cast<QComboBox *>(m_table->cellWidget(r, 4)))
      a.member = mem->currentText().trimmed();
  }
  QDialog::accept();
}

} // namespace WellComposite
