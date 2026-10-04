// 层：视图
#include "ui/paleotheme.h"
#include "sectionsetupdialog.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

SectionSetupDialog::SectionSetupDialog(QWidget *parent) : QDialog(parent) {
  setObjectName("sectionSetupDialog");
  setWindowTitle(tr("连井剖面与时深对齐"));
  resize(760, 600);
  auto *layout = new QVBoxLayout(this);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *hint = new QLabel(tr("井口、地震道头与地图须使用同一米制坐标。勾选井并"
                             "拖动排序，或直接在地图绘制折线。"),
                          this);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *columns = new QHBoxLayout;
  layout->addLayout(columns, 1);
  auto *left = new QVBoxLayout;
  columns->addLayout(left, 1);
  left->addWidget(new QLabel(tr("连井顺序（拖动调整）"), this));
  m_wells = new QListWidget(this);
  m_wells->setObjectName("sectionWells");
  m_wells->setDragDropMode(QAbstractItemView::InternalMove);
  left->addWidget(m_wells, 1);
  auto *order = new QHBoxLayout;
  left->addLayout(order);
  for (bool up : {true, false}) {
    auto *button = new QPushButton(up ? tr("上移") : tr("下移"), this);
    order->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, up] {
      int i = m_wells->currentRow(), j = i + (up ? -1 : 1);
      if (i < 0 || j < 0 || j >= m_wells->count())
        return;
      auto *item = m_wells->takeItem(i);
      m_wells->insertItem(j, item);
      m_wells->setCurrentItem(item);
    });
  }
  auto *build = new QPushButton(tr("按顺序生成连井剖面"), this);
  build->setObjectName("buildWellSection");
  left->addWidget(build);
  auto *draw = new QPushButton(tr("地图绘制任意折线"), this);
  draw->setObjectName("drawArbitrarySection");
  left->addWidget(draw);
  auto *clear = new QPushButton(tr("清除剖面连线"), this);
  clear->setObjectName("clearSectionRoute");
  clear->setToolTip(tr("移除地图上的剖面路线；剖面视图保留至下一次提取"));
  left->addWidget(clear);
  auto *right = new QGroupBox(tr("选中井 · 时深对齐"), this);
  columns->addWidget(right, 2);
  auto *form = new QFormLayout(right);
  m_status = new QLabel(right);
  m_status->setWordWrap(true);
  form->addRow(m_status);
  m_mode = new QComboBox(right);
  m_mode->setObjectName("alignmentMode");
  m_mode->addItems({tr("使用本井时深表（不外推）"), tr("常速近似（需复核）")});
  form->addRow(tr("对齐方式"), m_mode);
  m_velocity = new QDoubleSpinBox(right);
  m_velocity->setRange(101, 19999);
  m_velocity->setValue(2500);
  m_velocity->setSuffix(" m/s");
  m_velocity->setObjectName("alignmentVelocity");
  form->addRow(tr("近似速度"), m_velocity);
  m_shift = new QDoubleSpinBox(right);
  m_shift->setRange(-10000, 10000);
  m_shift->setDecimals(2);
  m_shift->setSuffix(" ms");
  m_shift->setObjectName("alignmentShift");
  form->addRow(tr("整体时间平移"), m_shift);
  auto *apply = new QPushButton(tr("应用到当前剖面"), right);
  apply->setObjectName("applyAlignment");
  form->addRow(apply);
  auto *note = new QLabel(tr("正值向下平移。分层优先 TVD，测井按 "
                             "MD；缺表或超出范围不绘制。左侧深度尺仅为公共常速"
                             "参考，地震图像仍按 TWT 排列。"),
                          right);
  note->setWordWrap(true);
  form->addRow(note);
  m_samples = new QTableWidget(0, 3, right);
  m_samples->setObjectName("alignmentSamples");
  m_samples->setHorizontalHeaderLabels(
      {tr("TVD (m)"), tr("MD (m)"), tr("TWT (ms)")});
  m_samples->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_samples->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  form->addRow(m_samples);
  auto *saveRow = new QHBoxLayout;
  layout->addLayout(saveRow);
  m_name = new QLineEdit(this);
  m_name->setPlaceholderText(tr("剖面名称"));
  m_name->setObjectName("sectionName");
  saveRow->addWidget(m_name);
  auto *save = new QPushButton(tr("保存路线与对齐新版本"), this);
  save->setObjectName("saveSectionSession");
  saveRow->addWidget(save);
  auto *loadRow = new QHBoxLayout;
  layout->addLayout(loadRow);
  m_saved = new QComboBox(this);
  m_saved->setObjectName("savedSections");
  loadRow->addWidget(m_saved, 1);
  auto *load = new QPushButton(tr("恢复剖面版本"), this);
  load->setObjectName("restoreSectionSession");
  loadRow->addWidget(load);
  m_message = new QLabel(this);
  m_message->setObjectName("sectionSetupMessage");
  m_message->setWordWrap(true);
  layout->addWidget(m_message);
  auto gateBuild = [this, build] {
    int selected = 0;
    for (int i = 0; i < m_wells->count(); ++i)
      selected += m_wells->item(i)->checkState() == Qt::Checked;
    build->setEnabled(selected >= 2);
    build->setToolTip(selected < 2 ? tr("至少勾选两口坐标不同的井")
                                   : tr("按列表从上到下生成剖面"));
  };
  connect(m_wells, &QListWidget::itemChanged, this, gateBuild);
  gateBuild();
  connect(m_wells, &QListWidget::currentRowChanged, this,
          &SectionSetupDialog::showWell);
  connect(m_mode, &QComboBox::currentIndexChanged, this,
          [this](int i) { m_velocity->setEnabled(i == 1); });
  connect(build, &QPushButton::clicked, this, [this] {
    QStringList ids;
    for (int i = 0; i < m_wells->count(); ++i)
      if (m_wells->item(i)->checkState() == Qt::Checked)
        ids << m_wells->item(i)
                   ->data(Qt::UserRole)
                   .toMap()
                   .value("id")
                   .toString();
    emit buildRequested(ids);
  });
  connect(draw, &QPushButton::clicked, this,
          &SectionSetupDialog::drawRequested);
  connect(clear, &QPushButton::clicked, this,
          &SectionSetupDialog::clearRequested);
  connect(apply, &QPushButton::clicked, this, [this] {
    if (auto *item = m_wells->currentItem())
      emit calibrationRequested(
          item->data(Qt::UserRole).toMap().value("id").toString(),
          m_mode->currentIndex() == 1, m_velocity->value(), m_shift->value());
  });
  connect(save, &QPushButton::clicked, this,
          [this] { emit saveRequested(m_name->text()); });
  connect(load, &QPushButton::clicked, this,
          [this] { emit restoreRequested(m_saved->currentData().toString()); });
}
void SectionSetupDialog::setWells(const QVariantList &wells) {
  QString selected;
  QStringList order, checked;
  for (int i = 0; i < m_wells->count(); ++i) {
    auto *item = m_wells->item(i);
    const auto id = item->data(Qt::UserRole).toMap().value("id").toString();
    order << id;
    if (item->checkState() == Qt::Checked)
      checked << id;
    if (item == m_wells->currentItem())
      selected = id;
  }
  QMap<QString, QVariantMap> byId;
  for (const auto &w : wells) {
    auto row = w.toMap();
    auto id = row.value("id").toString();
    byId[id] = row;
    if (!order.contains(id))
      order << id;
  }
  const QSignalBlocker block(m_wells);
  m_wells->clear();
  for (const auto &id : order) {
    if (!byId.contains(id))
      continue;
    const auto row = byId.value(id);
    auto *item = new QListWidgetItem(row.value("name").toString(), m_wells);
    item->setData(Qt::UserRole, row);
    item->setToolTip(row.value("status").toString());
    item->setCheckState(checked.contains(id) ? Qt::Checked : Qt::Unchecked);
    if (!row.value("coordinates").toBool()) {
      item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
      item->setToolTip(tr("缺少有效井口坐标"));
    }
    if (id == selected)
      m_wells->setCurrentItem(item);
  }
  if (!m_wells->currentItem() && m_wells->count())
    m_wells->setCurrentRow(0);
  showWell();
  int checkedCount = 0;
  for (int i = 0; i < m_wells->count(); ++i)
    checkedCount += m_wells->item(i)->checkState() == Qt::Checked;
  findChild<QPushButton *>("buildWellSection")->setEnabled(checkedCount >= 2);
}
void SectionSetupDialog::setSavedSections(const QVariantList &sections) {
  m_saved->clear();
  findChild<QPushButton *>("restoreSectionSession")
      ->setEnabled(!sections.isEmpty());
  m_saved->setToolTip(sections.isEmpty() ? tr("工程尚无已保存的剖面版本")
                                         : QString());
  for (const auto &v : sections) {
    auto row = v.toMap();
    m_saved->addItem(row.value("name").toString(), row.value("id"));
  }
}
void SectionSetupDialog::setMessage(const QString &text) {
  m_message->setText(text);
}
void SectionSetupDialog::showWell() {
  const auto row = m_wells->currentItem()
                       ? m_wells->currentItem()->data(Qt::UserRole).toMap()
                       : QVariantMap();
  findChild<QPushButton *>("applyAlignment")->setEnabled(!row.isEmpty());
  m_velocity->setToolTip(tr("仅常速近似模式使用此速度"));
  m_status->setText(row.isEmpty() ? tr("请先导入井数据及关联时深表")
                                  : row.value("name").toString() + " · " +
                                        row.value("status").toString());
  m_mode->setCurrentIndex(row.value("constant").toBool() ? 1 : 0);
  m_velocity->setValue(row.value("velocity", 2500).toDouble());
  m_velocity->setEnabled(m_mode->currentIndex() == 1);
  m_shift->setValue(row.value("shift").toDouble());
  const auto samples = row.value("samples").toList();
  m_samples->setRowCount(samples.size());
  const QStringList keys{"tvd", "md", "time"};
  for (int r = 0; r < samples.size(); ++r)
    for (int c = 0; c < 3; ++c) {
      const double v = samples[r].toMap().value(keys[c]).toDouble();
      m_samples->setItem(r, c,
                         new QTableWidgetItem(std::isfinite(v)
                                                  ? QString::number(v, 'f', 2)
                                                  : tr("缺失")));
    }
}
