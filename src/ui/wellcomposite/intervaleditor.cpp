// 层：视图
#include "intervaleditor.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace WellComposite
{

namespace IntervalEditor
{

QVector<LithologyInterval> sortedByDepth(const QVector<LithologyInterval> &intervals)
{
  auto sorted = intervals;
  std::sort(sorted.begin(), sorted.end(),
            [](const LithologyInterval &a, const LithologyInterval &b) {
              return a.topDepth < b.topDepth;
            });
  return sorted;
}

QVector<FaciesInterval> sortedFaciesByDepth(const QVector<FaciesInterval> &intervals)
{
  auto sorted = intervals;
  std::sort(sorted.begin(), sorted.end(),
            [](const FaciesInterval &a, const FaciesInterval &b) {
              return a.topDepth < b.topDepth;
            });
  return sorted;
}

QVector<LithologyInterval> mergeAdjacent(const QVector<LithologyInterval> &intervals, int *mergedPairs)
{
  if (mergedPairs)
    *mergedPairs = 0;
  const auto sorted = sortedByDepth(intervals);
  QVector<LithologyInterval> merged;
  for (const auto &li : sorted)
  {
    if (!merged.isEmpty() && merged.last().lithoName == li.lithoName &&
        std::abs(merged.last().bottomDepth - li.topDepth) < 1e-3)
    {
      merged.last().bottomDepth = std::max(merged.last().bottomDepth, li.bottomDepth);
      if (mergedPairs)
        ++(*mergedPairs);
    }
    else
    {
      merged << li;
    }
  }
  return merged;
}

QVector<LithologyInterval> splitAt(const QVector<LithologyInterval> &intervals, int index,
                                   double atDepth, bool *ok)
{
  if (ok)
    *ok = false;
  if (index < 0 || index >= intervals.size())
    return intervals;
  const auto &src = intervals.at(index);
  if (atDepth <= src.topDepth + 1e-3 || atDepth >= src.bottomDepth - 1e-3)
    return intervals;

  auto out = intervals;
  LithologyInterval upper = src;
  upper.bottomDepth = static_cast<float>(atDepth);
  LithologyInterval lower = src;
  lower.topDepth = static_cast<float>(atDepth);
  out[index] = upper;
  out.insert(index + 1, lower);
  if (ok)
    *ok = true;
  return out;
}

QStringList validateLithoIntervals(const QVector<LithologyInterval> &intervals)
{
  QStringList issues;
  for (int i = 0; i < intervals.size(); ++i)
  {
    const auto &li = intervals.at(i);
    if (li.bottomDepth <= li.topDepth)
      issues << QStringLiteral("第 %1 段「%2」：底深 %3 ≤ 顶深 %4")
                    .arg(QString::number(i + 1), li.lithoName,
                         QString::number(li.topDepth, 'f', 1),
                         QString::number(li.bottomDepth, 'f', 1));
    if (li.lithoName.trimmed().isEmpty())
      issues << QStringLiteral("第 %1 段：岩性名为空").arg(QString::number(i + 1));
  }
  // 重叠检查（排序后相邻比较）
  const auto sorted = sortedByDepth(intervals);
  for (int i = 1; i < sorted.size(); ++i)
  {
    if (sorted.at(i).topDepth < sorted.at(i - 1).bottomDepth - 1e-3)
      issues << QStringLiteral("「%1」与「%2」深度重叠 [%3 ~ %4]")
                    .arg(sorted.at(i - 1).lithoName, sorted.at(i).lithoName,
                         QString::number(sorted.at(i).topDepth, 'f', 1),
                         QString::number(sorted.at(i - 1).bottomDepth, 'f', 1));
  }
  return issues;
}

QVector<FaciesIssue> validateFaciesIntervals(const QVector<FaciesInterval> &intervals)
{
  QVector<FaciesIssue> issues;
  for (int i = 0; i < intervals.size(); ++i)
  {
    const auto &fi = intervals.at(i);
    if (fi.bottomDepth <= fi.topDepth)
      issues << FaciesIssue{i, QStringLiteral("底深 ≤ 顶深")};
    // 三级联动：微相存在则亚相、相都必须存在（微相⊂亚相⊂相）
    if (!fi.microFacies.isEmpty())
    {
      if (fi.subFacies.isEmpty())
        issues << FaciesIssue{i, QStringLiteral("微相「%1」缺少亚相归属").arg(fi.microFacies)};
      if (fi.majorFacies.isEmpty())
        issues << FaciesIssue{i, QStringLiteral("微相「%1」缺少相归属").arg(fi.microFacies)};
    }
    if (!fi.subFacies.isEmpty() && fi.majorFacies.isEmpty())
      issues << FaciesIssue{i, QStringLiteral("亚相「%1」缺少相归属").arg(fi.subFacies)};
  }

  // 嵌套校验：以亚相区间为骨架，微相区间必须完整落在同名亚相区间内
  const auto sorted = sortedFaciesByDepth(intervals);
  for (int m = 0; m < sorted.size(); ++m)
  {
    const auto &micro = sorted.at(m);
    if (micro.subFacies.isEmpty())
      continue;
    bool nested = false;
    for (const auto &sub : sorted)
    {
      if (sub.subFacies != micro.subFacies || sub.topDepth > micro.topDepth + 1e-3 ||
          sub.bottomDepth < micro.bottomDepth - 1e-3)
        continue;
      // 同名亚相区间存在且包住该微相
      nested = true;
      break;
    }
    if (!nested)
    {
      // 亚相区间可能被切片为多段同名区间：任一段包住即合法（上面循环已覆盖），
      // 走到这说明没有任何同名亚相段完整包住该微相
      issues << FaciesIssue{m, QStringLiteral("微相「%1」[%2~%3] 越出亚相「%4」区间")
                                 .arg(micro.microFacies, QString::number(micro.topDepth, 'f', 1),
                                      QString::number(micro.bottomDepth, 'f', 1), micro.subFacies)};
    }
  }
  return issues;
}

} // namespace IntervalEditor

// ----------------------------------------------------------------------------
// D3.4 LithoIntervalDialog
// ----------------------------------------------------------------------------
LithoIntervalDialog::LithoIntervalDialog(const LithologyInterval &initial, double minDepth,
                                         double maxDepth, QWidget *parent)
  : QDialog(parent), m_initial(initial), m_result(initial), m_minDepth(minDepth), m_maxDepth(maxDepth)
{
  setObjectName(QStringLiteral("wellCompositeLithoDialog"));
  setWindowTitle(tr("编辑岩性区间"));

  auto *root = new QVBoxLayout(this);
  auto *form = new QFormLayout();
  form->setLabelAlignment(Qt::AlignRight);

  m_nameEdit = new QLineEdit(initial.lithoName, this);
  form->addRow(tr("岩性名:"), m_nameEdit);

  m_topSpin = new QDoubleSpinBox(this);
  m_topSpin->setRange(minDepth, maxDepth);
  m_topSpin->setDecimals(1);
  m_topSpin->setValue(initial.topDepth);
  form->addRow(tr("顶深 (m):"), m_topSpin);

  m_bottomSpin = new QDoubleSpinBox(this);
  m_bottomSpin->setRange(minDepth, maxDepth);
  m_bottomSpin->setDecimals(1);
  m_bottomSpin->setValue(initial.bottomDepth);
  form->addRow(tr("底深 (m):"), m_bottomSpin);

  m_noteEdit = new QLineEdit(initial.lithoCode, this);
  form->addRow(tr("备注:"), m_noteEdit);

  root->addLayout(form);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void LithoIntervalDialog::accept()
{
  if (m_bottomSpin->value() <= m_topSpin->value())
    return; // 底 ≤ 顶：不接受（保持对话框打开）
  m_result = m_initial;
  m_result.lithoName = m_nameEdit->text().trimmed();
  m_result.topDepth = static_cast<float>(m_topSpin->value());
  m_result.bottomDepth = static_cast<float>(m_bottomSpin->value());
  m_result.lithoCode = m_noteEdit->text();
  QDialog::accept();
}

// ----------------------------------------------------------------------------
// D3.5 FaciesIntervalDialog
// ----------------------------------------------------------------------------
FaciesIntervalDialog::FaciesIntervalDialog(const FaciesInterval &initial, double minDepth,
                                           double maxDepth, QWidget *parent)
  : QDialog(parent), m_initial(initial), m_result(initial), m_minDepth(minDepth), m_maxDepth(maxDepth)
{
  setObjectName(QStringLiteral("wellCompositeFaciesDialog"));
  setWindowTitle(tr("编辑相区间"));

  auto *root = new QVBoxLayout(this);
  auto *form = new QFormLayout();
  form->setLabelAlignment(Qt::AlignRight);

  m_majorEdit = new QLineEdit(initial.majorFacies, this);
  form->addRow(tr("相:"), m_majorEdit);
  m_subEdit = new QLineEdit(initial.subFacies, this);
  form->addRow(tr("亚相:"), m_subEdit);
  m_microEdit = new QLineEdit(initial.microFacies, this);
  form->addRow(tr("微相:"), m_microEdit);

  m_topSpin = new QDoubleSpinBox(this);
  m_topSpin->setRange(minDepth, maxDepth);
  m_topSpin->setDecimals(1);
  m_topSpin->setValue(initial.topDepth);
  form->addRow(tr("顶深 (m):"), m_topSpin);

  m_bottomSpin = new QDoubleSpinBox(this);
  m_bottomSpin->setRange(minDepth, maxDepth);
  m_bottomSpin->setDecimals(1);
  m_bottomSpin->setValue(initial.bottomDepth);
  form->addRow(tr("底深 (m):"), m_bottomSpin);

  root->addLayout(form);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void FaciesIntervalDialog::accept()
{
  if (m_bottomSpin->value() <= m_topSpin->value())
    return;
  m_result = m_initial;
  m_result.majorFacies = m_majorEdit->text().trimmed();
  m_result.subFacies = m_subEdit->text().trimmed();
  m_result.microFacies = m_microEdit->text().trimmed();
  m_result.topDepth = static_cast<float>(m_topSpin->value());
  m_result.bottomDepth = static_cast<float>(m_bottomSpin->value());
  QDialog::accept();
}

} // namespace WellComposite
