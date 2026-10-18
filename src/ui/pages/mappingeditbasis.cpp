// 层：视图
#include "mappingeditbasis.h"
#include "../../workflow/mappingworkbench.h"
#include "../paleotheme.h"
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

MappingEditBasis::MappingEditBasis(MappingWorkbench *workbench, QWidget *parent)
    : QWidget(parent), m_workbench(workbench) {
  setObjectName("mappingEditBasis");
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  auto *group = new QGroupBox(tr("2 · 本次解释依据"), this);
  auto *layout = new QVBoxLayout(group);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  root->addWidget(group);
  auto *hint = new QLabel(tr("选择正在参考的证据，补充解释说明及修改目的后启用。"
                             "随后新建、改类和共边修改会自动记录这些依据；修改说明后需重新启用。"),
                          group);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *capture = new QPushButton(tr("选取当前显示的参考证据"), group);
  capture->setObjectName("workbench.captureEditEvidence");
  layout->addWidget(capture);
  connect(capture, &QPushButton::clicked, this,
          [this] { emit commandRequested("captureEditEvidence", {{"horizon", m_horizon}}); });
  m_sources = new QListWidget(group);
  m_sources->setObjectName("editBasisSources");
  m_sources->setAccessibleName(tr("本次解释引用的已保存版本"));
  m_sources->setMaximumHeight(96);
  layout->addWidget(m_sources);
  auto *remove = new QPushButton(tr("移除选中参考"), group);
  remove->setObjectName("editBasisRemoveSource");
  layout->addWidget(remove);
  connect(remove, &QPushButton::clicked, this, [this] {
    delete m_sources->takeItem(m_sources->currentRow());
    updateState();
  });
  m_text = new QPlainTextEdit(group);
  m_text->setObjectName("editBasisText");
  m_text->setAccessibleName(tr("本次解释说明"));
  m_text->setPlaceholderText(
      tr("描述井、地震或单因素的具体观察，以及为何支持此次解释；也可填写纯人工依据。"));
  m_text->setMaximumHeight(96);
  layout->addWidget(m_text);
  m_reason = new QLineEdit(group);
  m_reason->setObjectName("editBasisReason");
  m_reason->setAccessibleName(tr("本次修改目的"));
  m_reason->setPlaceholderText(tr("修改目的，例如：根据砂厚高值区调整河道边界"));
  layout->addWidget(m_reason);
  auto *apply = new QPushButton(tr("启用本次解释依据"), group);
  apply->setObjectName("workbench.setEditEvidence");
  layout->addWidget(apply);
  connect(apply, &QPushButton::clicked, this, [this] {
    QStringList versions;
    for (int i = 0; i < m_sources->count(); ++i)
      versions << m_sources->item(i)->data(Qt::UserRole).toString();
    emit commandRequested("setEditEvidence", {{"horizon", m_horizon},
                                              {"text", m_text->toPlainText()},
                                              {"reason", m_reason->text()},
                                              {"source_versions", versions}});
  });
  m_state = new QLabel(group);
  m_state->setObjectName("editBasisState");
  m_state->setWordWrap(true);
  m_state->setTextFormat(Qt::PlainText);
  layout->addWidget(m_state);
  connect(m_text, &QPlainTextEdit::textChanged, this, &MappingEditBasis::updateState);
  connect(m_reason, &QLineEdit::textChanged, this, &MappingEditBasis::updateState);
  connect(m_sources, &QListWidget::currentRowChanged, this, &MappingEditBasis::updateState);
  connect(workbench, &MappingWorkbench::editEvidenceChanged, this, &MappingEditBasis::updateState);
  connect(workbench, &MappingWorkbench::catalogBound, this, [this] {
    m_sources->clear();
    m_text->clear();
    m_reason->clear();
    updateState();
  });
  updateState();
}
void MappingEditBasis::setHorizon(const QString &horizon) {
  if (m_horizon != horizon) {
    m_workbench->clearEditEvidence();
    m_sources->clear();
    m_text->clear();
    m_reason->clear();
  }
  m_horizon = horizon;
  updateState();
}
void MappingEditBasis::setReferences(const QVariantList &references) {
  m_sources->clear();
  for (const auto &value : references) {
    const auto row = value.toMap();
    auto *item = new QListWidgetItem(row.value("name").toString(), m_sources);
    item->setData(Qt::UserRole, row.value("version_id"));
    item->setToolTip(row.value("version_id").toString());
  }
  updateState();
}
void MappingEditBasis::updateState() {
  m_sources->setVisible(m_sources->count() > 0);
  if (auto *remove = findChild<QPushButton *>("editBasisRemoveSource")) {
    remove->setVisible(m_sources->count() > 0);
    remove->setEnabled(m_sources->currentRow() >= 0);
  }
  if (auto *apply = findChild<QPushButton *>("workbench.setEditEvidence")) {
    const bool valid = !m_horizon.isEmpty() && !m_text->toPlainText().trimmed().isEmpty() &&
                       !m_reason->text().trimmed().isEmpty() &&
                       m_text->toPlainText().size() + m_reason->text().size() <= 9000;
    apply->setEnabled(valid);
    apply->setToolTip(valid ? tr("将本次依据用于后续元素修改")
                            : tr("请先选择层位，填写解释说明和修改目的，合计不超过 9000 字"));
  }
  const auto current = m_workbench->editEvidence(m_horizon);
  QStringList versions;
  for (int i = 0; i < m_sources->count(); ++i)
    versions << m_sources->item(i)->data(Qt::UserRole).toString();
  const bool same = !current.isEmpty() &&
                    current.value("text").toString() == m_text->toPlainText().trimmed() &&
                    current.value("reason").toString() == m_reason->text().trimmed() &&
                    current.value("source_versions").toStringList() == versions;
  m_state->setText(current.isEmpty() ? tr("尚未启用依据；请先启用，再绘制或修改元素。")
                   : same ? tr("已启用：%1 个参考版本及人工说明。解释仍需复核，参考图不自动视为地质支持。 ")
                                .arg(current.value("references").toList().size())
                          : tr("表单已改动；当前编辑仍采用上一次启用的依据，请重新启用。"));
}
