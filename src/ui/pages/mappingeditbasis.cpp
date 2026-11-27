// 层：视图
#include "mappingeditbasis.h"
#include "../../workflow/mappingworkbench.h"
#include "../paleotheme.h"
#include "../windowsizing_internal.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

MappingEditBasis::MappingEditBasis(MappingWorkbench *workbench, QWidget *parent)
    : QWidget(parent), m_workbench(workbench) {
  setObjectName(QStringLiteral("mappingEditBasis"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  auto *group = new QGroupBox(this);
  auto *outer = new QVBoxLayout(group);
  outer->setSpacing(PaleoTheme::tokens().spacingXs);
  root->addWidget(group, 1);

  m_toggle = new QToolButton(group);
  m_toggle->setObjectName(QStringLiteral("editBasisToggle"));
  m_toggle->setText(tr("可选 · 解释说明与后台自动生成"));
  m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_toggle->setArrowType(Qt::RightArrow);
  m_toggle->setCheckable(true);
  m_toggle->setChecked(false);
  outer->addWidget(m_toggle);

  m_autoStatus = new QLabel(group);
  m_autoStatus->setObjectName(QStringLiteral("editBasisAutoStatus"));
  m_autoStatus->setWordWrap(true);
  m_autoStatus->setTextFormat(Qt::PlainText);
  m_autoStatus->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(m_autoStatus, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  outer->addWidget(m_autoStatus);

  m_content = new QWidget(group);
  auto *layout = new QVBoxLayout(m_content);
  layout->setContentsMargins(0, PaleoTheme::tokens().spacingXs, 0, 0);
  layout->setSpacing(PaleoTheme::tokens().spacingSm);
  outer->addWidget(m_content, 1);
  m_content->setVisible(false);

  connect(m_toggle, &QToolButton::toggled, this, [this](bool expanded) {
    m_content->setVisible(expanded);
    m_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  });

  buildAutoSection(m_content, layout);
  buildManualSection(m_content, layout);

  connect(workbench, &MappingWorkbench::editEvidenceChanged, this, &MappingEditBasis::updateState);
  connect(workbench, &MappingWorkbench::explanationStatusChanged, this, &MappingEditBasis::updateState);
  connect(workbench, &MappingWorkbench::autoExplanationReady, this, &MappingEditBasis::onAutoExplanationReady);
  connect(workbench, &MappingWorkbench::catalogBound, this, [this] {
    m_forms.clear();
    m_layerId.clear();
    m_featureUid.clear();
    m_sources->clear();
    m_text->clear();
    m_reason->clear();
    m_latestDraft.clear();
    m_formEdited = false;
    updateState();
  });
  updateState();
}

void MappingEditBasis::buildAutoSection(QWidget *parent, QVBoxLayout *layout) {
  auto *hint = new QLabel(
      tr("编图过程不必需先写解释。绘制、分割、合并或改类后，系统会在独立后台线程自动生成解释草稿"
         "（已配置 AI 时由 AI 生成，未配置时生成本地事实摘要），不影响继续编图。"),
      parent);
  hint->setWordWrap(true);
  hint->setTextFormat(Qt::PlainText);
  layout->addWidget(hint);

  m_autoToggle = new QCheckBox(tr("编图时后台自动生成解释（有 AI 时由 AI 生成）"), parent);
  m_autoToggle->setObjectName(QStringLiteral("editBasisAutoToggle"));
  m_autoToggle->setChecked(m_workbench && m_workbench->autoExplainEnabled());
  layout->addWidget(m_autoToggle);
  connect(m_autoToggle, &QCheckBox::toggled, this, [this](bool checked) {
    emit commandRequested(QStringLiteral("setAutoExplain"), {{QStringLiteral("enabled"), checked}});
  });

  auto *autoActions = new QHBoxLayout;
  m_generateBtn = new QPushButton(tr("立即后台生成说明"), parent);
  m_generateBtn->setObjectName(QStringLiteral("workbench.generateEditExplanation"));
  m_cancelBtn = new QPushButton(tr("取消生成"), parent);
  m_cancelBtn->setObjectName(QStringLiteral("workbench.cancelEditExplanation"));
  autoActions->addWidget(m_generateBtn, 1);
  autoActions->addWidget(m_cancelBtn);
  layout->addLayout(autoActions);

  connect(m_generateBtn, &QPushButton::clicked, this, [this] {
    QStringList versions;
    for (int i = 0; i < m_sources->count(); ++i)
      versions << m_sources->item(i)->data(Qt::UserRole).toString();
    emit commandRequested(QStringLiteral("generateEditExplanation"),
                          {{QStringLiteral("layer"), m_layerId}, {QStringLiteral("horizon"), m_horizon},
                           {QStringLiteral("source_versions"), versions},
                           {QStringLiteral("apply_to_edit_basis"), true}});
  });
  connect(m_cancelBtn, &QPushButton::clicked, this, [this] {
    emit commandRequested(QStringLiteral("cancelEditExplanation"), {{QStringLiteral("horizon"), m_horizon}});
  });

  m_draftPreview = new QLabel(parent);
  m_draftPreview->setObjectName(QStringLiteral("editBasisAutoPreview"));
  m_draftPreview->setWordWrap(true);
  m_draftPreview->setTextFormat(Qt::PlainText);
  m_draftPreview->hide();
  layout->addWidget(m_draftPreview);

  m_adoptBtn = new QPushButton(tr("将后台草稿填入下方表单"), parent);
  m_adoptBtn->setObjectName(QStringLiteral("editBasisAdoptDraft"));
  m_adoptBtn->hide();
  layout->addWidget(m_adoptBtn);
  connect(m_adoptBtn, &QPushButton::clicked, this, [this] {
    const auto draft = m_latestDraft;
    emit commandRequested(QStringLiteral("adoptEditExplanation"),
                          {{"layer", m_layerId}, {"feature_uid", m_featureUid}, {"request_id", draft.value("request_id")}});
    if (m_layerId == draft.value("layer_id").toString() && m_featureUid == draft.value("feature_uid").toString())
      applyDraftToForm(draft);
  });
}

void MappingEditBasis::buildManualSection(QWidget *parent, QVBoxLayout *layout) {
  auto *capture = new QPushButton(tr("选取当前显示的参考证据"), parent);
  capture->setObjectName(QStringLiteral("workbench.captureEditEvidence"));
  layout->addWidget(capture);
  connect(capture, &QPushButton::clicked, this, [this] {
    emit commandRequested(QStringLiteral("captureEditEvidence"), {{QStringLiteral("horizon"), m_horizon}});
  });

  m_sources = new QListWidget(parent);
  m_sources->setObjectName(QStringLiteral("editBasisSources"));
  m_sources->setAccessibleName(tr("本次解释引用的已保存版本"));
  m_sources->setMinimumHeight(paleo::ui_detail::lineRows(m_sources, 3));
  layout->addWidget(m_sources, 1);

  auto *remove = new QPushButton(tr("移除选中参考"), parent);
  remove->setObjectName(QStringLiteral("editBasisRemoveSource"));
  layout->addWidget(remove);
  connect(remove, &QPushButton::clicked, this, [this] {
    delete m_sources->takeItem(m_sources->currentRow());
    updateState();
  });

  m_text = new QPlainTextEdit(parent);
  m_text->setObjectName(QStringLiteral("editBasisText"));
  m_text->setAccessibleName(tr("本次解释说明"));
  m_text->setPlaceholderText(
      tr("可选：补充井、地震或单因素观察；留空也可直接编图，修改后由后台自动生成草稿。"));
  m_text->setMinimumHeight(paleo::ui_detail::lineRows(m_text, 3));

  auto *form = new QFormLayout;
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->addRow(tr("解释说明"), m_text);

  m_reason = new QLineEdit(parent);
  m_reason->setObjectName(QStringLiteral("editBasisReason"));
  m_reason->setAccessibleName(tr("本次修改目的"));
  m_reason->setPlaceholderText(tr("可选：例如根据砂厚高值区调整河道边界"));
  form->addRow(tr("修改目的"), m_reason);
  layout->addLayout(form, 2);

  auto *manualActions = new QHBoxLayout;
  auto *feature = new QPushButton(tr("给当前图斑加备注"), parent);
  feature->setObjectName(QStringLiteral("editBasisFeatureNote"));
  layout->addWidget(feature);
  connect(feature, &QPushButton::clicked, this, [this] { sendManualNote(true); });
  auto *apply = new QPushButton(tr("后续编辑沿用这条说明"), parent);
  apply->setObjectName(QStringLiteral("workbench.setEditEvidence"));
  auto *clear = new QPushButton(tr("清空手动依据"), parent);
  clear->setObjectName(QStringLiteral("workbench.clearEditEvidence"));
  manualActions->addWidget(apply, 1);
  manualActions->addWidget(clear);
  layout->addLayout(manualActions);

  connect(apply, &QPushButton::clicked, this, [this] { sendManualNote(false); });
  connect(clear, &QPushButton::clicked, this, [this] {
    emit commandRequested(QStringLiteral("clearEditEvidence"), {{"horizon", m_horizon}, {"layer", m_layerId}});
  });

  m_state = new QLabel(parent);
  m_state->setObjectName(QStringLiteral("editBasisState"));
  m_state->setWordWrap(true);
  m_state->setTextFormat(Qt::PlainText);
  layout->addWidget(m_state);

  const auto onFormEdited = [this] {
    m_formEdited = true;
    updateState();
  };
  connect(m_text, &QPlainTextEdit::textChanged, this, onFormEdited);
  connect(m_reason, &QLineEdit::textChanged, this, onFormEdited);
  connect(m_sources, &QListWidget::currentRowChanged, this, &MappingEditBasis::updateState);
}

QVariantList MappingEditBasis::references() const {
  QVariantList result;
  for (int i = 0; i < m_sources->count(); ++i) {
    const auto *item = m_sources->item(i);
    result << QVariantMap{{"name", item->text()}, {"version_id", item->data(Qt::UserRole)}};
  }
  return result;
}

void MappingEditBasis::sendManualNote(bool featureOnly) {
  QVariantMap data{{"horizon", m_horizon}, {"layer", m_layerId}, {"text", m_text->toPlainText()},
                   {"reason", m_reason->text()}};
  if (featureOnly) {
    if (m_featureUid.isEmpty()) return;
    data.insert("type", "manual");
    data.insert("role", "SUPPORTED_BY");
    data.insert("target_feature_uid", m_featureUid);
    data.insert("target_version", m_workbench->versionForLayer(m_layerId).id);
    if (m_reason->text().trimmed().isEmpty()) data.insert("reason", m_text->toPlainText());
    emit commandRequested(QStringLiteral("addEvidence"), data);
  } else {
    QStringList versions;
    for (const auto &source : references()) versions << source.toMap().value("version_id").toString();
    data.insert("source_versions", versions);
    emit commandRequested(QStringLiteral("setEditEvidence"), data);
  }
}

void MappingEditBasis::setExpanded(bool expanded) {
  if (m_toggle)
    m_toggle->setChecked(expanded);
}

bool MappingEditBasis::isExpanded() const {
  return m_toggle && m_toggle->isChecked();
}

void MappingEditBasis::setHorizon(const QString &horizon) {
  if (m_horizon == horizon) return;
  m_workbench->clearEditEvidence();
  setTarget({}, {});
  m_horizon = horizon;
  m_text->clear();
  m_reason->clear();
  m_sources->clear();
  m_formEdited = false;
  updateState();
}

void MappingEditBasis::setTarget(const QString &id, const QString &featureUid) {
  if (m_layerId == id && m_featureUid == featureUid) return;
  QSignalBlocker text(m_text), reason(m_reason), sources(m_sources);
  const bool changedLayer = m_layerId != id;
  if (changedLayer && !m_layerId.isEmpty())
    m_forms.insert(m_layerId, {m_text->toPlainText(), m_reason->text(), m_featureUid, references(), m_formEdited});
  m_layerId = id;
  m_featureUid = featureUid;
  if (changedLayer) {
    const auto form = m_forms.value(id);
    const bool restore = form.edited || featureUid == form.featureUid;
    m_text->setPlainText(restore ? form.text : QString());
    m_reason->setText(restore ? form.reason : QString());
    m_formEdited = form.edited;
    setReferences(form.sources);
  } else if (!m_formEdited) {
    m_text->clear();
    m_reason->clear();
  }
  m_latestDraft.clear();
  updateState();
}

void MappingEditBasis::setReferences(const QVariantList &references) {
  m_sources->clear();
  for (const auto &value : references) {
    const auto row = value.toMap();
    auto *item = new QListWidgetItem(row.value(QStringLiteral("name")).toString(), m_sources);
    item->setData(Qt::UserRole, row.value(QStringLiteral("version_id")));
    item->setToolTip(row.value(QStringLiteral("version_id")).toString());
  }
  updateState();
}

void MappingEditBasis::applyDraftToForm(const QVariantMap &draft) {
  if (draft.isEmpty())
    return;
  const QString text = draft.value(QStringLiteral("text")).toString().trimmed();
  const QString reason = draft.value(QStringLiteral("reason")).toString().trimmed();
  m_text->setPlainText(text);
  m_reason->setText(reason);
  m_formEdited = false;
  updateState();
}

void MappingEditBasis::onAutoExplanationReady(const QString &horizon, const QVariantMap &explanation) {
  if (horizon != m_horizon || explanation.value("layer_id").toString() != m_layerId ||
      explanation.value("feature_uid").toString() != m_featureUid)
    return;
  m_latestDraft = explanation;
  if (explanation.value(QStringLiteral("apply_to_edit_basis")).toBool() && !m_formEdited)
    applyDraftToForm(explanation);
  else
    updateState();
}

void MappingEditBasis::updateState() {
  m_sources->setVisible(m_sources->count() > 0);
  if (auto *remove = findChild<QPushButton *>(QStringLiteral("editBasisRemoveSource"))) {
    remove->setVisible(m_sources->count() > 0);
    remove->setEnabled(m_sources->currentRow() >= 0);
  }
  if (m_autoToggle) {
    const QSignalBlocker block(m_autoToggle);
    m_autoToggle->setChecked(m_workbench->autoExplainEnabled());
  }
  const bool busy = m_workbench->explanationBusy();
  if (m_generateBtn) {
    m_generateBtn->setEnabled(!m_horizon.isEmpty());
    m_generateBtn->setToolTip(m_horizon.isEmpty()
                                  ? tr("请先选择编图层位")
                                  : m_workbench->aiExplanationAvailable()
                                        ? tr("在独立后台线程调用已配置 AI 生成编图解释草稿")
                                        : tr("在独立后台线程根据当前图件与井资料汇总事实草稿"));
  }
  if (m_cancelBtn) {
    m_cancelBtn->setEnabled(busy);
    m_cancelBtn->setToolTip(busy ? tr("取消当前后台解释生成") : tr("当前没有进行中的后台解释任务"));
  }
  renderDraft();
  const auto current = m_layerId.isEmpty() ? m_workbench->editEvidence(m_horizon)
                                           : m_workbench->editEvidenceForLayer(m_layerId);
  if (auto *apply = findChild<QPushButton *>(QStringLiteral("workbench.setEditEvidence"))) {
    const bool valid = !m_horizon.isEmpty() && !m_text->toPlainText().trimmed().isEmpty() &&
                       m_text->toPlainText().size() + m_reason->text().size() <= 9000;
    const bool writable = m_layerId.isEmpty() || m_layerId.startsWith("draft.");
    apply->setEnabled(valid && writable);
    if (auto *feature = findChild<QPushButton *>(QStringLiteral("editBasisFeatureNote")))
      feature->setEnabled(valid && writable && !m_featureUid.isEmpty());
    apply->setToolTip(valid ? tr("将手动说明优先用于后续相面修改")
                            : tr("填写解释说明即可启用，修改目的可选（合计不超过 9000 字）"));
  }
  if (auto *clear = findChild<QPushButton *>(QStringLiteral("workbench.clearEditEvidence"))) {
    clear->setEnabled(!current.isEmpty());
    clear->setToolTip(current.isEmpty() ? tr("当前未启用手动依据")
                                        : tr("清空手动依据，恢复后台自动生成模式"));
  }
  QStringList versions;
  for (int i = 0; i < m_sources->count(); ++i)
    versions << m_sources->item(i)->data(Qt::UserRole).toString();
  const bool same = !current.isEmpty() &&
                    current.value(QStringLiteral("text")).toString() == m_text->toPlainText().trimmed() &&
                    current.value(QStringLiteral("reason")).toString() == m_reason->text().trimmed() &&
                    current.value(QStringLiteral("source_versions")).toStringList() == versions;
  m_state->setText(current.isEmpty()
                       ? tr("编图不必需手动填写依据。直接绘制、分割或合并相面即可，修改后会在后台自动生成解释草稿。")
                   : same ? tr("已启用手动依据（%1 个参考版本）。后续编辑优先记录此人工说明；解释仍需复核。")
                                .arg(current.value(QStringLiteral("references")).toList().size())
                          : tr("手动依据表单已改动；当前编辑仍沿用上次启用的依据，重新启用后生效。"));
}

void MappingEditBasis::renderDraft() {
  const auto latest = m_layerId.isEmpty() ? m_workbench->lastAutoExplanation(m_horizon)
                                          : m_workbench->explanationForTarget(m_layerId, m_featureUid);
  const bool belongs = latest.value("layer_id").toString() == m_layerId && latest.value("feature_uid").toString() == m_featureUid;
  if ((!belongs || latest.isEmpty()) && !m_latestDraft.isEmpty() && !m_formEdited &&
      m_text->toPlainText() == m_latestDraft.value(QStringLiteral("text")).toString() &&
      m_reason->text() == m_latestDraft.value(QStringLiteral("reason")).toString()) {
    QSignalBlocker blockText(m_text), blockReason(m_reason);
    m_text->clear();
    m_reason->clear();
  }
  m_latestDraft = belongs ? latest : QVariantMap{};
  const QString statusLine = m_workbench->explanationStatus();
  if (!m_latestDraft.isEmpty()) {
    const bool ai = m_latestDraft.value(QStringLiteral("ai_generated")).toBool();
    const QString model = m_latestDraft.value(QStringLiteral("model")).toString();
    const QString fallback = m_latestDraft.value(QStringLiteral("fallback_reason")).toString();
    const QString badge = ai && !model.isEmpty()
                              ? tr("AI 草稿已生成（%1 · 待复核）").arg(model)
                              : ai ? tr("AI 草稿已生成（待复核）")
                                   : tr("后台事实摘要已生成（待复核）");
    m_autoStatus->setText(fallback.isEmpty() ? tr("%1 · %2").arg(badge, statusLine)
                                             : tr("%1（%2）").arg(badge, fallback));
    const QString owner = m_featureUid.isEmpty() ? m_workbench->declaration(m_layerId).title
                                                  : tr("%1 · 图斑 %2").arg(m_workbench->declaration(m_layerId).title, m_featureUid);
    const auto saved = m_latestDraft.value("saved_version");
    const QString location = saved.isValid() ? tr("已随 v%1 保存").arg(saved.toInt()) : tr("工作副本草稿");
    m_draftPreview->setText(tr("%1 · %2\n%3").arg(owner, location, m_latestDraft.value("text").toString()));
    m_draftPreview->show();
    m_adoptBtn->show();
  } else {
    m_autoStatus->setText(statusLine);
    m_draftPreview->hide();
    m_adoptBtn->hide();
  }

}
