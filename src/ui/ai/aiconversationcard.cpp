// 层：视图
#include "aiconversationcard.h"
#include "../notifications/paleonotify.h"
#include "../paleotheme.h"
#include "aitoolresultview.h"

#include <QClipboard>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

// ui/ai — G5 会话工具卡实现。
//
// 渲染纪律同 dock：色/字体/spacing 一律 PaleoTheme::tokens()（本文件不写
// 色值字面量）；模型可控文本一律 Qt::PlainText；状态词进文案（不只靠色）。

namespace {
QString elided(const QString &text, int cap) {
  return text.size() <= cap ? text : text.left(cap) + QStringLiteral("…");
}
} // namespace

AiConversationCard::AiConversationCard(const QString &toolName,
                                       const QString &toolCallId,
                                       const QString &statusText, QWidget *parent)
  : QWidget(parent), m_toolName(toolName), m_toolCallId(toolCallId) {
  setObjectName(QStringLiteral("aiConversationCard")); // G5 卡级查找口径
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  m_frame = new QFrame(this);
  m_frame->setObjectName(QStringLiteral("aiAssistantToolCard")); // 既有查找口径保留
  outer->addWidget(m_frame);
  auto *layout = new QVBoxLayout(m_frame);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingXs, tokens.spacingXs, tokens.spacingXs,
                             tokens.spacingXs);
  layout->setSpacing(tokens.spacingXs);

  m_headerRow = new QHBoxLayout;
  m_headerRow->setSpacing(tokens.spacingXs);
  m_nameLabel = new QLabel(m_toolName, m_frame);
  m_nameLabel->setObjectName(QStringLiteral("aiCardToolName"));
  m_nameLabel->setTextFormat(Qt::PlainText); // 模型可控文本：不做富文本探测
  m_nameLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_headerRow->addWidget(m_nameLabel, 1);
  m_attributionLabel = new QLabel(m_frame);
  m_attributionLabel->setObjectName(QStringLiteral("aiCardAttribution"));
  m_attributionLabel->setTextFormat(Qt::PlainText);
  m_attributionLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_attributionLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_headerRow->addWidget(m_attributionLabel, 2);
  m_timestampLabel = new QLabel(m_frame);
  m_timestampLabel->setObjectName(QStringLiteral("aiCardTimestamp"));
  m_timestampLabel->setTextFormat(Qt::PlainText);
  m_timestampLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_timestampLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_headerRow->addWidget(m_timestampLabel);
  // 首态 = 分发结论（与既有两态卡同语义）：不冒充「已执行」。
  m_capsule =
    PaleoTheme::capsuleLabel(statusText.isEmpty() ? tr("待执行") : statusText,
                             PaleoTheme::CapsuleKind::Warning, m_frame);
  m_headerRow->addWidget(m_capsule);
  m_expandToggle = new QToolButton(m_frame);
  m_expandToggle->setObjectName(QStringLiteral("aiCardExpandToggle"));
  m_expandToggle->setText(tr("详情"));
  m_expandToggle->setCheckable(true);
  m_expandToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
  m_expandToggle->setArrowType(Qt::RightArrow);
  m_expandToggle->hide(); // 有结果才可展开
  m_headerRow->addWidget(m_expandToggle);
  layout->addLayout(m_headerRow);
  refreshTheme();
  connect(m_expandToggle, &QToolButton::toggled, this, [this](bool on) {
    m_expandToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    if (on)
      buildBody(); // 惰性：首次展开才构建结果视图（W8）
    if (m_body)
      m_body->setVisible(on);
    emit expandedChanged(this);
  });
}

void AiConversationCard::changeEvent(QEvent *event) {
  if (event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::PaletteChange)
    refreshTheme();
  QWidget::changeEvent(event);
}

void AiConversationCard::refreshTheme() {
  if (!m_frame)
    return;
  const auto &tokens = PaleoTheme::tokens();
  PaleoTheme::applyThemedStyleSheet(m_frame, [&tokens] {
    return QStringLiteral("QFrame#aiAssistantToolCard {"
                          " background: %1; border: 1px solid %2;"
                          " border-radius: %3px; padding: %4px; }")
      .arg(tokens.surfaceAlt.name(), tokens.border.name())
      .arg(tokens.radiusMd)
      .arg(tokens.spacingXs);
  });
}

void AiConversationCard::swapCapsule(const QString &text, int kind) {
  auto *fresh = PaleoTheme::capsuleLabel(
    text, static_cast<PaleoTheme::CapsuleKind>(kind), m_frame);
  const int index = m_headerRow->indexOf(m_capsule);
  if (index >= 0)
    m_headerRow->insertWidget(index, fresh);
  else
    m_headerRow->addWidget(fresh);
  m_capsule->deleteLater();
  m_capsule = fresh;
}

void AiConversationCard::markRunning() {
  swapCapsule(tr("执行中"), int(PaleoTheme::CapsuleKind::Neutral));
}

void AiConversationCard::setResultSummary(bool ok, const QString &summary) {
  // 失败原文如实显示（不吞错）；成功态摘要仅兜底使用（结构化视图更全）。
  m_resultSummary = summary;
  if (m_summaryLabel) {
    m_summaryLabel->setText(summary);
    m_summaryLabel->setVisible(!ok); // 成功 + 结构化视图时由 buildBody 决定
  }
}

void AiConversationCard::setArgumentsSummary(const QString &argumentsJson) {
  m_argumentsJson = argumentsJson;
}

void AiConversationCard::setActionNote(const QString &note) {
  if (!m_actionNoteLabel) {
    m_actionNoteLabel = new QLabel(m_frame);
    m_actionNoteLabel->setObjectName(QStringLiteral("aiCardActionNote"));
    m_actionNoteLabel->setTextFormat(Qt::PlainText);
    m_actionNoteLabel->setWordWrap(true);
    m_actionNoteLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    PaleoTheme::applyThemedStyleSheet(
      m_actionNoteLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    if (auto *frameLayout = qobject_cast<QVBoxLayout *>(m_frame->layout()))
      frameLayout->addWidget(m_actionNoteLabel);
    m_actionNoteLabel->hide();
  }
  if (note.isEmpty()) {
    m_actionNoteLabel->hide();
    return;
  }
  m_actionNoteLabel->setText(note);
  m_actionNoteLabel->show();
}

void AiConversationCard::setOperationControlsAllowed(bool allowed) {
  m_operationControlsAllowed = allowed;
  // 已建 body 的卡（如实时卡在解绑 adapter 后）：撤下控制行并给原因，
  // 不留可点而无效的按钮。
  if (!allowed && m_controlRow) {
    m_controlRow->hide();
    setActionNote(tr("操作查询/取消/恢复未接入（无操作服务）"));
  } else if (allowed && m_controlRow) {
    m_controlRow->show();
    setActionNote(QString());
  }
}

QString AiConversationCard::attributionText() const {
  QStringList parts;
  if (!m_attribution.pairedCall)
    parts << tr("历史记录：未找到配对调用");
  else if (m_attribution.ambiguousCallId)
    parts << tr("历史记录：调用标识歧义");
  if (m_attribution.turnKnown && !m_attribution.turnId.isEmpty()) {
    parts << tr("回合 %1").arg(elided(m_attribution.turnId, 10));
    if (m_attribution.bindingKnown)
      parts << tr("宿主绑定已验证");
  } else {
    parts << tr("归属未验证（旧记录未记回合）");
  }
  return parts.join(QStringLiteral(" · "));
}

void AiConversationCard::setAttribution(const Attribution &attribution) {
  m_attribution = attribution;
  m_attributionLabel->setText(attributionText());
  m_attributionLabel->setToolTip(attribution.turnId);
  if (attribution.timestamp.isValid())
    m_timestampLabel->setText(attribution.timestamp.toString(QStringLiteral("HH:mm:ss")));
}

void AiConversationCard::setResult(const paleo::chat::ToolResultFacts &facts,
                                   bool lazyBody) {
  m_facts = facts;
  m_hasResult = true;
  m_operationId = facts.operationId;
  // 终态翻面：完成/失败/未知（状态词进文案，不只靠颜色）。
  using paleo::chat::ToolResultFacts;
  if (facts.outcomeKnown) {
    if (facts.outcome == ToolResultFacts::Outcome::Succeeded)
      swapCapsule(facts.commitState == QLatin1String("committed")
                    ? tr("已完成（已提交）")
                    : tr("已完成"),
                  int(PaleoTheme::CapsuleKind::Success));
    else
      swapCapsule(tr("失败"), int(PaleoTheme::CapsuleKind::Error));
  } else {
    swapCapsule(tr("结果未知"), int(PaleoTheme::CapsuleKind::Warning));
  }
  if (!m_attribution.timestamp.isValid() && facts.tool != m_toolName) {
    // 历史卡：工具名以配对调用为准（分发帧当时可能未知）。
    m_toolName = facts.tool;
    m_nameLabel->setText(m_toolName);
  }
  m_expandToggle->show();
  if (lazyBody) {
    // 历史卡默认收起；既有展开状态保持（重挂刷新场景）。
    if (m_body)
      m_body->setVisible(m_expandToggle->isChecked());
    return;
  }
  // 实时结果：即展开（与既有结果卡体验一致；展开上限由 dock 收敛）。
  setExpanded(true);
}

void AiConversationCard::setExpanded(bool expanded) {
  if (!m_expandToggle || m_expandToggle->isChecked() == expanded)
    return;
  m_expandToggle->setChecked(expanded); // toggled 信号路径负责构建/显隐
}

void AiConversationCard::buildBody() {
  if (m_bodyBuilt || !m_hasResult)
    return;
  m_bodyBuilt = true;
  const auto &tokens = PaleoTheme::tokens();
  m_body = new QWidget(m_frame);
  m_body->setObjectName(QStringLiteral("aiCardBody"));
  m_bodyLayout = new QVBoxLayout(m_body);
  m_bodyLayout->setContentsMargins(0, 0, 0, 0);
  m_bodyLayout->setSpacing(tokens.spacingXs);

  // 历史留存注记（W2.5）：raw 与有界 envelope 的完整度差异如实陈述。
  using paleo::chat::ToolResultFacts;
  if (m_facts.source == ToolResultFacts::Source::Envelope) {
    auto *note = new QLabel(tr("历史留存结果（paleo.tool-result.v1 有界摘要）——"
                               "当时完整出参可能大于此留存"),
                            m_body);
    note->setObjectName(QStringLiteral("aiCardEnvelopeNote"));
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    note->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    PaleoTheme::applyThemedStyleSheet(
      note, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    m_bodyLayout->addWidget(note);
  }
  if (!m_facts.hasData && !m_facts.operationFacts &&
      m_facts.source != ToolResultFacts::Source::Unparseable) {
    auto *missing = new QLabel(tr("结果缺失（无留存内容）"), m_body);
    missing->setTextFormat(Qt::PlainText);
    missing->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    PaleoTheme::applyThemedStyleSheet(
      missing, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    m_bodyLayout->addWidget(missing);
  }

  // 调用参数摘要：既有 argumentsJson 原文（有界、纯文本、tooltip 全文）。
  if (!m_argumentsJson.isEmpty()) {
    auto *arguments = new QLabel(
      tr("参数：%1").arg(elided(m_argumentsJson, 96)), m_body);
    arguments->setObjectName(QStringLiteral("aiCardArguments"));
    arguments->setTextFormat(Qt::PlainText);
    arguments->setToolTip(m_argumentsJson);
    arguments->setWordWrap(true);
    arguments->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    PaleoTheme::applyThemedStyleSheet(
      arguments, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    m_bodyLayout->addWidget(arguments);
  }

  auto *view = new AiToolResultView(m_facts, m_body);
  view->setObjectName(QStringLiteral("aiCardResultView"));
  m_bodyLayout->addWidget(view);
  connect(view, &AiToolResultView::assetNavigateRequested, this,
          &AiConversationCard::assetNavigateRequested);
  connect(view, &AiToolResultView::entityNavigateRequested, this,
          &AiConversationCard::entityNavigateRequested);
  connect(view, &AiToolResultView::lineageNavigateRequested, this,
          &AiConversationCard::lineageNavigateRequested);
  // 兜底摘要行（DESIGN.md 既有口径）：失败态必见原文；成功兜底（tile 等
  // 折叠态）显示摘要、展开折叠块补全文；结构化视图态隐藏。
  if (!m_resultSummary.isEmpty() &&
      view->kind() == AiToolResultViewKind::Fallback) {
    m_summaryLabel = new QLabel(m_resultSummary, m_body);
    m_summaryLabel->setObjectName(QStringLiteral("aiCardSummary"));
    m_summaryLabel->setTextFormat(Qt::PlainText);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
    const bool failed =
      m_facts.outcome == paleo::chat::ToolResultFacts::Outcome::Failed;
    m_summaryLabel->setVisible(failed); // 成功兜底的全文走折叠块
    m_bodyLayout->insertWidget(m_bodyLayout->count() - 1, m_summaryLabel);
  }

  // 操作控制行（仅 operation 事实存在且有 operationId）：意图交 adapter 裁决。
  if (m_facts.operationFacts && !m_operationId.isEmpty()) {
    if (!m_operationControlsAllowed) {
      // 业务不允许时给原因（W8.4）：按钮不出现，说明为什么。
      auto *note = new QLabel(tr("操作查询/取消/恢复未接入（无操作服务）"),
                              m_body);
      note->setObjectName(QStringLiteral("aiCardOperationUnavailable"));
      note->setTextFormat(Qt::PlainText);
      note->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
      PaleoTheme::applyThemedStyleSheet(
        note, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
      m_bodyLayout->addWidget(note);
      m_body->hide();
      if (auto *frameLayout = qobject_cast<QVBoxLayout *>(m_frame->layout()))
        frameLayout->addWidget(m_body);
      return;
    }
    // 控制行包一层 QWidget 记账：allowed=false（adapter 解绑）时整体撤下。
    m_controlRow = new QWidget(m_body);
    auto *actions = new QHBoxLayout(m_controlRow);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(tokens.spacingXs);
    auto *detail = new QPushButton(tr("操作详情"), m_body);
    detail->setObjectName(QStringLiteral("aiCardOperationDetail"));
    actions->addWidget(detail);
    m_refreshButton = new QPushButton(tr("刷新"), m_body);
    m_refreshButton->setObjectName(QStringLiteral("aiCardOperationRefresh"));
    actions->addWidget(m_refreshButton);
    m_cancelButton = new QPushButton(tr("取消"), m_body);
    m_cancelButton->setObjectName(QStringLiteral("aiCardOperationCancel"));
    actions->addWidget(m_cancelButton);
    m_resumeButton = new QPushButton(tr("恢复"), m_body);
    m_resumeButton->setObjectName(QStringLiteral("aiCardOperationResume"));
    actions->addWidget(m_resumeButton);
    actions->addStretch(1);
    m_bodyLayout->addWidget(m_controlRow);
    connect(detail, &QPushButton::clicked, this,
            [this] { emit operationDetailRequested(m_operationId); });
    connect(m_refreshButton, &QPushButton::clicked, this,
            [this] { emit operationRefreshRequested(m_operationId); });
    connect(m_cancelButton, &QPushButton::clicked, this,
            [this] { emit operationCancelRequested(m_operationId); });
    connect(m_resumeButton, &QPushButton::clicked, this,
            [this] { emit operationResumeRequested(m_operationId); });
  }
  m_body->hide(); // 显隐统一由 expand toggle 管理
  if (auto *frameLayout = qobject_cast<QVBoxLayout *>(m_frame->layout()))
    frameLayout->addWidget(m_body);
}

void AiConversationCard::setActionBusy(const QString &action, bool busy) {
  // 精确映射（read/events/results 不牵连恢复按钮）。
  QPushButton *button = nullptr;
  if (action == QLatin1String("refresh"))
    button = m_refreshButton;
  else if (action == QLatin1String("cancel"))
    button = m_cancelButton;
  else if (action == QLatin1String("resume"))
    button = m_resumeButton;
  if (button)
    button->setEnabled(!busy);
}
