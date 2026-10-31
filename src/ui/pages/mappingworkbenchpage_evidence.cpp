// 层：视图
#include "../windowsizing_internal.h"
#include "mappingworkbenchpage.h"
#include "pageshared.h"
#include "../../workflow/mappingworkbench.h"
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTabWidget>

QPushButton *MappingWorkbenchPage::evidenceButton(const QString &name, const QString &text, QWidget *parent) {
  auto *button = new QPushButton(text, parent);
  button->setObjectName("workbench." + name);
  button->setAccessibleName(text);
  connect(button, &QPushButton::clicked, this, [this, name] { issue(name); });
  return button;
}
void MappingWorkbenchPage::buildEvidenceSection(QWidget *parent, QVBoxLayout *layout) {
  auto *summary = new QLabel(tr("在地图中选择图斑，选择资料并写下解释即可保存。"), parent);
  summary->setObjectName("faciesEvidenceSelectionSummary"); summary->setWordWrap(true);
  layout->addWidget(summary);
  buildEvidenceEditor(parent, layout);
  m_evidence = new QListWidget(parent);
  m_evidence->setObjectName("faciesEvidenceList");
  m_evidence->setAccessibleName(tr("解释证据列表"));
  m_evidence->setMinimumHeight(paleo::ui_detail::lineRows(m_evidence, 4)); // R6 #836：不再封顶，随 dock 增高
  layout->addWidget(new QLabel(tr("已保存的证据（选择后可修订）"), parent));
  layout->addWidget(m_evidence, 2);
  auto *navigation = new QHBoxLayout;
  navigation->addWidget(evidenceButton("locateEvidence", tr("定位来源"), parent));
  navigation->addWidget(evidenceButton("removeEvidence", tr("移除证据"), parent));
  layout->addLayout(navigation);
  auto *advanced = new paleo::pagesinternal::CollapsibleSection(tr("更多详情与核验"), parent);
  advanced->setObjectName("evidenceAdvancedSection"); advanced->setExpanded(false);
  auto *tabs = new QTabWidget(advanced->container());
  tabs->setObjectName("faciesEvidenceTabs");
  advanced->containerLayout()->addWidget(tabs);
  buildEvidenceSources(parent, tabs); buildEvidenceReview(parent, tabs); buildEvidenceHistory(parent, tabs);
  layout->addWidget(advanced);
  wireEvidenceControls();
  connect(advanced->findChild<QPushButton *>("collapsibleSectionToggle"), &QPushButton::toggled,
          this, [this](bool expanded) { emit evidenceDetailsRequested(expanded); updateState(); });
}
bool MappingWorkbenchPage::evidenceDetailsVisible() const {
  const auto *section = findChild<QWidget *>("evidenceAdvancedSection");
  const auto *toggle = section ? section->findChild<QPushButton *>("collapsibleSectionToggle") : nullptr;
  return toggle && toggle->isChecked();
}

void MappingWorkbenchPage::buildEvidenceEditor(QWidget *parent, QVBoxLayout *layout) {
  m_evidenceSource = new QComboBox(parent);
  m_evidenceSource->setObjectName("faciesEvidenceSource");
  m_evidenceSource->setAccessibleName(tr("证据来源图件"));
  layout->addWidget(m_evidenceSource);
  connect(m_evidenceSource, &QComboBox::activated, this, [this] { m_sourceFeatureUid->clear(); });
  auto *sourceHint = new QLabel(
      tr("图件来源引用其已保存版本；手工解释可不选来源。"), parent);
  sourceHint->setWordWrap(true);
  layout->addWidget(sourceHint);
  m_evidenceText = new QPlainTextEdit(parent);
  m_evidenceText->setObjectName("faciesEvidenceText");
  m_evidenceText->setAccessibleName(tr("手工解释证据内容"));
  m_evidenceText->setPlaceholderText(
      tr("输入井段、地震反射、单因素特征或专家解释；证据按上方编辑层级记录。"));
  m_evidenceText->setMinimumHeight(paleo::ui_detail::lineRows(m_evidenceText, 3));
  layout->addWidget(m_evidenceText, 1);
  auto *semantics = new QFormLayout;
  m_evidenceRole = new QComboBox(parent);
  m_evidenceRole->setObjectName("faciesEvidenceRole");
  m_evidenceRole->addItem(tr("参考（不推断支持）"), "REFERENCES");
  m_evidenceRole->addItem(tr("支持解释"), "SUPPORTED_BY");
  m_evidenceRole->addItem(tr("反驳解释"), "CONTRADICTED_BY");
  m_evidenceRole->addItem(tr("约束解释"), "CONSTRAINED_BY");
  semantics->addRow(tr("证据作用"), m_evidenceRole);
  layout->addLayout(semantics);
  layout->addWidget(evidenceButton("addEvidence", tr("保存这条证据"), parent));
}

void MappingWorkbenchPage::buildEvidenceSources(QWidget *, QTabWidget *tabs) {
  auto *page = new QWidget(tabs);
  auto *layout = new QVBoxLayout(page);
  tabs->addTab(page, tr("查找资料"));
  m_generationEvidence = new QListWidget(page);
  m_generationEvidence->setObjectName("faciesGenerationEvidence");
  m_generationEvidence->setAccessibleName(tr("生成来源版本"));
  m_generationEvidence->setMinimumHeight(paleo::ui_detail::lineRows(m_generationEvidence, 4));
  layout->addWidget(new QLabel(tr("生成来源与局部空间候选"), page));
  layout->addWidget(m_generationEvidence, 1);
  connect(m_generationEvidence, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    emit evidenceActivated(item->data(Qt::UserRole).toMap());
  });
  // G08/G12 全工程证据反查：按类型/检索词查反向索引（可重建检索结构），
  // 双击结果行复用证据定位链路跳到图斑。
  layout->addWidget(new QLabel(tr("证据反查（全工程）"), page));
  auto *reverseRow = new QHBoxLayout;
  m_reverseKind = new QComboBox(page);
  m_reverseKind->setObjectName("evidenceReverseKind");
  m_reverseKind->setAccessibleName(tr("反查类型"));
  m_reverseKind->addItem(tr("本层位待复核图斑"), QStringLiteral("pending_review"));
  m_reverseKind->addItem(tr("有反驳证据的图斑"), QStringLiteral("contradicted"));
  m_reverseKind->addItem(tr("失效或缺失来源"), QStringLiteral("stale_sources"));
  m_reverseKind->addItem(tr("引用所选来源版本"), QStringLiteral("references_source"));
  m_reverseKind->addItem(tr("全文检索（证据文本/操作人/证据号）"), QStringLiteral("search"));
  reverseRow->addWidget(m_reverseKind, 1);
  m_reverseText = new QLineEdit(page);
  m_reverseText->setObjectName("evidenceReverseText");
  m_reverseText->setAccessibleName(tr("反查检索词"));
  m_reverseText->setPlaceholderText(tr("检索词（版本号类型可留空）"));
  reverseRow->addWidget(m_reverseText, 1);
  reverseRow->addWidget(evidenceButton("reverseQuery", tr("反查"), page));
  layout->addLayout(reverseRow);
  m_reverseResults = new QListWidget(page);
  m_reverseResults->setObjectName("evidenceReverseResults");
  m_reverseResults->setAccessibleName(tr("证据反查结果"));
  m_reverseResults->setMinimumHeight(paleo::ui_detail::lineRows(m_reverseResults, 3));
  layout->addWidget(m_reverseResults, 1);
  connect(m_reverseResults, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    // 反查命中行的主目标是图斑本身（layer/version/feature_uid），不是其
    // 证据来源——走独立信号，由交互层 navigateFeature 定位。
    emit reverseHitActivated(item->data(Qt::UserRole).toMap());
  });
  auto *useSource = new QPushButton(tr("将候选作为关联来源"), page);
  useSource->setObjectName("workbench.useEvidenceSource");
  useSource->setAccessibleName(useSource->text());
  layout->addWidget(useSource);
  useSource->setEnabled(false);
  useSource->setToolTip(tr("请选择已定位的稳定来源图元；支持或反驳的作用需人工明确选择"));
  connect(useSource, &QPushButton::clicked, this, [this] {
    auto *item = m_generationEvidence->currentItem(); if (!item) return;
    const auto row = item->data(Qt::UserRole).toMap();
    const int source = m_evidenceSource->findData(row.value("source_layer"));
    if (source < 0) return;
    m_evidenceSource->setCurrentIndex(source);
    m_sourceFeatureUid->setText(row.value("source_feature_uid").toString());
    m_evidenceText->setPlainText(row.value("name").toString());
    m_evidenceText->setFocus();
  });
}

void MappingWorkbenchPage::buildEvidenceReview(QWidget *, QTabWidget *tabs) {
  auto *page = new QWidget(tabs);
  auto *layout = new QVBoxLayout(page);
  tabs->addTab(page, tr("核验与修订"));
  m_interpretationState = new QLabel(page);
  m_interpretationState->setObjectName("faciesInterpretationState");
  m_interpretationState->setTextFormat(Qt::PlainText); m_interpretationState->setWordWrap(true);
  layout->addWidget(m_interpretationState);
  m_quality = new QLabel(page);
  m_quality->setObjectName("faciesEvidenceQuality");
  m_quality->setTextFormat(Qt::PlainText); m_quality->setWordWrap(true);
  layout->addWidget(m_quality);
  auto *semantics = new QFormLayout;
  m_evidenceType = new QComboBox(page);
  m_evidenceType->setObjectName("faciesEvidenceType");
  for (const auto &entry : QList<QPair<QString, QString>>{{tr("人工观察"), "manual"}, {tr("井资料"), "well"},
      {tr("地震资料"), "seismic"}, {tr("单因素成果"), "single_factor"}, {tr("地质约束"), "constraint"},
      {tr("地质规则"), "rule"}, {tr("参考资料"), "reference"}, {tr("计算成果"), "computation"}})
    m_evidenceType->addItem(entry.first, entry.second);
  semantics->addRow(tr("证据类型"), m_evidenceType);
  m_sourceFeatureUid = new QLineEdit(page);
  m_sourceFeatureUid->setObjectName("faciesEvidenceSourceFeatureUid");
  m_sourceFeatureUid->setPlaceholderText(tr("可选：来源的稳定图元 UID"));
  m_sourceFeatureUid->setToolTip(tr("可从空间候选选择；该字段只指定关联，不修改来源图元"));
  semantics->addRow(tr("来源图元"), m_sourceFeatureUid);
  m_evidenceActor = new QLineEdit(m_workbench->localOperator(), page);
  m_evidenceActor->setObjectName("faciesEvidenceActor");
  m_evidenceActor->setReadOnly(true);
  m_evidenceActor->setToolTip(tr("须为 local:名称；保存时一律写入本机操作人，此字段不代表账户认证"));
  semantics->addRow(tr("本地操作人"), m_evidenceActor);
  m_evidenceReason = new QLineEdit(page);
  m_evidenceReason->setObjectName("faciesEvidenceReason");
  m_evidenceReason->setPlaceholderText(tr("修订或审核时填写；新增证据直接使用说明"));
  semantics->addRow(tr("修改理由"), m_evidenceReason);
  m_reviewState = new QComboBox(page);
  m_reviewState->setObjectName("faciesInterpretationReview");
  for (const auto &entry : QList<QPair<QString, QString>>{{tr("拟议"), "Proposed"}, {tr("待复核"), "NeedsReview"},
      {tr("已确认"), "Confirmed"}, {tr("已驳回"), "Rejected"}, {tr("存在争议"), "Disputed"}, {tr("已替代"), "Superseded"}})
    m_reviewState->addItem(entry.first, entry.second);
  semantics->addRow(tr("解释结论"), m_reviewState);
  m_verification = new QComboBox(page);
  m_verification->setObjectName("faciesEvidenceVerification");
  m_verification->addItem(tr("未核验"), "Unverified"); m_verification->addItem(tr("已核验"), "Verified"); m_verification->addItem(tr("数据争议"), "Disputed");
  semantics->addRow(tr("数据核验"), m_verification);
  layout->addLayout(semantics);
  auto *reviewButtons = new QHBoxLayout;
  reviewButtons->addWidget(evidenceButton("updateEvidence", tr("修订关联"), page));
  reviewButtons->addWidget(evidenceButton("verifyEvidence", tr("记录数据核验"), page));
  layout->addWidget(evidenceButton("reviewInterpretation", tr("保存解释结论"), page));
  layout->addLayout(reviewButtons);
}

void MappingWorkbenchPage::buildEvidenceHistory(QWidget *, QTabWidget *tabs) {
  auto *page = new QWidget(tabs);
  auto *layout = new QVBoxLayout(page);
  tabs->addTab(page, tr("历史"));
  auto *navigation = new QVBoxLayout;
  navigation->addWidget(evidenceButton("evidenceGraph", tr("查看局部证据图谱"), page));
  m_evidenceBack = new QPushButton(tr("返回上一图斑"), page);
  m_evidenceBack->setObjectName("evidenceBack");
  m_evidenceBack->setAccessibleName(m_evidenceBack->text());
  m_evidenceBack->setEnabled(false);
  m_evidenceBack->setToolTip(
      tr("返回反查或图谱导航之前选中的图斑（只回溯研究目标，不回溯来源高亮）"));
  connect(m_evidenceBack, &QPushButton::clicked, this,
          [this] { emit evidenceBackRequested(); });
  navigation->addWidget(m_evidenceBack);
  layout->addLayout(navigation);
  m_evidenceHistory = new QPlainTextEdit(page);
  m_evidenceHistory->setObjectName("faciesEvidenceHistory");
  m_evidenceHistory->setReadOnly(true);
  m_evidenceHistory->setAccessibleName(tr("解释修订历史"));
  m_evidenceHistory->setMinimumHeight(paleo::ui_detail::lineRows(m_evidenceHistory, 4));
  layout->addWidget(m_evidenceHistory, 1);
  auto *historyNavigation = new QHBoxLayout;
  historyNavigation->addWidget(evidenceButton("historyOlder", tr("更早历史"), page));
  historyNavigation->addWidget(evidenceButton("historyNewer", tr("较新历史"), page));
  layout->addLayout(historyNavigation);
  // G03/G04：计算归因 tab——冻结输入重放、上游回执与井空间关联的只读事实。
  auto *attributionPage = new QWidget(tabs);
  auto *attributionLayout = new QVBoxLayout(attributionPage);
  tabs->addTab(attributionPage, tr("计算归因"));
  m_attributionSummary = new QLabel(attributionPage);
  m_attributionSummary->setObjectName("faciesAttributionSummary");
  m_attributionSummary->setTextFormat(Qt::PlainText);
  m_attributionSummary->setWordWrap(true);
  attributionLayout->addWidget(m_attributionSummary);
  m_attributionDetail = new QPlainTextEdit(attributionPage);
  m_attributionDetail->setObjectName("faciesAttributionDetail");
  m_attributionDetail->setReadOnly(true);
  m_attributionDetail->setAccessibleName(tr("计算归因详情"));
  attributionLayout->addWidget(m_attributionDetail);
  // X14：可定位对象——图斑内井与带定位信息的冲突来源，双击复用证据导航。
  attributionLayout->addWidget(new QLabel(tr("可定位对象"), attributionPage));
  m_attributionTargets = new QListWidget(attributionPage);
  m_attributionTargets->setObjectName("attributionTargets");
  m_attributionTargets->setAccessibleName(tr("可定位对象"));
  m_attributionTargets->setMinimumHeight(paleo::ui_detail::lineRows(m_attributionTargets, 3));
  connect(m_attributionTargets, &QListWidget::itemActivated, this,
          [this](QListWidgetItem *item) {
            emit evidenceActivated(item->data(Qt::UserRole).toMap());
          });
  attributionLayout->addWidget(m_attributionTargets, 1);
}

void MappingWorkbenchPage::wireEvidenceControls() {
  connect(m_generationEvidence, &QListWidget::currentRowChanged, this, [this] {
    if (auto *button = commandButton("locateEvidence")) button->setEnabled(m_generationEvidence->currentItem() || m_evidence->currentItem());
    if (auto *button = commandButton("useEvidenceSource")) {
      const auto row = m_generationEvidence->currentItem() ? m_generationEvidence->currentItem()->data(Qt::UserRole).toMap() : QVariantMap();
      button->setEnabled(!row.value("source_feature_uid").toString().isEmpty() && m_evidenceSource->findData(row.value("source_layer")) >= 0);
    }
  });
  connect(m_evidence, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    emit evidenceActivated(item->data(Qt::UserRole + 1).toMap());
  });
  // #440：按键只刷新证据按钮使能，不在界面线程重算图斑证据。
  connect(m_evidenceReason, &QLineEdit::textChanged, this, [this] { refreshEvidenceCommandGates(); });
  connect(m_evidenceActor, &QLineEdit::textChanged, this, [this] { refreshEvidenceCommandGates(); });
  connect(m_evidenceText, &QPlainTextEdit::textChanged, this,
          [this] { refreshEvidenceCommandGates(); });
  connect(m_evidence, &QListWidget::currentRowChanged, this, [this] {
    const auto row = m_evidence->currentItem() ? m_evidence->currentItem()->data(Qt::UserRole + 1).toMap() : QVariantMap{};
    if (m_evidenceRole && !row.isEmpty() && !row.value("quality_error").toBool()) {
      const QSignalBlocker text(m_evidenceText), role(m_evidenceRole), type(m_evidenceType), verify(m_verification);
      m_evidenceText->setPlainText(row.value("text").toString());
      m_sourceFeatureUid->setText(row.value("source_feature_uid").toString());
      m_evidenceRole->setCurrentIndex(qMax(0, m_evidenceRole->findData(row.value("role", "REFERENCES"))));
      m_evidenceType->setCurrentIndex(qMax(0, m_evidenceType->findData(row.value("type", "manual"))));
      m_verification->setCurrentIndex(qMax(0, m_verification->findData(row.value("verification", "Unverified"))));
    }
    auto *b = commandButton("removeEvidence");
    b->setEnabled(nativeSelectionMatchesEvidence() && selectedLayer().startsWith("draft.") &&
                  m_evidence->currentItem() && !row.value("quality_error").toBool());
    b->setToolTip(b->isEnabled() ? tr("移除此条证据，可撤销")
                                 : tr("请先选择编辑副本中的证据"));
    if (auto *locate = commandButton("locateEvidence")) locate->setEnabled(m_evidence->currentItem() || m_generationEvidence->currentItem());
    const auto actorText = m_evidenceActor->text().trimmed();
    const bool localActorOk = actorText.startsWith(QStringLiteral("local:"))
        && !actorText.mid(6).trimmed().isEmpty();
    const bool ready = nativeSelectionMatchesEvidence() && selectedLayer().startsWith("draft.") && !row.isEmpty() && !row.value("quality_error").toBool() &&
        m_workbench->selectedFeatures(selectedLayer()).size() == 1 && !m_evidenceReason->text().trimmed().isEmpty() && localActorOk;
    commandButton("updateEvidence")->setEnabled(ready); commandButton("verifyEvidence")->setEnabled(ready);
  });
}
