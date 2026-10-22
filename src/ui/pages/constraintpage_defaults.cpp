// 层：视图
#include "constraintpage.h"
#include "constraintpage_internal.h"
#include "pageshared.h"
#include <QComboBox>
#include "../../workflow/workflows.h"
#include <QDoubleSpinBox>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <limits>
void ConstraintPage::suggestFactorCellSize() {
  if (property("paleo.page.cellSizeManual").toBool()) return;
  auto *cell = findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  auto *workflow = qobject_cast<ConstraintWorkflow *>(property(paleo::pagesinternal::kWfProp).value<QObject *>());
  if (!cell || !workflow) return;
  auto *horizon = findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  auto *table = findChild<QTableWidget *>(QStringLiteral("factorTable"));
  const auto id = paleo::constraintpage_internal::factorIdOfRow(table, paleo::constraintpage_internal::checkedRow(table));
  if (!horizon || property("paleo.page.wellRowsKey").toString() != horizon->currentText() + QChar(0x1f) + id) return;
  double minX = std::numeric_limits<double>::infinity(), minY = minX, maxX = -minX, maxY = -minX;
  for (const auto &value : workflow->wellFactorRows()) {
    const auto row = value.toMap();
    if (!row.value("contributing").toBool() || !row.contains("x") || !row.contains("y")) continue;
    const double x = row.value("x").toDouble(), y = row.value("y").toDouble();
    minX = std::min(minX,x); maxX = std::max(maxX,x); minY = std::min(minY,y); maxY = std::max(maxY,y);
  }
  const double span = std::max(maxX - minX, maxY - minY);
  if (!std::isfinite(span) || span <= 0) return;
  const double scale = std::pow(10.,std::floor(std::log10(span / 300.)));
  const double normalized = span / 300. / scale;
  const double step = std::max(.0001, (normalized < 1.5 ? 1 : normalized < 3.5 ? 2 : normalized < 7.5 ? 5 : 10) * scale);
  const QSignalBlocker blocker(cell); cell->setValue(step);
}
