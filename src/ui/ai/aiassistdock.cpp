// 层：视图
#include "aiassistdock.h"
#include "aiplanreviewwidget.h"
#include "../paleoemptystate.h"
#include <QTextDocument>
#include "../notifications/paleonotify.h"

#include "aiconversationcard.h"
#include "aicontextbar.h"
#include "aioperationdetailwidget.h"
#include "aitoolresultview.h"

#include "../../ai/chat/markdown.h"
#include "../../workflow/aichatcontroller.h"
#include "../../workflow/aioperationpresentation.h"
#include "../paleotheme.h"

#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

// ui/ai/ — 助手 dock 实现（方向51 骨架，方向62 会话/排版/消息操作，G5
// 会话工作台：上下文归属、历史结果卡恢复、操作详情、草稿/键盘生命周期）。
//
// 渲染纪律：全部文本经 toHtmlEscaped 或 AiMarkdown（内部先整体转义），
// 模型输出按不可信文本处理；色/字体/spacing 一律 PaleoTheme::tokens()，
// 本文件不写任何颜色字面量（check_ui_invariants 门禁）。

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
QString roleLabel(ChatRole role) {
  switch (role) {
  case ChatRole::System:
    return QObject::tr("系统");
  case ChatRole::User:
    return QObject::tr("你");
  case ChatRole::Assistant:
    return QObject::tr("助手");
  case ChatRole::Tool:
    return QObject::tr("工具");
  }
  return QObject::tr("消息");
}
} // namespace

AiAssistDock::AiAssistDock(AiChatController *controller, QWidget *parent)
  : QWidget(parent), m_controller(controller) {
  auto *layout = new QVBoxLayout(this);
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm,
                             tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);

  auto *header = new QHBoxLayout;
  m_status = new QLabel(tr("未配置"), this);
  m_status->setObjectName(QStringLiteral("aiAssistantStatus"));
  m_status->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  // P2(review)：muted 色走活体样式（applyTheme 在 dock 建前执行，palette
  // 事件不可靠；applyThemedStyleSheet 构造即设且随主题自动重算）。
  PaleoTheme::applyThemedStyleSheet(
    m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  header->addWidget(m_status, 1);
  m_new = new QPushButton(tr("新会话"), this);
  m_new->setObjectName(QStringLiteral("aiAssistantNew"));
  m_configure = new QPushButton(tr("配置…"), this);
  m_configure->setObjectName(QStringLiteral("aiAssistantConfigure"));
  header->addWidget(m_new);
  header->addWidget(m_configure);
  layout->addLayout(header);

  // 会话行（方向62）：历史会话入口。下拉选择 = 打开意图（emit 信号，
  // 装配侧落控制器）；重命名/删除同样只发意图，确认对话框归宿主。
  auto *sessionRow = new QHBoxLayout;
  auto *sessionLabel = new QLabel(tr("会话"), this);
  sessionLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_sessions = new QComboBox(this);
  m_sessions->setObjectName(QStringLiteral("aiAssistantSessions"));
  m_rename = new QPushButton(tr("重命名"), this);
  m_rename->setObjectName(QStringLiteral("aiAssistantRename"));
  m_delete = new QPushButton(tr("删除"), this);
  m_delete->setObjectName(QStringLiteral("aiAssistantDelete"));
  sessionRow->addWidget(sessionLabel);
  sessionRow->addWidget(m_sessions, 1);
  sessionRow->addWidget(m_rename);
  sessionRow->addWidget(m_delete);
  layout->addLayout(sessionRow);

  // G5 W1：上下文归属条（当前宿主上下文 vs 回合归属；只读投影）。
  m_contextBar = new AiContextBar(this);
  m_contextBar->setObjectName(QStringLiteral("aiAssistantContextBar"));
  layout->addWidget(m_contextBar);

  m_transcript = new QTextBrowser(this);
  m_transcript->setObjectName(QStringLiteral("aiAssistantTranscript"));
  m_transcript->setReadOnly(true);
  m_transcript->setOpenExternalLinks(false);
  m_transcript->setFont(PaleoTheme::bodyFont());
  layout->addWidget(m_transcript, 1);
  // #870：对话区无内容时居中给下一步指引。
  m_emptyState = new PaleoEmptyStateLabel(QString(), m_transcript->viewport());
  m_emptyState->setObjectName(QStringLiteral("aiAssistantEmptyState"));
  connect(m_transcript->document(), &QTextDocument::contentsChanged, this,
          &AiAssistDock::updateEmptyState);

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
  layout->addWidget(m_cards);

  m_input = new QTextEdit(this);
  m_input->setObjectName(QStringLiteral("aiAssistantInput"));
  m_input->setPlaceholderText(
    tr("向助手提问（Enter 发送，Shift+Enter 换行）"));
  m_input->setFont(PaleoTheme::bodyFont());
  m_input->installEventFilter(this); // W7：键盘生命周期
  // #851/#881：三行输入高度完全按字体推导（去掉硬 64px 地板）。
  m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * 3 +
                          2 * m_input->frameWidth() + 8);
  layout->addWidget(m_input);
  // W7：草稿按会话保存（textChanged 持续记账；接受后才清理）。
  connect(m_input, &QTextEdit::textChanged, this, [this] {
    if (!m_restoringDraft)
      saveCurrentDraft();
  });

  auto *actions = new QHBoxLayout;
  m_copy = new QPushButton(tr("复制"), this);
  m_copy->setObjectName(QStringLiteral("aiAssistantCopy"));
  m_export = new QPushButton(tr("导出…"), this);
  m_export->setObjectName(QStringLiteral("aiAssistantExport"));
  m_resend = new QPushButton(tr("重发"), this);
  m_resend->setObjectName(QStringLiteral("aiAssistantResend"));
  m_send = new QPushButton(tr("发送"), this);
  m_send->setObjectName(QStringLiteral("aiAssistantSend"));
  m_stop = new QPushButton(tr("停止"), this);
  m_stop->setObjectName(QStringLiteral("aiAssistantStop"));
  actions->addWidget(m_copy);
  actions->addWidget(m_export);
  actions->addWidget(m_resend);
  actions->addStretch(1);
  actions->addWidget(m_stop);
  actions->addWidget(m_send);
  layout->addLayout(actions);

  connect(m_send, &QPushButton::clicked, this, &AiAssistDock::sendCurrentText);
  connect(m_new, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->newSession();
  });
  connect(m_stop, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->cancel();
  });
  connect(m_configure, &QPushButton::clicked, this,
          &AiAssistDock::configureRequested);
  // activated 只在用户主动选择时触发（程序化重建不触发），不会反馈环。
  connect(m_sessions, &QComboBox::activated, this, [this](int index) {
    if (!m_controller || index < 0 || index >= m_sessionIds.size())
      return;
    const QString id = m_sessionIds.at(index);
    if (id == m_controller->session().id)
      return; // 选的就是当前会话：无事发生
    emit openSessionRequested(id);
  });
  connect(m_rename, &QPushButton::clicked, this,
          &AiAssistDock::renameComboSession);
  connect(m_delete, &QPushButton::clicked, this, [this] {
    const int index = m_sessions->currentIndex();
    if (index < 0 || index >= m_sessionIds.size())
      return;
    emit deleteSessionRequested(m_sessionIds.at(index));
  });
  connect(m_copy, &QPushButton::clicked, this, &AiAssistDock::copyTranscript);
  connect(m_export, &QPushButton::clicked, this, &AiAssistDock::exportSession);
  connect(m_resend, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->retryLast();
  });
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
  if (m_controller) {
    bindController();
    // 换了编排器：整页按新控制器重画；滚动/卡片/草稿记账一并复位。
    m_currentSessionId = m_controller->session().id;
    m_assistantStart = -1;
    m_assistantDraft.clear();
    m_scrollPositions.clear();
    m_transcript->clear();
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
              finalizeAssistantBlock(); // 流结束：纯文本块 → markdown 定型
          });
  connect(m_controller, &AiChatController::projectBriefChanged, this,
          &AiAssistDock::refreshContextBar);
  connect(m_controller, &AiChatController::sessionChanged, this, [this] {
    // 切换前记住旧会话滚动位置；文档清空后流式块记账一并失效。
    if (!m_currentSessionId.isEmpty())
      m_scrollPositions.insert(m_currentSessionId,
                               m_transcript->verticalScrollBar()->value());
    m_transcript->clear();
    clearToolCards();
    m_assistantStart = -1;
    m_assistantDraft.clear();
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
}

void AiAssistDock::refresh() {
  const bool ready = m_controller && m_controller->enabled();
  const bool streaming = m_controller && m_controller->streaming();
  m_input->setEnabled(ready && !streaming);
  m_send->setEnabled(ready && !streaming);
  m_stop->setEnabled(streaming);
  if (m_controller)
    m_status->setText(m_controller->statusText());
  else
    m_status->setText(tr("未接入助手编排"));
  updateActionStates();
}

void AiAssistDock::updateEmptyState() {
  if (!m_emptyState)
    return;
  const bool empty = m_transcript->document()->isEmpty();
  m_emptyState->setVisible(empty);
  if (!empty)
    return;
  const bool ready = m_controller && m_controller->enabled();
  m_emptyState->setDetailText(
    !m_controller ? tr("助手尚未接入 — 打开工程后再使用 AI 助手")
    : ready       ? tr("还没有对话 — 在下方输入问题并按「发送」，或点「新会话」开始新的话题")
                  : tr("助手尚未配置 — 点上方「配置…」设置大模型服务后即可提问"));
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

void AiAssistDock::refreshSessions() {
  if (!m_controller) {
    m_sessions->clear();
    m_sessionIds.clear();
    return;
  }
  QSignalBlocker block(m_sessions); // 重建不触发 activated/currentIndexChanged
  m_sessions->clear();
  m_sessionIds.clear();
  const ChatSession current = m_controller->session();
  m_sessions->addItem(current.displayTitle().isEmpty()
                        ? tr("（未命名会话）")
                        : current.displayTitle());
  m_sessionIds.append(current.id);
  for (const ChatSession &recent : m_controller->recentSessions(20)) {
    if (recent.id == current.id)
      continue;
    m_sessions->addItem(recent.displayTitle());
    m_sessionIds.append(recent.id);
  }
  m_sessions->setCurrentIndex(0);
  updateActionStates();
}

void AiAssistDock::renameComboSession() {
  const int index = m_sessions->currentIndex();
  if (index < 0 || index >= m_sessionIds.size())
    return;
  bool ok = false;
  const QString title = QInputDialog::getText(
    this, tr("重命名会话"), tr("会话标题："), QLineEdit::Normal,
    m_sessions->itemText(index), &ok);
  if (!ok || title.trimmed().isEmpty())
    return;
  emit renameSessionRequested(m_sessionIds.at(index), title.trimmed());
}

void AiAssistDock::renderSession() {
  if (!m_controller)
    return;
  for (const ChatMessage &message : m_controller->messages())
    appendMessage(message);
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

void AiAssistDock::sendCurrentText() {
  if (!m_controller)
    return;
  const QString text = m_input->toPlainText().trimmed();
  if (text.isEmpty())
    return;
  // W7：未接受的文本不清理。controller 同步拒绝（未配置/上一轮进行中）会发
  // errorOccurred；草稿留在输入框，用户可编辑或停止后重发。
  const qsizetype before = m_controller->messages().size();
  m_controller->sendUserText(text);
  if (!m_controller)
    return; // 发送路径上控制器被拆（防御）
  // 接受判据：历史确实新增了一条内容相同的 user 消息（sendUserText 同步
  // 追加；其后可能已跟上 assistant 流式占位，故比对最后一条 user 消息）。
  const auto &messages = m_controller->messages();
  bool accepted = messages.size() > before;
  for (qsizetype i = messages.size() - 1; i >= 0; --i) {
    if (messages.at(i).role != ChatRole::User)
      continue;
    accepted = accepted && messages.at(i).content == text;
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

void AiAssistDock::changeEvent(QEvent *event) {
  // W8.3：主题切换刷新既有呈现（卡片/状态条各自经活体样式或 changeEvent
  // 重建 token 样式）。
  QWidget::changeEvent(event);
}

void AiAssistDock::writeBlock(const QString &html) {
  QTextCursor cursor(m_transcript->document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertHtml(html);
  m_transcript->ensureCursorVisible();
  updateActionStates();
}

void AiAssistDock::appendMessage(const ChatMessage &message) {
  if (message.role == ChatRole::Assistant && message.isEmpty())
    return; // 流式占位：等增量到了再显示，避免先出现一条空白
  finalizeAssistantBlock(); // 防御：上一轮流式块若未定型，先定型再上新消息
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  if (message.role == ChatRole::Tool) {
    // 工具结果不灌 JSON 进正文（可读性）；完整呈现走工具卡（实时+历史）。
    writeBlock(
      QStringLiteral("<div style=\"color:%1\">%2：%3</div>")
        .arg(tokens.textMuted.name(), roleLabel(message.role).toHtmlEscaped(),
             tr("结果已回传模型（%1 字符）").arg(message.content.size())));
    return;
  }
  const QString color = message.role == ChatRole::User
                          ? tokens.primaryText.name()
                          : tokens.text.name();
  const QString body = message.role == ChatRole::Assistant
                         ? AiMarkdown::toHtml(message.content)
                         : message.content.toHtmlEscaped();
  writeBlock(QStringLiteral("<div><b style=\"color:%1\">%2</b>：%3</div>")
               .arg(color, roleLabel(message.role).toHtmlEscaped(), body));
}

void AiAssistDock::appendDelta(const QString &text) {
  if (m_assistantStart < 0)
    m_assistantStart = m_transcript->document()->characterCount() - 1;
  m_assistantDraft += text;
  QTextCursor cursor(m_transcript->document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text);
  // 用户正在看旧消息时不抢滚动/选择；回底由用户主动操作。
  QScrollBar *scroll = m_transcript->verticalScrollBar();
  if (scroll->value() >= scroll->maximum() - scroll->singleStep())
    m_transcript->ensureCursorVisible();
}

void AiAssistDock::finalizeAssistantBlock() {
  if (m_assistantStart < 0)
    return;
  QTextCursor cursor(m_transcript->document());
  cursor.setPosition(m_assistantStart);
  cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  m_assistantStart = -1;
  const QString draft = m_assistantDraft;
  m_assistantDraft.clear();
  if (draft.trimmed().isEmpty())
    return;
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  writeBlock(QStringLiteral("<div><b style=\"color:%1\">%2</b>：%3</div>")
               .arg(tokens.text.name(),
                    roleLabel(ChatRole::Assistant).toHtmlEscaped(),
                    AiMarkdown::toHtml(draft)));
}

void AiAssistDock::appendError(const QString &message) {
  finalizeAssistantBlock();
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  writeBlock(QStringLiteral("<div style=\"color:%1\">⚠ %2</div>")
               .arg(tokens.error.name(), message.toHtmlEscaped()));
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
  m_historyNote->clear();
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
  QString text;
  if (m_transcript->textCursor().hasSelection()) {
    text = m_transcript->textCursor().selectedText();
    text.replace(QChar(0x2029), QChar::fromLatin1('\n'));
  } else {
    text = m_transcript->toPlainText();
  }
  if (QClipboard *clipboard = QGuiApplication::clipboard()) {
    clipboard->setText(text);
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
