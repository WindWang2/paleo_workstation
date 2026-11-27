// 层：视图
#include "aiassistdock.h"
#include "aiplanreviewwidget.h"
#include <QTextDocument>
#include "../notifications/paleonotify.h"

#include "aiconversationcard.h"
#include "aicontextbar.h"
#include "ai/chat/chatworkspacecontext.h"
#include "aioperationdetailwidget.h"
#include "aitoolresultview.h"

#include "../../workflow/aichatcontroller.h"
#include "../../workflow/aioperationpresentation.h"
#include "../paleotheme.h"

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QVBoxLayout>

// ui/ai/ — 助手 dock 实现（方向51 骨架，方向62 会话/排版/消息操作，G5
// 会话工作台：上下文归属、历史结果卡恢复、操作详情、草稿/键盘生命周期）。
//
// 对话正文的排版与主题切换见 aiassistdock_transcript.cpp。

namespace {
// 控制动作结果的可解释文案（与详情面同口径：服务事实、不夸大不吞错）。
QString actionNoteText(const QString &action, const QJsonObject &response) {
  const QString code = response.value(QStringLiteral("code")).toString();
  if (action == QLatin1String("cancel")) {
    if (code == QLatin1String("cancel_after_commit"))
      return QObject::tr("操作已提交，取消请求不影响已提交成果（cancel_after_commit）");
    if (code == QLatin1String("commit_uncertain"))
      return QObject::tr("提交不确定（commit_uncertain）：提交状态以服务事实为准");
    if (code == QLatin1String("cancel_requested") ||
        response.value(QStringLiteral("ok")).toBool())
      return QObject::tr("已请求取消（等待执行面确认，不提前宣称已停止）");
    return QObject::tr("取消请求失败：%1").arg(code);
  }
  if (action == QLatin1String("resume"))
    return response.value(QStringLiteral("ok")).toBool()
             ? QObject::tr("恢复已受理（原 operationId 继续执行）")
             : QObject::tr("恢复失败：%1").arg(code);
  if (response.value(QStringLiteral("ok")).toBool())
    return QObject::tr("状态已刷新（修订 %1）")
      .arg(response.value(QStringLiteral("revision")).toString());
  return QObject::tr("状态查询失败：%1").arg(code);
}
} // namespace

AiAssistDock::AiAssistDock(AiChatController *controller, QWidget *parent)
  : QWidget(parent), m_controller(controller) {
  auto *layout = new QVBoxLayout(this);
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm,
                             tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);

  buildSessionHeader(layout);

  // G5 W1：上下文归属条（当前宿主上下文 vs 回合归属；只读投影）。
  m_contextBar = new AiContextBar(this);
  m_contextBar->setObjectName(QStringLiteral("aiAssistantContextBar"));
  layout->addWidget(m_contextBar);

  buildConversationArea(layout);

  // G5 W6：操作详情面（从结果卡「操作详情」显式打开；默认隐藏）。
  m_detail = new AiOperationDetailWidget(this);
  layout->addWidget(m_detail);
  // H5：计划审阅面板（从计划提案卡「审阅此计划」显式打开；默认隐藏）。
  m_planReview = new AiPlanReviewWidget(this);
  layout->addWidget(m_planReview);
  connect(m_planReview, &AiPlanReviewWidget::operationDetailRequested, this,
          [this](const QString &id) {
            // 复用既有操作详情面（三轴/事件/端口/取消/恢复），不建第二套。
            if (!m_presenter)
              return;
            m_detail->openOperation(id);
            for (const auto &action : {QStringLiteral("refresh"), QStringLiteral("cancel"),
                                       QStringLiteral("resume")})
              m_detail->onBusyChanged(id, action, m_presenter->busy(id, action));
          });

  // 工具卡区（G5：实时与历史共用 AiConversationCard；历史按窗口有界呈现）。
  m_cards = new QWidget(this);
  m_cards->setObjectName(QStringLiteral("aiAssistantToolCards"));
  m_cardLayout = new QVBoxLayout(m_cards);
  m_cardLayout->setContentsMargins(0, 0, 0, 0);
  m_cardLayout->setSpacing(tokens.spacingXs);
  auto *historyNav = new QHBoxLayout;
  m_historyOlder = new QPushButton(tr("◀ 更早结果"), m_cards);
  m_historyOlder->setObjectName(QStringLiteral("aiAssistantHistoryOlder"));
  m_historyNewer = new QPushButton(tr("更新结果 ▶"), m_cards);
  m_historyNewer->setObjectName(QStringLiteral("aiAssistantHistoryNewer"));
  m_historyNote = new QLabel(m_cards);
  m_historyNote->setObjectName(QStringLiteral("aiAssistantHistoryNote"));
  m_historyNote->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(
    m_historyNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  historyNav->addWidget(m_historyOlder);
  historyNav->addWidget(m_historyNote, 1);
  historyNav->addWidget(m_historyNewer);
  m_cardLayout->addLayout(historyNav);
  m_cardLayout->setAlignment(Qt::AlignTop);
  m_cardScroll = new QScrollArea(this);
  m_cardScroll->setObjectName(QStringLiteral("aiAssistantToolResults"));
  m_cardScroll->setAccessibleName(tr("工具结果"));
  m_cardScroll->setFrameShape(QFrame::NoFrame);
  m_cardScroll->setWidgetResizable(true);
  m_cardScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  m_cardScroll->setMaximumHeight(fontMetrics().lineSpacing() * 10 + tokens.spacingSm);
  m_cardScroll->setWidget(m_cards);
  m_cardScroll->hide();
  layout->addWidget(m_cardScroll);

  buildComposer(layout);
  connect(m_historyOlder, &QPushButton::clicked, this,
          [this] { navigateHistory(-1); });
  connect(m_historyNewer, &QPushButton::clicked, this,
          [this] { navigateHistory(1); });

  if (m_controller) {
    bindController();
    // 初始整页渲染：控制器可能带着历史（如重挂场景）。
    m_currentSessionId = m_controller->session().id;
    renderSession();
    restoreDraft(m_currentSessionId);
    m_transcript->verticalScrollBar()->setValue(
      m_transcript->verticalScrollBar()->maximum());
  }
  refresh();
  refreshSessions(); // 初始即显示当前会话（不等首个信号）
  refreshContextBar();
}

void AiAssistDock::attachController(AiChatController *controller) {
  if (m_controller == controller)
    return;
  if (m_controller) {
    // runner 是 controller 的组合成员（非同一对象）：两处都要解绑，
    // 否则旧 runner 的 toolFinished 会继续打到新面板（结构化视图串台）。
    m_controller->toolRunner()->disconnect(this);
    m_controller->disconnect(this);
  }
  m_controller = controller;
  showFileImportStatus({});
  if (m_controller) {
    bindController();
    // 换了编排器：整页按新控制器重画；滚动/卡片/草稿记账一并复位。
    m_currentSessionId = m_controller->session().id;
    m_scrollPositions.clear();
    clearTranscript();
    clearToolCards();
    renderSession();
    restoreDraft(m_currentSessionId);
  }
  refresh();
  refreshSessions();
  refreshContextBar();
}

void AiAssistDock::bindController() {
  connect(m_controller, &AiChatController::messageAppended, this,
          &AiAssistDock::appendMessage);
  connect(m_controller, &AiChatController::assistantDelta, this,
          &AiAssistDock::appendDelta);
  connect(m_controller, &AiChatController::errorOccurred, this,
          &AiAssistDock::appendError);
  connect(m_controller, &AiChatController::toolCallDispatched, this,
          &AiAssistDock::addToolCard);
  connect(m_controller, &AiChatController::toolExecutionStarted, this,
          &AiAssistDock::markToolRunning);
  connect(m_controller, &AiChatController::toolResultReady, this,
          &AiAssistDock::markToolResult);
  // 方向 93：结构化结果面取全量出参——直连执行器的 toolFinished（既有
  // 信号；toolResultReady 只有 160 字符摘要）。
  connect(m_controller->toolRunner(), &AiChatToolRunner::toolFinished, this,
          &AiAssistDock::showToolResult);
  connect(m_controller, &AiChatController::statusChanged, this,
          [this](const QString &text) {
            m_status->setText(text);
            refresh(); // 配置变化（未配置↔就绪）同步按钮可用性
            refreshContextBar();
          });
  connect(m_controller, &AiChatController::streamingChanged, this,
          [this](bool streaming) {
            refresh();
            refreshContextBar();
            if (!streaming)
              finalizeAssistantBlock(); // 立即显示最后一批文本并收录本条回复。
          });
  connect(m_controller, &AiChatController::projectBriefChanged, this,
          &AiAssistDock::refreshContextBar);
  connect(m_controller, &AiChatController::sessionChanged, this, [this] {
    showFileImportStatus({});
    // 切换前记住旧会话滚动位置；文档清空后流式块记账一并失效。
    if (!m_currentSessionId.isEmpty())
      m_scrollPositions.insert(m_currentSessionId,
                               m_transcript->verticalScrollBar()->value());
    clearTranscript();
    clearToolCards();
    m_currentSessionId =
      m_controller ? m_controller->session().id : QString();
    renderSession();
    restoreDraft(m_currentSessionId);
    const auto stored = m_scrollPositions.constFind(m_currentSessionId);
    if (stored != m_scrollPositions.constEnd())
      m_transcript->verticalScrollBar()->setValue(stored.value());
    else
      m_transcript->verticalScrollBar()->setValue(
        m_transcript->verticalScrollBar()->maximum());
    refreshSessions();
    refreshContextBar();
  });
  connect(m_controller, &AiChatController::sessionListChanged, this,
          &AiAssistDock::refreshSessions);
  // H2：元信息查询结果 → Combo 重建（当前选择/草稿/工具卡不受影响；每次
  // 结果都按当下 controller 状态重建，较旧完成不覆盖新列表）。
  connect(m_controller, &AiChatController::recentSessionSummariesReady, this,
          &AiAssistDock::rebuildSessionCombo);
}

void AiAssistDock::attachOperationPresenter(
    paleo::chat::AiOperationPresentation *presenter) {
  if (m_presenter == presenter)
    return;
  if (m_presenter) {
    m_presenter->disconnect(this);
    m_presenter->disconnect(m_detail);
  }
  // 详情面 → dock 的意图转发一并断开（下次 attach 重建），防换绑后翻倍。
  m_detail->disconnect(this);
  m_detail->hide();
  m_presenter = presenter;
  if (m_presenter)
    handlePresenterSignals();
  for (auto *card : m_toolCards)
    syncCardOperationControls(card);
  for (auto *card : m_historyCards)
    syncCardOperationControls(card);
  refreshContextBar();
}

void AiAssistDock::attachPlanPresenter(paleo::chat::AiPlanPresentation *presenter) {
  if (m_planPresenter == presenter)
    return;
  if (m_planPresenter)
    m_planPresenter->disconnect(this);
  m_planPresenter = presenter;
  // 面板重挂 adapter（幂等；null = 解绑只读）。
  m_planReview->attachPresenter(presenter);
  refreshContextBar();
}

void AiAssistDock::handlePresenterSignals() {
  using paleo::chat::AiOperationPresentation;
  // adapter 事实 → 详情面（详情面自带目标/代次过滤）。
  connect(m_presenter, &AiOperationPresentation::operationStateChanged, m_detail,
          &AiOperationDetailWidget::onOperationState);
  connect(m_presenter, &AiOperationPresentation::operationEventsArrived, m_detail,
          &AiOperationDetailWidget::onOperationEvents);
  connect(m_presenter, &AiOperationPresentation::operationResultsArrived, m_detail,
          &AiOperationDetailWidget::onOperationResults);
  connect(m_presenter, &AiOperationPresentation::operationPortPage, m_detail,
          &AiOperationDetailWidget::onPortPage);
  connect(m_presenter, &AiOperationPresentation::operationActionFinished, m_detail,
          &AiOperationDetailWidget::onActionFinished);
  connect(m_presenter, &AiOperationPresentation::busyChanged, m_detail,
          &AiOperationDetailWidget::onBusyChanged);
  // 忙碌反馈 → 对应卡的控制按钮。
  connect(m_presenter, &AiOperationPresentation::busyChanged, this,
          [this](const QString &operationId, const QString &action, bool busy) {
            const auto cards = operationCards(operationId);
            for (AiConversationCard *card : cards)
              card->setActionBusy(action, busy);
          });
  // 控制动作结果 → 对应卡注记（详情面未开时卡上仍可见服务事实）。
  connect(m_presenter, &AiOperationPresentation::operationActionFinished, this,
          [this](const QString &operationId, const QString &action,
                 const QJsonObject &response) {
            const QString note = actionNoteText(action, response);
            const auto cards = operationCards(operationId);
            for (AiConversationCard *card : cards)
              card->setActionNote(note);
          });
  connect(m_presenter, &AiOperationPresentation::contextChanged, this, [this] {
    m_detail->hide();
    refreshContextBar();
  });
  // 详情面的显式意图 → adapter（由服务裁决；面板不加第二道门）。
  connect(m_detail, &AiOperationDetailWidget::refreshRequested, this,
          [this](const QString &id) {
            QString reason;
            if (m_presenter && !m_presenter->requestRefresh(id, &reason) && !reason.isEmpty())
              m_detail->onOperationState(id, {{QStringLiteral("ok"), false},
                                              {QStringLiteral("code"), reason}});
          });
  connect(m_detail, &AiOperationDetailWidget::eventsRequested, this,
          [this](const QString &id, quint64 after) {
            if (m_presenter)
              m_presenter->requestEvents(id, after);
          });
  connect(m_detail, &AiOperationDetailWidget::resultsRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestResults(id);
          });
  connect(m_detail, &AiOperationDetailWidget::portPageRequested, this,
          [this](const QString &id, const QString &portId, qint64 offset,
                 int maxBytes) {
            if (m_presenter)
              m_presenter->requestPortPage(id, portId, offset, maxBytes);
          });
  connect(m_detail, &AiOperationDetailWidget::cancelRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestCancel(id);
          });
  connect(m_detail, &AiOperationDetailWidget::resumeRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestResume(id);
          });
}

void AiAssistDock::handleCardOperationIntents(AiConversationCard *card) {
  // 导航意图透传到面板级信号（宿主接线消费；面板不自己跳页）。
  connect(card, &AiConversationCard::assetNavigateRequested, this,
          &AiAssistDock::assetNavigateRequested);
  connect(card, &AiConversationCard::entityNavigateRequested, this,
          &AiAssistDock::entityNavigateRequested);
  connect(card, &AiConversationCard::lineageNavigateRequested, this,
          &AiAssistDock::lineageNavigateRequested);
  connect(card, &AiConversationCard::expandedChanged, this,
          &AiAssistDock::enforceExpandedCap);
  connect(card, &AiConversationCard::operationDetailRequested, this,
          [this](const QString &id) {
            if (!m_presenter)
              return;
            m_detail->openOperation(id);
            for (const auto &action : {QStringLiteral("refresh"), QStringLiteral("cancel"),
                                       QStringLiteral("resume")})
              m_detail->onBusyChanged(id, action, m_presenter && m_presenter->busy(id, action));
          });
  // adapter 可能事后解绑（attachOperationPresenter(nullptr)）：空判防悬垂调用。
  connect(card, &AiConversationCard::operationRefreshRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestRefresh(id);
          });
  connect(card, &AiConversationCard::operationCancelRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestCancel(id);
          });
  connect(card, &AiConversationCard::operationResumeRequested, this,
          [this](const QString &id) {
            if (m_presenter)
              m_presenter->requestResume(id);
          });
  handleCardPlanIntents(card);
}

void AiAssistDock::handleCardPlanIntents(AiConversationCard *card) {
  // H5：计划卡的审阅意图 → 审阅面板（adapter 可用时编辑/执行，否则只读
  // 拒绝态；打开面板不自动 prepare/查询/开始任何回合）。
  connect(card, &AiConversationCard::planReviewRequested, this,
          [this](const QString &toolCallId, const QJsonObject &payload) {
            m_planReview->openProposal(payload, toolCallId);
          });
}

void AiAssistDock::refreshContextBar() {
  const QString brief = m_controller ? m_controller->projectBrief() : QString();
  const bool streaming = m_controller && m_controller->streaming();
  const QString turnId = m_controller ? m_controller->hostTurnId() : QString();
  if (!m_presenter) {
    // 未接 adapter（无 host / 测试壳）：工程状态未知（不冒称未打开），操作
    // 服务未接入单独陈述；历史仍可读。
    m_contextBar->presentUnavailable(brief, streaming, turnId,
                                     tr("操作服务未接入（查询/取消/恢复不可用）"));
    updateEmptyState();
    return;
  }
  const auto view = m_presenter->currentContext();
  // 回合冻结身份 = controller 快照（documentId/epoch/generation/taskSession）；
  // host identity 不含层位等内容时不把当前层位冒充历史冻结参数。
  QStringList identity;
  const QJsonObject context =
    m_controller ? m_controller->turnContext() : QJsonObject();
  static const char *keys[] = {"documentId", "epoch", "generation", "taskSession"};
  for (const char *key : keys) {
    const auto value = context.value(QLatin1String(key));
    if (!value.isUndefined())
      identity << QStringLiteral("%1=%2").arg(QString::fromLatin1(key),
                                              value.toString());
  }
  m_contextBar->present(brief, streaming, view.unavailableCode, view.open,
                        view.readOnly, view.horizon, view.unitId, view.documentId,
                        view.epoch, turnId, identity.join(QStringLiteral(", ")));
  updateEmptyState();
}

void AiAssistDock::refresh() {
  const bool ready = m_controller && m_controller->enabled();
  const bool streaming = m_controller && m_controller->streaming();
  m_input->setEnabled(ready && !streaming);
  m_send->setVisible(!streaming);
  m_stop->setEnabled(streaming);
  m_stop->setVisible(streaming);
  m_activity->setVisible(streaming);
  m_status->setText(m_controller ? m_controller->statusText() : tr("助手尚未接入"));
  m_status->setToolTip(m_status->text());
  m_input->setToolTip(ready && !streaming ? tr("Enter 发送，Shift+Enter 换行") : m_status->text());
  updateSendButton();
  updateActionStates();
}

void AiAssistDock::updateSendButton() {
  const bool available = m_input->isEnabled();
  const bool hasDraft = !m_input->toPlainText().trimmed().isEmpty();
  m_send->setEnabled(available && hasDraft);
  m_send->setToolTip(!available ? m_status->text()
                                : hasDraft ? tr("发送消息（Enter）") : tr("输入消息后即可发送"));
}

void AiAssistDock::updateEmptyState() {
  if (!m_emptyState)
    return;
  const bool empty = m_transcript->document()->isEmpty();
  m_conversationArea->setCurrentIndex(empty ? 0 : 1);
  m_emptyState->setVisible(empty);
  if (!empty)
    return;
  const bool ready = m_controller && m_controller->enabled();
  const bool canImport = m_importFiles && m_importFiles->isEnabled();
  const bool canSuggest = ready && !m_controller->streaming() && m_input &&
                          m_input->toPlainText().isEmpty();
  m_projectPrompt->setVisible(canSuggest && (canImport || m_contextBar->hasOpenProject()));
  m_toolsPrompt->setVisible(canSuggest);
  m_welcomeConfigure->setVisible(!ready && m_controller);
  m_welcomeImport->setVisible(!ready && canImport);
  if (ready)
    m_emptyState->setText(canImport ? tr("询问工区资料，或将文件拖到这里导入。")
                                    : tr("询问可用工具，或写下要处理的问题。"));
  else if (canImport)
    m_emptyState->setText(tr("可直接导入工区文件。配置模型后即可对话。"));
  else
    m_emptyState->setText(m_controller ? tr("配置模型后，即可在这里对话和调用工具。")
                                       : tr("助手尚未接入。"));
}

void AiAssistDock::updateActionStates() {
  updateEmptyState();
  const bool hasController = m_controller != nullptr;
  const bool streaming = hasController && m_controller->streaming();
  const bool ready = hasController && m_controller->enabled();
  const bool hasContent = !m_transcript->document()->isEmpty();
  m_rename->setEnabled(hasController && m_sessions->count() > 0);
  m_delete->setEnabled(hasController && m_sessions->count() > 0);
  m_copy->setEnabled(hasContent);
  m_export->setEnabled(hasController && !m_controller->messages().isEmpty());
  m_resend->setEnabled(ready && !streaming && m_controller->canRetry());
}

void AiAssistDock::renderSession() {
  if (!m_controller)
    return;
  const auto messages = m_controller->messages();
  for (qsizetype i = 0; i < messages.size(); ++i) {
    const auto &message = messages.at(i);
    if (i + 1 == messages.size() && m_controller->streaming() &&
        message.role == ChatRole::Assistant)
      appendDelta(message.content);
    else
      appendMessage(message);
  }
  // G5 W2：历史工具卡恢复（与实时卡同一 renderer；重放零模型请求/零执行）。
  m_historyTools = paleo::chat::projectSessionTools(m_controller->session());
  m_historyWindowStart =
    qMax(0, m_historyTools.size() - kCardWidgetCap); // 默认呈现最新一窗
  buildHistoryCards();
}

void AiAssistDock::buildHistoryCards() {
  // 清掉上一窗的历史卡（实时卡另由 m_toolCards 记账）。
  for (AiConversationCard *card : m_historyCards)
    retireCard(card);
  m_historyCards.clear();
  const int count = int(m_historyTools.size());
  const int start = qBound(0, m_historyWindowStart, qMax(0, count - 1));
  const int end = qMin(count, start + kCardWidgetCap);
  for (int index = start; index < end; ++index) {
    const auto &entry = m_historyTools.at(index);
    auto *card = new AiConversationCard(
      entry.toolName.isEmpty() ? tr("（未知工具）") : entry.toolName,
      entry.toolCallId, tr("历史记录"), m_cards);
    AiConversationCard::Attribution attribution;
    attribution.turnId = entry.turnId;
    attribution.turnKnown = !entry.turnId.isEmpty();
    attribution.bindingKnown = entry.bindingKnown;
    attribution.bindingDocumentId = entry.bindingDocumentId;
    attribution.ambiguousCallId = entry.ambiguousCallId;
    attribution.pairedCall = entry.callPaired;
    attribution.hasResult = entry.hasResult;
    if (entry.toolMessageIndex >= 0 && m_controller &&
        entry.toolMessageIndex < m_controller->messages().size())
      attribution.timestamp =
        m_controller->messages().at(entry.toolMessageIndex).timestamp;
    card->setAttribution(attribution);
    card->setArgumentsSummary(entry.argumentsJson);
    if (entry.hasResult)
      card->setResult(entry.facts, /*lazyBody=*/true); // W8：历史卡惰性构建
    syncCardOperationControls(card);
    m_cardLayout->addWidget(card);
    m_historyCards.append(card);
    handleCardOperationIntents(card);
  }
  // 窗口导航（W8.2：较早结果仍可访问；消息与导出内容不受分页影响）。
  const bool pageable = count > kCardWidgetCap;
  m_historyOlder->setVisible(pageable && start > 0);
  m_historyNewer->setVisible(pageable && end < count);
  m_historyNote->setVisible(pageable);
  m_cardScroll->setVisible(count > 0 || !m_toolCards.isEmpty());
  m_historyNote->setText(
    pageable ? tr("历史工具结果 %1–%2 / 共 %3 张（消息与导出不受此窗口影响）")
                 .arg(start + 1)
                 .arg(end)
                 .arg(count)
             : QString());
}

void AiAssistDock::navigateHistory(int delta) {
  const int count = int(m_historyTools.size());
  if (count <= kCardWidgetCap)
    return;
  // delta<0 = 更早（起点前移），delta>0 = 更新（起点后移）。
  const int next = qBound(0, m_historyWindowStart + delta * kCardWidgetCap,
                          count - kCardWidgetCap);
  if (next == m_historyWindowStart)
    return;
  m_historyWindowStart = next;
  buildHistoryCards();
}

void AiAssistDock::enforceExpandedCap(AiConversationCard *card) {
  m_expandedOrder.removeAll(card);
  if (card->expanded())
    m_expandedOrder.append(card);
  while (m_expandedOrder.size() > kExpandedCardCap)
    m_expandedOrder.takeFirst()->setExpanded(false);
}

void AiAssistDock::syncCardOperationControls(AiConversationCard *card) {
  card->setOperationControlsAllowed(m_presenter != nullptr);
  for (const auto &action : {QStringLiteral("refresh"), QStringLiteral("cancel"),
                             QStringLiteral("resume")})
    card->setActionBusy(action, m_presenter && m_presenter->busy(card->operationId(), action));
}

void AiAssistDock::retireCard(AiConversationCard *card) {
  m_expandedOrder.removeAll(card);
  m_cardLayout->removeWidget(card);
  card->hide();
  card->disconnect(this);
  card->deleteLater();
}

QList<AiConversationCard *> AiAssistDock::operationCards(const QString &operationId) const {
  QList<AiConversationCard *> cards;
  for (auto *card : m_historyCards)
    if (card->operationId() == operationId)
      cards.append(card);
  for (auto *card : m_toolCards)
    if (card->operationId() == operationId)
      cards.append(card);
  return cards;
}

QString AiAssistDock::messageWithWorkspace(const QString &text) const {
  if (!m_presenter) return text;
  const auto host = m_presenter->currentContext();
  QStringList summary;
  if (host.open) {
    if (m_workspaceLabels) summary = m_workspaceLabels();
    summary << tr("工程标识 %1").arg(host.documentId)
            << tr("层位 %1").arg(host.horizon.isEmpty() ? tr("未选择") : host.horizon);
    if (!host.unitId.isEmpty()) summary << tr("层段 %1").arg(host.unitId);
    summary << (host.readOnly ? tr("工程只读") : tr("工程可写"));
  } else if (host.unavailableCode == QLatin1String("document_unavailable")) {
    summary << tr("工程未打开");
  }
  return summary.isEmpty() ? text : text + paleo::chat::workspaceContextSeparator() + summary.join(QStringLiteral(" · "));
}

void AiAssistDock::sendCurrentText() {
  if (!m_controller)
    return;
  const QString text = m_input->toPlainText().trimmed();
  if (text.isEmpty())
    return;
  // W7：未接受的文本不清理。controller 同步拒绝（未配置/上一轮进行中）会发
  // errorOccurred；草稿留在输入框，用户可编辑或停止后重发。
  const qsizetype before = m_controller->messages().size();
  const QString message = messageWithWorkspace(text);
  m_controller->sendUserText(message);
  if (!m_controller)
    return; // 发送路径上控制器被拆（防御）
  // 接受判据：历史确实新增了一条内容相同的 user 消息（sendUserText 同步
  // 追加；其后可能已跟上 assistant 流式占位，故比对最后一条 user 消息）。
  const auto &messages = m_controller->messages();
  bool accepted = messages.size() > before;
  for (qsizetype i = messages.size() - 1; i >= 0; --i) {
    if (messages.at(i).role != ChatRole::User)
      continue;
    accepted = accepted && messages.at(i).content == message;
    break;
  }
  if (accepted) {
    m_input->clear();
    m_drafts.remove(m_currentSessionId);
  } else {
    saveCurrentDraft();
  }
}

void AiAssistDock::saveCurrentDraft() {
  if (m_currentSessionId.isEmpty())
    return;
  const QString text = m_input->toPlainText();
  if (text.isEmpty())
    m_drafts.remove(m_currentSessionId);
  else
    m_drafts.insert(m_currentSessionId, text);
}

void AiAssistDock::restoreDraft(const QString &sessionId) {
  m_restoringDraft = true;
  m_input->setPlainText(m_drafts.value(sessionId));
  m_restoringDraft = false;
}

bool AiAssistDock::eventFilter(QObject *watched, QEvent *event) {
  if (filterFileDrop(watched, event))
    return true;
  // W7.1：Enter 发送 / Shift+Enter 换行；IME 组字中的 Enter 属输入法确认
  // 不发送；按住不放的重复 Enter 不重复发送。
  if (watched == m_input && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    if (enter && !key->isAutoRepeat()) {
      const bool shift = key->modifiers().testFlag(Qt::ShiftModifier);
      const bool composing = QGuiApplication::inputMethod() &&
                             QGuiApplication::inputMethod()->isVisible();
      if (!shift && !composing) {
        sendCurrentText();
        return true;
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void AiAssistDock::clearToolCards() {
  m_detail->hide();
  m_planReview->hide(); // 关闭视图不取消已受理的计算（W8.1）
  for (auto *card : m_toolCards)
    retireCard(card);
  for (auto *card : m_historyCards)
    retireCard(card);
  m_toolCards.clear();
  m_historyCards.clear();
  while (auto *item = m_cardLayout->takeAt(0)) {
    if (item->widget() && item->widget() != m_historyNote &&
        item->widget() != m_historyOlder && item->widget() != m_historyNewer)
      item->widget()->deleteLater();
    delete item;
  }
  // 历史导航行常驻（buildHistoryCards 重建时再按窗口显隐）。
  auto *historyNav = new QHBoxLayout;
  historyNav->addWidget(m_historyOlder);
  historyNav->addWidget(m_historyNote, 1);
  historyNav->addWidget(m_historyNewer);
  m_cardLayout->addLayout(historyNav);
  m_historyOlder->hide();
  m_historyNewer->hide();
  m_historyNote->hide();
  m_historyNote->clear();
  m_cardScroll->hide();
}

void AiAssistDock::addToolCard(const ChatToolCall &call,
                               const QString &statusText) {
  // 同 id 帧重放（脚本端点/断线重试）不叠卡：旧卡先撤，hash 顶替。
  if (m_toolCards.contains(call.id)) {
    retireCard(m_toolCards.take(call.id));
  }
  // W8：实时卡同样受活跃卡上限约束（超出按 QHash 任意序淘汰一张——有界性
  // 是目标而非次序；被撤卡的迟到 toolFinished 因 hash 已移除而安全丢弃，
  // 消息与历史回放不受影响）。
  while (m_toolCards.size() >= kCardWidgetCap) {
    retireCard(m_toolCards.take(m_toolCards.constBegin().key()));
  }
  auto *card = new AiConversationCard(call.name, call.id, statusText, m_cards);
  // 实时卡归属：当前回合（hostTurnId 是宿主持久归属快照）。
  AiConversationCard::Attribution attribution;
  attribution.timestamp = QDateTime::currentDateTime();
  if (m_controller) {
    attribution.turnId = m_controller->hostTurnId();
    attribution.turnKnown = !attribution.turnId.isEmpty();
  }
  card->setAttribution(attribution);
  card->setArgumentsSummary(call.argumentsJson);
  card->setOperationControlsAllowed(m_presenter != nullptr);
  m_cardLayout->addWidget(card);
  m_toolCards.insert(call.id, card);
  m_cardScroll->show();
  handleCardOperationIntents(card);
}

void AiAssistDock::markToolRunning(const ChatToolCall &call) {
  const auto it = m_toolCards.find(call.id);
  if (it != m_toolCards.end())
    it.value()->markRunning();
}

void AiAssistDock::markToolResult(const ChatToolCall &call, bool ok,
                                  const QString &summary) {
  const auto it = m_toolCards.find(call.id);
  if (it == m_toolCards.end())
    return;
  // toolResultReady（160 字符摘要）先于 toolFinished 全量帧到达：摘要交给
  // 卡片（兜底态可见），完整翻面由 showToolResult 完成。
  it.value()->setResultSummary(ok, summary);
}

void AiAssistDock::showToolResult(const ChatToolCall &call, bool ok,
                                  const QString &resultJson) {
  const auto it = m_toolCards.find(call.id);
  if (it == m_toolCards.end())
    return; // 卡片已随会话切换清掉（迟到帧）：不复活卡
  AiConversationCard *card = it.value();
  const auto facts = paleo::chat::projectToolResult(call.name, resultJson, true, ok);
  card->setResult(facts, /*lazyBody=*/false); // 实时结果即展开
  syncCardOperationControls(card);
}

void AiAssistDock::copyTranscript() {
  auto cursor = m_transcript->textCursor();
  if (!cursor.hasSelection())
    cursor.select(QTextCursor::Document);
  if (QClipboard *clipboard = QGuiApplication::clipboard()) {
    const QTextDocumentFragment fragment = cursor.selection();
    auto *mime = new QMimeData;
    mime->setText(fragment.toPlainText());
    mime->setHtml(fragment.toHtml());
    clipboard->setMimeData(mime);
    PaleoNotify::copied(this, tr("对话内容"));
  }
}

void AiAssistDock::exportSession() {
  if (!m_controller)
    return;
  const ChatSession session = m_controller->session();
  const QString suggested =
    (session.displayTitle().isEmpty() ? tr("会话") : session.displayTitle())
      .remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))) +
    QStringLiteral(".md");
  const QString path = QFileDialog::getSaveFileName(
    this, tr("导出会话"), suggested, tr("Markdown 文件 (*.md)"));
  if (path.isEmpty())
    return; // 用户取消
  QString error;
  if (!m_controller->exportCurrentSession(path, &error))
    appendError(error.isEmpty() ? tr("导出失败") : error);
}
