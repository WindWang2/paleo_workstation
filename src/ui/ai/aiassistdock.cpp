// 层：视图
#include "aiassistdock.h"

#include "../../ai/chat/markdown.h"
#include "../../workflow/aichatcontroller.h"
#include "../paleotheme.h"

#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
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

// ui/ai/ — 助手 dock 实现（方向51 骨架，方向62 补全会话/排版/消息操作）。
//
// 渲染纪律：全部文本经 toHtmlEscaped 或 AiMarkdown（内部先整体转义），
// 模型输出按不可信文本处理；流式期间用 insertText 纯文本追加（不解释
// HTML），流结束一轮把该块替换为 markdown 渲染。色/字体/spacing 一律
// PaleoTheme::tokens()，本文件不写任何颜色字面量（check_ui_invariants 门禁）。

namespace {
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
  QPalette palette = m_status->palette();
  palette.setColor(QPalette::WindowText, tokens.textMuted);
  m_status->setPalette(palette);
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

  m_transcript = new QTextBrowser(this);
  m_transcript->setObjectName(QStringLiteral("aiAssistantTranscript"));
  m_transcript->setReadOnly(true);
  m_transcript->setOpenExternalLinks(false);
  m_transcript->setFont(PaleoTheme::bodyFont());
  layout->addWidget(m_transcript, 1);

  // 工具调用占位卡片区（本方向只展示「模型想调什么 + 分发结论」）。
  m_cards = new QWidget(this);
  m_cards->setObjectName(QStringLiteral("aiAssistantToolCards"));
  m_cardLayout = new QVBoxLayout(m_cards);
  m_cardLayout->setContentsMargins(0, 0, 0, 0);
  m_cardLayout->setSpacing(tokens.spacingXs);
  layout->addWidget(m_cards);

  m_input = new QTextEdit(this);
  m_input->setObjectName(QStringLiteral("aiAssistantInput"));
  m_input->setPlaceholderText(
    tr("向助手提问（Enter 发送，Shift+Enter 换行）"));
  m_input->setFont(PaleoTheme::bodyFont());
  m_input->setFixedHeight(64);
  layout->addWidget(m_input);

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

  if (m_controller) {
    bindController();
    // 初始整页渲染：控制器可能带着历史（如重挂场景）。
    m_currentSessionId = m_controller->session().id;
    renderSession();
    m_transcript->verticalScrollBar()->setValue(
      m_transcript->verticalScrollBar()->maximum());
  }
  refresh();
  refreshSessions(); // 初始即显示当前会话（不等首个信号）
}

void AiAssistDock::attachController(AiChatController *controller) {
  if (m_controller == controller)
    return;
  if (m_controller)
    m_controller->disconnect(this);
  m_controller = controller;
  if (m_controller) {
    bindController();
    // 换了编排器：整页按新控制器重画；滚动记账一并复位（旧控制器的
    // 会话 id 属于另一段历史，UUID 不复用，留着只是死重）。
    m_currentSessionId = m_controller->session().id;
    m_assistantStart = -1;
    m_assistantDraft.clear();
    m_scrollPositions.clear();
    m_transcript->clear();
    renderSession();
  }
  refresh();
  refreshSessions();
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
  connect(m_controller, &AiChatController::statusChanged, this,
          [this](const QString &text) { m_status->setText(text); });
  connect(m_controller, &AiChatController::streamingChanged, this,
          [this](bool streaming) {
            refresh();
            if (!streaming)
              finalizeAssistantBlock(); // 流结束：纯文本块 → markdown 定型
          });
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
    const auto stored = m_scrollPositions.constFind(m_currentSessionId);
    if (stored != m_scrollPositions.constEnd())
      m_transcript->verticalScrollBar()->setValue(stored.value());
    else
      m_transcript->verticalScrollBar()->setValue(
        m_transcript->verticalScrollBar()->maximum());
    refreshSessions();
  });
  connect(m_controller, &AiChatController::sessionListChanged, this,
          &AiAssistDock::refreshSessions);
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

void AiAssistDock::updateActionStates() {
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
  // 当前会话固定首位（哪怕尚未落盘，也保证下拉始终反映现状）。
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
  // 视图本地输入框（取新标题是纯输入，不是业务动作）；落盘走信号给宿主。
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
}

void AiAssistDock::sendCurrentText() {
  if (!m_controller)
    return;
  const QString text = m_input->toPlainText().trimmed();
  if (text.isEmpty())
    return;
  m_input->clear();
  m_controller->sendUserText(text);
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
    // 工具结果不灌 JSON 进正文（可读性）；详情在工具卡摘要里，这里是
    // 弱化一行占位（重载会话时同样呈现，不假装历史上没有这回事）。
    writeBlock(
      QStringLiteral("<div style=\"color:%1\">%2：%3</div>")
        .arg(tokens.textMuted.name(), roleLabel(message.role).toHtmlEscaped(),
             tr("结果已回传模型（%1 字符）").arg(message.content.size())));
    return;
  }
  const QString color = message.role == ChatRole::User
                          ? tokens.primaryText.name()
                          : tokens.text.name();
  // assistant 消息按 markdown 渲染（转换器内部整体转义，无裸 HTML 通路）；
  // 其余角色维持纯文本转义口径。
  const QString body = message.role == ChatRole::Assistant
                         ? AiMarkdown::toHtml(message.content)
                         : message.content.toHtmlEscaped();
  writeBlock(QStringLiteral("<div><b style=\"color:%1\">%2</b>：%3</div>")
               .arg(color, roleLabel(message.role).toHtmlEscaped(), body));
}

void AiAssistDock::appendDelta(const QString &text) {
  // 流式期间纯文本追加（insertText 不解释 HTML——增量即原样文本）。
  if (m_assistantStart < 0)
    m_assistantStart = m_transcript->document()->characterCount() - 1;
  m_assistantDraft += text;
  QTextCursor cursor(m_transcript->document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text);
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
    return; // 没有实际增量：不留空块
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  writeBlock(QStringLiteral("<div><b style=\"color:%1\">%2</b>：%3</div>")
               .arg(tokens.text.name(),
                    roleLabel(ChatRole::Assistant).toHtmlEscaped(),
                    AiMarkdown::toHtml(draft)));
}

void AiAssistDock::appendError(const QString &message) {
  // 防御：若上一轮流式块还没定型（错误先于 streamingChanged 到达的路径），
  // 先定型再上错误行——finalize 的整段替换不会吃掉这行。
  finalizeAssistantBlock();
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  writeBlock(QStringLiteral("<div style=\"color:%1\">⚠ %2</div>")
               .arg(tokens.error.name(), message.toHtmlEscaped()));
}

void AiAssistDock::clearToolCards() {
  m_toolCards.clear();
  while (auto *item = m_cardLayout->takeAt(0)) {
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }
}

void AiAssistDock::copyTranscript() {
  // 有选区复制选区（单条/局部），无选区复制整个会话文本。
  QString text;
  if (m_transcript->textCursor().hasSelection()) {
    text = m_transcript->textCursor().selectedText();
    text.replace(QChar(0x2029), QChar::fromLatin1('\n')); // Qt 选区换行符
  } else {
    text = m_transcript->toPlainText();
  }
  if (QClipboard *clipboard = QGuiApplication::clipboard())
    clipboard->setText(text);
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

void AiAssistDock::addToolCard(const ChatToolCall &call,
                               const QString &statusText) {
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  // 同 id 帧重放（脚本端点/断线重试）不叠卡：旧卡先撤，hash 顶替。
  if (m_toolCards.contains(call.id)) {
    const ToolCard old = m_toolCards.value(call.id);
    if (old.card) {
      m_cardLayout->removeWidget(old.card);
      old.card->deleteLater();
    }
    m_toolCards.remove(call.id);
  }
  ToolCard entry;
  entry.card = new QFrame(m_cards);
  entry.card->setObjectName(QStringLiteral("aiAssistantToolCard"));
  entry.card->setStyleSheet(QStringLiteral(
                              "QFrame#aiAssistantToolCard {"
                              " background: %1; border: 1px solid %2;"
                              " border-radius: %3px; padding: %4px; }")
                              .arg(tokens.surfaceAlt.name(), tokens.border.name())
                              .arg(tokens.radiusMd)
                              .arg(tokens.spacingXs));
  auto *layout = new QVBoxLayout(entry.card);
  layout->setContentsMargins(tokens.spacingXs, tokens.spacingXs, tokens.spacingXs,
                             tokens.spacingXs);
  layout->setSpacing(tokens.spacingXs);
  entry.row = new QHBoxLayout;
  entry.row->setSpacing(tokens.spacingXs);
  auto *name = new QLabel(call.name, entry.card);
  name->setTextFormat(Qt::PlainText); // 模型可控文本：不做富文本探测
  name->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  entry.row->addWidget(name, 1);
  // 首态 = 分发结论（已路由/未登记/本构建不可用都如实写）——不冒充"已执行"。
  entry.capsule =
    PaleoTheme::capsuleLabel(statusText.isEmpty() ? tr("待执行") : statusText,
                             PaleoTheme::CapsuleKind::Warning, entry.card);
  entry.row->addWidget(entry.capsule);
  layout->addLayout(entry.row);
  entry.summary = new QLabel(entry.card);
  entry.summary->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  entry.summary->setWordWrap(true);
  entry.summary->hide(); // 结果到了才显示（两态卡片的第二态）
  layout->addWidget(entry.summary);
  m_cardLayout->addWidget(entry.card);
  m_toolCards.insert(call.id, entry);
}

void AiAssistDock::swapCapsule(ToolCard &card, const QString &text,
                               PaleoTheme::CapsuleKind kind) {
  auto *fresh = PaleoTheme::capsuleLabel(text, kind, card.card);
  const int index = card.row->indexOf(card.capsule);
  if (index >= 0)
    card.row->insertWidget(index, fresh);
  else
    card.row->addWidget(fresh);
  card.capsule->deleteLater();
  card.capsule = fresh;
}

void AiAssistDock::markToolRunning(const ChatToolCall &call) {
  const auto it = m_toolCards.find(call.id);
  if (it == m_toolCards.end())
    return;
  swapCapsule(it.value(), tr("执行中"), PaleoTheme::CapsuleKind::Neutral);
}

void AiAssistDock::markToolResult(const ChatToolCall &call, bool ok,
                                  const QString &summary) {
  const auto it = m_toolCards.find(call.id);
  if (it == m_toolCards.end())
    return;
  ToolCard &card = it.value();
  // 终态翻面：完成/失败 + 摘要行（失败原文如实显示，不吞错）。
  swapCapsule(card, ok ? tr("已完成") : tr("失败"),
              ok ? PaleoTheme::CapsuleKind::Success
                 : PaleoTheme::CapsuleKind::Error);
  card.summary->setText(summary.toHtmlEscaped());
  card.summary->show();
}
