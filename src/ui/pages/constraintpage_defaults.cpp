// 层：视图
#include "constraintpage.h"
#include "constraintpage_internal.h"
#include "pageshared.h"
#include <QComboBox>
#include "../../workflow/workflows.h"
#include "../../services/singlefactordef.h"
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

SingleFactorDefinition ConstraintPage::factorDefinition(const QString &factorId, bool *ok) const
{
  const auto *source = findChild<QComboBox *>(QStringLiteral("factorSourceCombo"));
  return SingleFactorRegistry::byId(factorId, ok, source ? source->currentData().toString() : QStringLiteral("wells"));
}

void ConstraintPage::arrangeFactorFlow(QWidget *content, QVBoxLayout *layout)
{
  auto *details = findChild<QWidget *>(QStringLiteral("factorDataDetailsSection"));
  int index = layout->indexOf(details);
  for (const char *name : {"extractWellFactorsButton", "wellFactorHint"})
    if (auto *widget = findChild<QWidget *>(QString::fromLatin1(name)))
      layout->insertWidget(index++, widget);
  auto *defaults = paleo::pagesinternal::caption(QString(), content);
  defaults->setObjectName(QStringLiteral("factorMapDefaultsHint"));
  defaults->setWordWrap(true);
  layout->insertWidget(layout->indexOf(findChild<QWidget *>(QStringLiteral("factorSettingsCaption"))) + 1, defaults);
}

void ConstraintPage::refreshFactorDefaultsHint()
{
  auto *hint = findChild<QLabel *>(QStringLiteral("factorMapDefaultsHint"));
  auto *table = findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *horizon = findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  if (!hint || !table || !horizon) return;
  const auto factor = paleo::constraintpage_internal::factorIdOfRow(table, paleo::constraintpage_internal::checkedRow(table));
  if (factorDefinition(factor).processingAlgId == QLatin1String("paleo:paleo_isopach")) {
    hint->setText(tr("采用顶、底构造面差值，按结果厚度自动推荐值域；与井分层 MD 厚度分开记录。约束线与等值线均可选。"));
    return;
  }
  double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
  const auto key = horizon->currentText() + QChar(0x1f) + factor;
  auto *workflow = qobject_cast<ConstraintWorkflow *>(property(paleo::pagesinternal::kWfProp).value<QObject *>());
  const bool current = !workflow || !workflow->preparedWellFactorVersion(horizon->currentText(), factor, wellFactorParams()).isEmpty();
  if (current && property("paleo.page.wellRowsKey").toString() == key)
    if (const auto *values = findChild<QTableWidget *>(QStringLiteral("wellFactorTable")))
      for (int row = 0; row < values->rowCount(); ++row) {
        const auto *item = values->item(row, 1);
        if (!item || !item->data(Qt::UserRole).toBool()) continue;
        bool ok = false; const auto value = item->text().toDouble(&ok);
        if (ok && std::isfinite(value)) { minimum = std::min(minimum, value); maximum = std::max(maximum, value); }
      }
  const auto defaults = SingleFactorRegistry::mapDefaults(factor, minimum, maximum);
  hint->setText(defaults.hasRange
      ? tr("推荐值域 %1–%2 %3；等值线间距 %4，可调整。")
          .arg(defaults.minimum, 0, 'g', 6).arg(defaults.maximum, 0, 'g', 6)
          .arg(defaults.unit).arg(defaults.contourInterval, 0, 'g', 6)
      : tr("按有效井值自动推荐值域与等值线间距，默认参数即可成图。"));
}

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
