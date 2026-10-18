// 层：视图
#include "mappingbasemapselector.h"
#include "mappingworkbenchpage.h"
#include "../paleotheme.h"
#include "../../workflow/mappingworkbench.h"
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
#include <QSet>
MappingBaseMapSelector::MappingBaseMapSelector(MappingWorkbench *workbench, QWidget *parent) : QWidget(parent), m_workbench(workbench) {
  setObjectName("mappingBaseMapSelector"); auto *root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0);
  auto *group = new QGroupBox(tr("1 · 选择基础图"), this); root->addWidget(group); auto *form = new QFormLayout(group);
  m_category = new QComboBox(group); m_category->setObjectName("baseMapCategory"); m_source = new QComboBox(group); m_source->setObjectName("baseMapSource");
  m_thresholds = new QLineEdit(group); m_thresholds->setObjectName("baseMapThresholds"); m_thresholds->setPlaceholderText(tr("按当前分类填写递增阈值，例如 10, 25"));
  form->addRow(tr("来源分类"), m_category); form->addRow(tr("本层位基础图"), m_source); form->addRow(tr("单因素分相阈值"), m_thresholds);
  m_show = new QPushButton(tr("采用并显示基础图"), group); m_start = new QPushButton(tr("以此建立编图成果"), group); form->addRow(m_show, m_start);
  m_hint = new QLabel(group); m_hint->setWordWrap(true); m_hint->setTextFormat(Qt::PlainText); form->addRow(m_hint);
  connect(m_category, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
  connect(m_source, &QComboBox::currentIndexChanged, this, [this] { updateState(); });
  connect(m_thresholds, &QLineEdit::textChanged, this, [this] { updateState(); });
  connect(m_show, &QPushButton::clicked, this, [this] { issue(false); }); connect(m_start, &QPushButton::clicked, this, [this] { issue(true); });
  connect(workbench, &MappingWorkbench::changed, this, [this] { refresh(); });
  refresh();
}
void MappingBaseMapSelector::setHorizon(const QString &horizon) { m_horizon = horizon; refresh(); }
void MappingBaseMapSelector::refresh() {
  const auto selectedCategory = m_category->currentData().toString(), selectedSource = m_source->currentData().toMap().value("id").toString();
  const QSignalBlocker categoryBlock(m_category), sourceBlock(m_source); const auto rows = m_workbench->baseMapCandidates(m_horizon);
  m_category->clear(); QSet<QString> categories;
  for (const auto &v : rows) { const auto row = v.toMap(); const auto key = row.value("category").toString(); if (!categories.contains(key)) { categories.insert(key); m_category->addItem(row.value("category_title").toString(), key); } }
  m_category->setCurrentIndex(qMax(0, m_category->findData(selectedCategory))); m_source->clear();
  for (const auto &v : rows) if (v.toMap().value("category") == m_category->currentData()) m_source->addItem(v.toMap().value("name").toString(), v);
  for (int i = 0; i < m_source->count(); ++i) if (m_source->itemData(i).toMap().value("id") == selectedSource) m_source->setCurrentIndex(i);
  updateState();
}
void MappingBaseMapSelector::updateState() {
  const auto row = m_source->currentData().toMap(); const auto action = row.value("start_action").toString();
  const bool continuous = action == "compose";
  if (auto *form = qobject_cast<QFormLayout *>(m_thresholds->parentWidget()->layout())) form->setRowVisible(m_thresholds, continuous);
  m_show->setEnabled(!row.isEmpty()); m_show->setToolTip(row.isEmpty() ? tr("请先导入古地理图或生成本层位的预测／单因素成果") : tr("显示所选图件作为编图基础"));
  bool valid = true; double previous = -std::numeric_limits<double>::infinity();
  const auto parts = m_thresholds->text().split(QRegularExpression("[,，;；\\s]+"), Qt::SkipEmptyParts);
  if (continuous) {
    valid = parts.size() == m_workbench->facies(m_horizon).size() - 1;
    for (const auto &part : parts) { bool ok; const auto number = part.toDouble(&ok); valid &= ok && std::isfinite(number) && number > previous; previous = number; }
  }
  m_start->setEnabled(!action.isEmpty() && valid);
  m_start->setToolTip(action.isEmpty() ? tr("此图可作为参考底图；相面编辑需要完整三级相分类和 facies_code 字段") : !valid ? tr("阈值数须为分类数减一，且为有限递增数值") : tr("生成独立成果版本，保留原始图件"));
  m_hint->setText(row.isEmpty() ? tr("请先在数据管理中导入本层位古地理图，或生成单因素／地震相图。") :
      action == "polygonize" ? tr("先将相栅格转为相面，完成后可复制相面进行编辑。") :
      continuous ? tr("单因素按当前相分类的编码顺序分相；不同因子分类列出，避免混用量纲。") :
      action.isEmpty() ? tr("当前图件作为空间参考；未登记三级相归属时不自动推定相类别。") : tr("复制为独立相图工作副本，原始图件保留。"));
}
void MappingBaseMapSelector::issue(bool start) {
  const auto row = m_source->currentData().toMap(); if (row.isEmpty()) return;
  const auto action = start ? row.value("start_action").toString() : QString("show"); if (action.isEmpty()) return;
  QVariantMap params{{"horizon", m_horizon}, {"layer", row.value("id")}, {"inputs", QStringList{row.value("id").toString()}}};
  if (action == "copy") params["inputs"] = QStringList{};
  if (action == "compose") { QVariantList values; for (const auto &p : m_thresholds->text().split(QRegularExpression("[,，;；\\s]+"), Qt::SkipEmptyParts)) values << p.toDouble(); params.insert("thresholds", values); }
  emit commandRequested(action, params);
}
void MappingWorkbenchPage::buildBaseMapStep(QWidget *body, QVBoxLayout *layout) {
  auto *selector = new MappingBaseMapSelector(m_workbench, body); layout->addWidget(selector);
  connect(this, &MappingWorkbenchPage::horizonChanged, selector, &MappingBaseMapSelector::setHorizon);
  connect(selector, &MappingBaseMapSelector::commandRequested, this, &MappingWorkbenchPage::commandRequested);
}
