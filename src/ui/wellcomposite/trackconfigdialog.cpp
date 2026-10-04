// 层：视图
// token 例外：DESIGN 数据符号例外：曲线样式预设编辑器的数据色值，使用者选择后写入曲线配置。（tools/ui-token-exceptions.json 精确计数）。
#include "trackconfigdialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace WellComposite
{

// 预置颜色集（D1.6「颜色集」）：经典测井 / 高对比 / 柔和三套，每套 4 槽
struct ColorPaletteDef
{
  QString name;
  QColor colors[4];
};

static const ColorPaletteDef kPalettes[] = {
    {QStringLiteral("经典测井"),
     {QColor(QStringLiteral("#2E7D32")), QColor(QStringLiteral("#D32F2F")),
      QColor(QStringLiteral("#0288D1")), QColor(QStringLiteral("#7B1FA2"))}},
    {QStringLiteral("高对比"),
     {QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#E53935")),
      QColor(QStringLiteral("#1B73D0")), QColor(QStringLiteral("#F29900"))}},
    {QStringLiteral("柔和"),
     {QColor(QStringLiteral("#66BB6A")), QColor(QStringLiteral("#EF7777")),
      QColor(QStringLiteral("#64B5F6")), QColor(QStringLiteral("#BA68C8"))}},
};

QStringList TrackConfigDialog::paletteNames()
{
  QStringList names;
  for (const auto &p : kPalettes)
    names << p.name;
  return names;
}

QColor TrackConfigDialog::paletteColor(int paletteIdx, int curveIdx)
{
  if (paletteIdx < 0 || paletteIdx >= 3)
    paletteIdx = 0;
  return kPalettes[paletteIdx].colors[qBound(0, curveIdx, 3)];
}

TrackConfigDialog::TrackConfigDialog(const TrackSpec &spec, const QVector<CurveData> &curvePool,
                                     QWidget *parent)
  : QDialog(parent), m_initial(spec), m_result(spec), m_curvePool(curvePool)
{
  setObjectName(QStringLiteral("wellCompositeTrackConfigDialog"));
  setWindowTitle(tr("道配置"));
  setMinimumSize(QSize(460, 420));
  buildUi();
}

void TrackConfigDialog::buildUi()
{
  auto *root = new QVBoxLayout(this);

  auto *form = new QFormLayout();
  form->setLabelAlignment(Qt::AlignRight);

  m_typeCombo = new QComboBox(this);
  const QStringList creatable = TrackRegistry::instance().userCreatableTypeIds();
  for (const QString &id : creatable)
    m_typeCombo->addItem(TrackRegistry::instance().displayName(id), id);
  const int typeIdx = m_typeCombo->findData(m_initial.typeId);
  if (typeIdx >= 0)
    m_typeCombo->setCurrentIndex(typeIdx);
  m_typeCombo->setEnabled(false); // 既有道改类型 = 删旧建新（右键菜单「新建道」路径），此处禁改防误触
  form->addRow(tr("道类型:"), m_typeCombo);

  m_titleEdit = new QLineEdit(m_initial.title, this);
  form->addRow(tr("标题:"), m_titleEdit);

  m_widthSpin = new QSpinBox(this);
  m_widthSpin->setRange(24, 600);
  m_widthSpin->setValue(static_cast<int>(m_initial.width));
  m_widthSpin->setSuffix(QStringLiteral(" px"));
  form->addRow(tr("道宽:"), m_widthSpin);

  m_printCheck = new QCheckBox(tr("参与打印/导出"), this);
  m_printCheck->setChecked(m_initial.printIncluded);
  form->addRow(QString(), m_printCheck);

  root->addLayout(form);

  // ---- 曲线族配置区（D1.7 多曲线组合编辑器）----
  const bool isCurveFamily = m_initial.typeId == QStringLiteral("curve") ||
                             m_initial.typeId == QStringLiteral("gr") ||
                             m_initial.typeId == QStringLiteral("discrete");
  m_curveFamily = isCurveFamily; // isVisible() 在未 show 的对话框上恒 false，用标志
  m_curveGroup = new QWidget(this);
  auto *curveLay = new QVBoxLayout(m_curveGroup);

  auto *paletteRow = new QHBoxLayout();
  paletteRow->addWidget(new QLabel(tr("颜色集:"), m_curveGroup));
  m_paletteCombo = new QComboBox(m_curveGroup);
  m_paletteCombo->addItems(paletteNames());
  paletteRow->addWidget(m_paletteCombo);
  paletteRow->addStretch(1);
  curveLay->addLayout(paletteRow);
  connect(m_paletteCombo, &QComboBox::activated, this, &TrackConfigDialog::applyPalette);

  m_curveTable = new QTableWidget(m_curveGroup);
  m_curveTable->setColumnCount(7);
  m_curveTable->setHorizontalHeaderLabels(
      {tr("选用"), tr("曲线"), tr("最小值"), tr("最大值"), tr("对数"), tr("单位"), tr("颜色")});
  m_curveTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_curveTable->verticalHeader()->hide();
  m_curveTable->setMaximumHeight(200);
  curveLay->addWidget(m_curveTable);

  auto *gridRow = new QHBoxLayout();
  m_gridCheck = new QCheckBox(tr("重叠网格"), m_curveGroup);
  m_gridCheck->setChecked(m_initial.showGrid());
  gridRow->addWidget(m_gridCheck);
  gridRow->addWidget(new QLabel(tr("密度:"), m_curveGroup));
  m_gridDensityCombo = new QComboBox(m_curveGroup);
  m_gridDensityCombo->addItem(tr("无"), 0);
  m_gridDensityCombo->addItem(tr("2 等分"), 1);
  m_gridDensityCombo->addItem(tr("4 等分"), 2);
  m_gridDensityCombo->addItem(tr("10 分含次网格"), 3);
  const int gIdx = m_gridDensityCombo->findData(m_initial.gridDensity());
  m_gridDensityCombo->setCurrentIndex(gIdx >= 0 ? gIdx : 2);
  gridRow->addWidget(m_gridDensityCombo);
  gridRow->addStretch(1);
  curveLay->addLayout(gridRow);

  root->addWidget(m_curveGroup);
  m_curveGroup->setVisible(isCurveFamily);

  populateCurveTable();

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);
}

void TrackConfigDialog::populateCurveTable()
{
  const QStringList initialNames = m_initial.curveNames();
  const QVariantMap overrides = m_initial.curveOverrides();

  m_curveTable->setRowCount(m_curvePool.size());
  for (int r = 0; r < m_curvePool.size(); ++r)
  {
    const CurveData &c = m_curvePool.at(r);
    const bool checked = initialNames.contains(c.name, Qt::CaseInsensitive);

    auto *checkItem = new QTableWidgetItem();
    checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    checkItem->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    m_curveTable->setItem(r, 0, checkItem);

    auto *nameItem = new QTableWidgetItem(c.name);
    nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_curveTable->setItem(r, 1, nameItem);

    auto *minSpin = new QDoubleSpinBox(m_curveTable);
    minSpin->setRange(-999999.0, 999999.0);
    minSpin->setDecimals(2);
    minSpin->setValue(c.minScale);
    m_curveTable->setCellWidget(r, 2, minSpin);

    auto *maxSpin = new QDoubleSpinBox(m_curveTable);
    maxSpin->setRange(-999999.0, 999999.0);
    maxSpin->setDecimals(2);
    maxSpin->setValue(c.maxScale);
    m_curveTable->setCellWidget(r, 3, maxSpin);

    auto *logCheck = new QCheckBox(m_curveTable);
    logCheck->setChecked(c.isLogarithmic);
    m_curveTable->setCellWidget(r, 4, logCheck);

    auto *unitEdit = new QLineEdit(c.unit, m_curveTable);
    m_curveTable->setCellWidget(r, 5, unitEdit);

    auto *colorBtn = new QPushButton(m_curveTable);
    const QColor effective = overrides.value(c.name).toMap().value(QStringLiteral("color")).toString().isEmpty()
                                 ? c.color
                                 : QColor(overrides.value(c.name).toMap().value(QStringLiteral("color")).toString());
    colorBtn->setText(effective.name());
    connect(colorBtn, &QPushButton::clicked, this, [this, colorBtn]() {
      const QColor picked = QColorDialog::getColor(QColor(colorBtn->text()), this, tr("曲线颜色"));
      if (picked.isValid())
        colorBtn->setText(picked.name());
    });
    m_curveTable->setCellWidget(r, 6, colorBtn);
  }
}

void TrackConfigDialog::applyPalette(int palette)
{
  int slot = 0;
  for (int r = 0; r < m_curveTable->rowCount(); ++r)
  {
    if (m_curveTable->item(r, 0)->checkState() != Qt::Checked)
      continue;
    if (auto *btn = qobject_cast<QPushButton *>(m_curveTable->cellWidget(r, 6)))
      btn->setText(paletteColor(palette, slot).name());
    ++slot;
  }
}

void TrackConfigDialog::setCurveRowChecked(int row, bool checked)
{
  if (row < 0 || row >= m_curveTable->rowCount())
    return;
  m_curveTable->item(row, 0)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

void TrackConfigDialog::accept()
{
  gatherResult();
  QDialog::accept();
}

void TrackConfigDialog::gatherResult()
{
  TrackSpec spec = m_initial;
  spec.title = m_titleEdit->text().trimmed().isEmpty() ? m_initial.title : m_titleEdit->text().trimmed();
  spec.width = m_widthSpin->value();
  spec.printIncluded = m_printCheck->isChecked();

  if (m_curveFamily)
  {
    QStringList names;
    QVariantMap overrides;
    for (int r = 0; r < m_curveTable->rowCount(); ++r)
    {
      if (m_curveTable->item(r, 0)->checkState() != Qt::Checked)
        continue;
      const QString name = m_curveTable->item(r, 1)->text();
      names << name;

      QVariantMap ov;
      const CurveData &src = m_curvePool.at(r);
      auto *minSpin = static_cast<QDoubleSpinBox *>(m_curveTable->cellWidget(r, 2));
      auto *maxSpin = static_cast<QDoubleSpinBox *>(m_curveTable->cellWidget(r, 3));
      auto *logCheck = static_cast<QCheckBox *>(m_curveTable->cellWidget(r, 4));
      auto *unitEdit = static_cast<QLineEdit *>(m_curveTable->cellWidget(r, 5));
      auto *colorBtn = static_cast<QPushButton *>(m_curveTable->cellWidget(r, 6));
      if (minSpin->value() != src.minScale)
        ov.insert(QStringLiteral("min"), minSpin->value());
      if (maxSpin->value() != src.maxScale)
        ov.insert(QStringLiteral("max"), maxSpin->value());
      if (logCheck->isChecked() != src.isLogarithmic)
        ov.insert(QStringLiteral("log"), logCheck->isChecked());
      if (unitEdit->text() != src.unit)
        ov.insert(QStringLiteral("unit"), unitEdit->text());
      const QColor col(colorBtn->text());
      if (col != src.color && col.isValid())
        ov.insert(QStringLiteral("color"), col.name());
      if (!ov.isEmpty())
        overrides.insert(name, ov);
    }
    spec.setCurveNames(names);
    spec.setCurveOverrides(overrides);
    spec.params.insert(QStringLiteral("showGrid"), m_gridCheck->isChecked());
    spec.params.insert(QStringLiteral("gridDensity"), m_gridDensityCombo->currentData().toInt());
  }

  m_result = spec;
}

} // namespace WellComposite
