// 层：视图
// constraintpage_wellfactors — 井点因子域 TU（方向 96 自 constraintpage.cpp
// 拆出，原文行序保持）：wellFactorSection 构建（口径下拉/字段三下拉/提取与
// 维护/结果表）+ 井点因子六方法 + wellAttributes/horizons/factors 触发接线。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildWellFactorSection( QWidget *content, QVBoxLayout *lay, QComboBox *horizons,
                                             QTableWidget *factors )
{
  auto *wellFactors = new QWidget(content);
  wellFactors->setObjectName(QStringLiteral("wellFactorSection"));
  auto *wellLay = panelLayout(wellFactors);
  wellLay->addWidget(caption(tr("井点因子"), wellFactors));
  auto *factorMode = new QComboBox(wellFactors);
  factorMode->setObjectName(QStringLiteral("factorModeCombo"));
  factorMode->addItem(tr("直读字段"), QStringLiteral("direct"));
  factorMode->addItem(tr("比值（分子 ÷ 分母）"), QStringLiteral("ratio"));
  factorMode->setAccessibleName(tr("因子提取口径"));
  wellLay->addWidget(factorMode);
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), tr("指标字段")},
       {QStringLiteral("factorNumeratorFieldCombo"), tr("分子字段")},
       {QStringLiteral("factorDenominatorFieldCombo"), tr("分母字段")}}) {
    auto *label = caption(entry.second, wellFactors);
    label->setObjectName(entry.first + QStringLiteral("Caption"));
    wellLay->addWidget(label);
    auto *combo = new QComboBox(wellFactors); combo->setObjectName(entry.first);
    combo->setAccessibleName(entry.second); combo->setPlaceholderText(tr("请选择字段")); wellLay->addWidget(combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { invalidateWellFactors(); });
  }
  auto *extract = new QPushButton(tr("提取井点因子"), wellFactors);
  extract->setObjectName(QStringLiteral("extractWellFactorsButton"));
  auto *maintain = new QPushButton(tr("维护井点属性"), wellFactors);
  maintain->setObjectName(QStringLiteral("maintainWellFactorsButton"));
  maintain->setToolTip(tr("按当前层位维护砂厚、层厚、砂地比与其他单因素；保存后重新提取"));
  wellLay->addWidget(maintain);
  connect(maintain, &QPushButton::clicked, this, [this, horizons] { emit maintainWellFactorsRequested(horizons->currentText()); });
  wellLay->addWidget(extract);
  auto *wellTable = new QTableWidget(0, 3, wellFactors);
  wellTable->setObjectName(QStringLiteral("wellFactorTable"));
  wellTable->setAccessibleName(tr("井点因子提取结果"));
  wellTable->setHorizontalHeaderLabels({tr("井名"), tr("因子值"), tr("来源或缺失原因")});
  wellTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  wellTable->verticalHeader()->hide(); wellTable->horizontalHeader()->setStretchLastSection(true);
  wellLay->addWidget(wellTable);
  auto *wellHint = new QLabel(tr("选择字段后提取；解释砂厚和分层层厚采用 MD，同层段相除得到砂地比。"), wellFactors);
  wellHint->setObjectName(QStringLiteral("wellFactorHint")); wellHint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(wellHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  wellLay->addWidget(wellHint); lay->addWidget(wellFactors);
  const auto modeChanged = [this, factorMode] {
    const bool ratio = factorMode->currentData() == QStringLiteral("ratio");
    for (const QString &name : {QStringLiteral("factorValueFieldCombo"), QStringLiteral("factorNumeratorFieldCombo"),
                               QStringLiteral("factorDenominatorFieldCombo")}) {
      const bool visible = (name == QLatin1String("factorValueFieldCombo")) != ratio;
      findChild<QWidget *>(name)->setVisible(visible);
      findChild<QWidget *>(name + QStringLiteral("Caption"))->setVisible(visible);
    }
    invalidateWellFactors();
  };
  connect(factorMode, &QComboBox::currentIndexChanged, this, modeChanged); modeChanged();
  connect(extract, &QPushButton::clicked, this, [this, factors, horizons] {
    const int r = checkedRow(factors); if (r < 0) return;
    emit extractWellFactorsRequested(factorIdOfRow(factors, r), horizons->currentText(), wellFactorParams());
  });
}

void ConstraintPage::wireWellFactorTriggers( ConstraintWorkflow *wf, QComboBox *horizons, QTableWidget *factors )
{
  if (wf) {
    connect(wf, &ConstraintWorkflow::wellAttributesChanged, this, [this] { invalidateWellFactors(); refreshWellFactorFields(); });
    connect(wf, &ConstraintWorkflow::wellFactorsExtracted, this,
            [this](const QString &, const QString &) { refreshWellFactorResults(); });
  }
  connect(horizons, &QComboBox::currentIndexChanged, this, [this] { invalidateWellFactors(); refreshWellFactorFields(); });
  connect(factors, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (item && item->column() == 0) { invalidateWellFactors(); refreshWellFactorFields(); }
  });
}

QVariantMap ConstraintPage::wellFactorParams() const {
  const auto value = [this](const char *name) { auto *c = findChild<QComboBox *>(QString::fromLatin1(name)); return c ? c->currentData().toString() : QString(); };
  return {{QStringLiteral("factorMode"), value("factorModeCombo")},
          {QStringLiteral("valueField"), value("factorValueFieldCombo")},
          {QStringLiteral("numeratorField"), value("factorNumeratorFieldCombo")},
          {QStringLiteral("denominatorField"), value("factorDenominatorFieldCombo")}};
}
void ConstraintPage::refreshWellFactorFields() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *h = child<QComboBox>(this, "horizonCombo");
  QString error;
  const QVariantList fields = wf ? wf->wellFactorFields(h ? h->currentText() : QString(), &error) : QVariantList();
  if (!error.isEmpty()) child<QLabel>(this, "wellFactorHint")->setText(error);
  setWellFactorFields(fields);
  setProperty("paleo.page.fieldsbound", wf != nullptr);
  updateFactorActionStates();
}
void ConstraintPage::setWellFactorFields(const QVariantList &fields) {
  setProperty("paleo.page.fieldsbound", true);
  QVariantMap old = wellFactorParams();
  auto *factors = child<QTableWidget>(this, "factorTable");
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  const bool changedFactor = property("paleo.page.extractionFactor").toString() != factor;
  if (changedFactor) old.remove(QStringLiteral("valueField"));
  setProperty("paleo.page.extractionFactor", factor);
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), QStringLiteral("valueField")},
       {QStringLiteral("factorNumeratorFieldCombo"), QStringLiteral("numeratorField")},
       {QStringLiteral("factorDenominatorFieldCombo"), QStringLiteral("denominatorField")}}) {
    auto *c = findChild<QComboBox *>(entry.first); if (!c) continue;
    c->blockSignals(true); c->clear();
    for (const QVariant &v : fields) { const auto m = v.toMap(); c->addItem(m.value(QStringLiteral("label")).toString(), m.value(QStringLiteral("id"))); }
    QString selected = old.value(entry.second).toString();
    if (selected.isEmpty() && entry.second == QLatin1String("valueField")) {
      const auto *table = child<QTableWidget>(this, "factorTable"); const int r = checkedRow(table);
      if (r >= 0) selected = SingleFactorRegistry::byId(factorIdOfRow(table, r)).defaultParams.value(QStringLiteral("field")).toString();
    }
    if (selected.isEmpty() && entry.second == QLatin1String("numeratorField")) selected = QStringLiteral("log_sand_thickness_md");
    if (selected.isEmpty() && entry.second == QLatin1String("denominatorField")) selected = QStringLiteral("log_layer_thickness_md");
    int index = c->findData(selected);
    if (index < 0 && entry.second == QLatin1String("valueField")) {
      if (factor == QLatin1String("sandthick")) index = c->findData(QStringLiteral("log_sand_thickness_md"));
      if (factor == QLatin1String("strathick")) index = c->findData(QStringLiteral("log_layer_thickness_md"));
    }
    c->setCurrentIndex(index); c->blockSignals(false);
  }
  if (changedFactor) {
    const bool direct = child<QComboBox>(this, "factorValueFieldCombo")->currentIndex() >= 0;
    auto *mode = child<QComboBox>(this, "factorModeCombo");
    const bool ratioAvailable = child<QComboBox>(this, "factorNumeratorFieldCombo")->currentIndex() >= 0 && child<QComboBox>(this, "factorDenominatorFieldCombo")->currentIndex() >= 0;
    mode->setCurrentIndex(mode->findData(factor == QLatin1String("sandratio") && !direct && ratioAvailable ? QStringLiteral("ratio") : QStringLiteral("direct")));
  }
  updateFactorActionStates();
}
void ConstraintPage::invalidateWellFactors() {
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("wellFactorTable"))) table->setRowCount(0);
  if (auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorHint")))
    hint->setText(tr("当前口径尚未提取井点因子。选择字段后提取；解释厚度采用同一 MD 层段。"));
  markInputsStale(); updateWellFactorActionState();
  updateFactorActionStates();
}
void ConstraintPage::updateWellFactorActionState() {
  auto *button = child<QPushButton>(this, "extractWellFactorsButton"); if (!button) return;
  const auto params = wellFactorParams(); const bool ratio = params.value(QStringLiteral("factorMode")) == QStringLiteral("ratio");
  const bool ready = checkedRow(child<QTableWidget>(this, "factorTable")) >= 0 &&
      (ratio ? !params.value(QStringLiteral("numeratorField")).toString().isEmpty() && !params.value(QStringLiteral("denominatorField")).toString().isEmpty()
             : !params.value(QStringLiteral("valueField")).toString().isEmpty());
  const bool busy = property("paleo.page.runbusy").toBool();
  button->setEnabled(ready && !busy);
  button->setToolTip(busy ? tr("正在计算，可取消") : ready ? QString() : tr("请勾选因子并选择真实提取字段"));
}
void ConstraintPage::refreshWellFactorResults() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *table = child<QTableWidget>(this, "wellFactorTable"); if (!wf || !table) return;
  table->setRowCount(0);
  const auto params = wellFactorParams();
  table->setHorizontalHeaderItem(1, new QTableWidgetItem(params.value(QStringLiteral("factorMode")) == QStringLiteral("ratio") ? tr("比值（分子 ÷ 分母）") : tr("因子值")));
  for (const QVariant &v : wf->wellFactorRows()) {
    const auto m = v.toMap(); const int row = table->rowCount(); table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString()));
    auto *value = new QTableWidgetItem(m.contains(QStringLiteral("value")) ? QString::number(m.value(QStringLiteral("value")).toDouble(), 'g', 8) : QStringLiteral("—"));
    value->setFont(PaleoTheme::monoFont()); table->setItem(row, 1, value);
    auto *detail = new QTableWidgetItem(m.value(QStringLiteral("contributing")).toBool() ? m.value(QStringLiteral("source")).toString() : m.value(QStringLiteral("reason")).toString());
    detail->setToolTip(detail->text()); table->setItem(row, 2, detail);
  }
  child<QLabel>(this, "wellFactorHint")->setText(wf->wellFactorMessage());
}
