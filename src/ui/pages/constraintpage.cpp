#include "constraintpage.h"

#include "panelshared.h"

#include "../../io/arearules.h"
#include "../../domain/mappinghorizons.h"
#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace PaleoPanel;

// 层：视图
// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素
// ---------------------------------------------------------------------------
ConstraintPage::ConstraintPage(ConstraintWorkflow *wf, QWidget *parent)
  : QWidget(parent)
{
  auto *lay = panelLayout(this);

  lay->addWidget(caption(tr("层位"), this));
  auto *horizons = new QComboBox(this);
  horizons->setObjectName(QStringLiteral("horizonCombo"));
  lay->addWidget(horizons);

  lay->addWidget(caption(tr("约束"), this));
  auto *list = new QListWidget(this);
  list->setObjectName(QStringLiteral("constraintList"));
  list->setAccessibleName(tr("约束列表"));
  lay->addWidget(list, 1);

  auto *spin = new QSpinBox(this);
  spin->setObjectName(QStringLiteral("faciesCodeSpin"));
  spin->setRange(0, 9999);
  spin->setAccessibleName(tr("相代码"));
  lay->addWidget(spin);

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox(this);
  shape->setObjectName(QStringLiteral("shapeCombo"));
  shape->addItem(tr("约束线"), QStringLiteral("line"));
  shape->addItem(tr("约束多边形"), QStringLiteral("polygon"));
  shape->addItem(tr("约束矩形"), QStringLiteral("rect"));
  shape->addItem(tr("约束点"), QStringLiteral("point"));
  shape->addItem(tr("约束圆"), QStringLiteral("circle"));
  shape->addItem(tr("约束椭圆"), QStringLiteral("ellipse"));
  lay->addWidget(shape);

  auto *draw = new QPushButton(tr("绘制约束"), this);
  draw->setObjectName(QStringLiteral("drawButton"));
  lay->addWidget(draw);
  connect(draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested(horizons->currentText(),
                                 shape->currentData().toString(), spin->value());
  });

  auto *field = new QLineEdit(QStringLiteral("z"), this);
  field->setObjectName(QStringLiteral("idwField"));
  field->setPlaceholderText(tr("井属性字段"));
  field->setAccessibleName(tr("插值字段"));
  lay->addWidget(field);

  auto *cell = new QDoubleSpinBox(this);
  cell->setObjectName(QStringLiteral("idwCellSize"));
  cell->setRange(0.0001, 1.0e9);
  cell->setDecimals(4);
  cell->setValue(1.0);
  cell->setAccessibleName(tr("像元大小"));
  lay->addWidget(cell);

  auto *idw = new QPushButton(tr("插值"), this);
  idw->setObjectName(QStringLiteral("runIdwButton"));
  lay->addWidget(idw);
  connect(idw, &QPushButton::clicked, this, [this, horizons] {
    emit runIdwRequested(horizons->currentText());
  });

  auto *status = new QLabel(this);
  status->setObjectName(QStringLiteral("statusLabel"));
  status->setWordWrap(true);
  lay->addWidget(status);

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // 镜像到 ConstraintWorkflow 的 paleo.thickness.* 动态属性；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  lay->addSpacing(16); // spacing.md
  // 厚度样本的层位/基面名随工程参数（AreaRules targetHorizon + 有序集合的
  // 下一界面）——文案在工程打开时由 refreshAreaParamLabels 重写。
  auto *thCap = caption(
      tr("%1→%2 厚度样本")
          .arg(AreaRules::active().targetHorizon,
               baseHorizonFor(AreaRules::active().targetHorizon)),
      this);
  thCap->setObjectName(QStringLiteral("thicknessCaption"));
  lay->addWidget(thCap);
  auto *thTable = new QTableWidget(0, 4, this);
  thTable->setObjectName(QStringLiteral("thicknessTable"));
  thTable->setAccessibleName(tr("厚度样本表"));
  thTable->setHorizontalHeaderLabels(
      {tr("井名"), tr("%1 TVD").arg(AreaRules::active().targetHorizon),
       tr("%1 TVD").arg(baseHorizonFor(AreaRules::active().targetHorizon)),
       tr("层间速度或原因")});
  thTable->verticalHeader()->setVisible(false);
  thTable->horizontalHeader()->setStretchLastSection(true);
  lay->addWidget(thTable, 1);
  auto *thHint = new QLabel(this);
  thHint->setObjectName(QStringLiteral("thicknessHint"));
  thHint->setWordWrap(true);
  thHint->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
  lay->addWidget(thHint);

  if (wf) // workflow feedback lands on the status label
  {
    setProperty(kWfProp, QVariant::fromValue(static_cast<QObject *>(wf)));
    connect(wf, &ConstraintWorkflow::constraintAdded, status,
            [status](const QString &id) { status->setText(tr("已添加约束 %1").arg(id)); });
    connect(wf, &ConstraintWorkflow::factorDone, status,
            [status](const QString &h, const QString &layerId) {
              status->setText(tr("单因素完成：%1 → %2").arg(h, layerId));
            });
  }
}

void ConstraintPage::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  refreshThicknessSamples();
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>(this, "thicknessTable");
  auto *hint = child<QLabel>(this, "thicknessHint");
  if (!table)
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  const QVariantList rows =
      wf ? wf->property("paleo.thickness.samples").toList() : QVariantList();
  const QString message =
      wf ? wf->property("paleo.thickness.message").toString() : QString();

  table->setRowCount(0);
  for (const QVariant &v : rows)
  {
    const QVariantMap m = v.toMap();
    const int r = table->rowCount();
    table->insertRow(r);
    auto *name = new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString());
    const QString tvdTop = m.contains(QStringLiteral("tvd_top"))
                               ? QString::number(m.value(QStringLiteral("tvd_top")).toDouble(), 'f', 1)
                               : QStringLiteral("—");
    const QString tvdBase = m.contains(QStringLiteral("tvd_base"))
                                ? QString::number(m.value(QStringLiteral("tvd_base")).toDouble(), 'f', 1)
                                : QStringLiteral("—");
    // 贡献井 → 层间速度（m/s）；否则 → 原因文案。
    const QString last = m.value(QStringLiteral("contributing")).toBool()
                             ? tr("%1 m/s").arg(m.value(QStringLiteral("vint")).toDouble(), 0, 'f', 0)
                             : m.value(QStringLiteral("reason")).toString();
    auto *itTop = new QTableWidgetItem(tvdTop);
    auto *itBase = new QTableWidgetItem(tvdBase);
    auto *itV = new QTableWidgetItem(last);
    for (auto *it : {name, itTop, itBase, itV})
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    table->setItem(r, 0, name);
    table->setItem(r, 1, itTop);
    table->setItem(r, 2, itBase);
    table->setItem(r, 3, itV);
  }
  if (hint)
    hint->setText(message);
}
