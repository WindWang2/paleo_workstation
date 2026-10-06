// 层：视图
#include "aiassistdock.h"

#include "../../workflow/aichatcontroller.h"
#include "../paleotheme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

// ui/ai/ — 助手 dock 实现（方向51）。
//
// 渲染纪律：全部文本经 toHtmlEscaped（模型输出不可信）；色/字体/spacing 一律
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
  m_send = new QPushButton(tr("发送"), this);
  m_send->setObjectName(QStringLiteral("aiAssistantSend"));
  m_stop = new QPushButton(tr("停止"), this);
  m_stop->setObjectName(QStringLiteral("aiAssistantStop"));
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

  if (m_controller)
    bindController();
  refresh();
}

void AiAssistDock::attachController(AiChatController *controller) {
  if (m_controller == controller)
    return;
  if (m_controller)
    m_controller->disconnect(this);
  m_controller = controller;
  if (m_controller)
    bindController();
  refresh();
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
          &AiAssistDock::refresh);
  connect(m_controller, &AiChatController::sessionChanged, this, [this] {
    m_transcript->clear();
    clearToolCards();
  });
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
}

void AiAssistDock::appendMessage(const ChatMessage &message) {
  if (message.role == ChatRole::Assistant && message.isEmpty())
    return; // 流式占位：等增量到了再显示，避免先出现一条空白
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
  writeBlock(QStringLiteral("<div><b style=\"color:%1\">%2</b>：%3</div>")
               .arg(color, roleLabel(message.role).toHtmlEscaped(),
                    message.content.toHtmlEscaped()));
}

void AiAssistDock::appendDelta(const QString &text) {
  QTextCursor cursor(m_transcript->document());
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text);
  m_transcript->ensureCursorVisible();
}

void AiAssistDock::appendError(const QString &message) {
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
