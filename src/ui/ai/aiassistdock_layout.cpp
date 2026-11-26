// 层：视图
#include "aiassistdock.h"
#include "../../workflow/aichatcontroller.h"

#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString composerStyle() {
  const auto &t = PaleoTheme::tokens();
  return PaleoTheme::metricStyleSheet(QStringLiteral(
    "QFrame#aiAssistantComposer { background: %1; border: 1px solid %2;"
    " border-radius: {rounded.md}px; }"
    "QTextEdit#aiAssistantInput { background: %1; border: 2px solid transparent;"
    " border-radius: {rounded.sm}px; }"
    "QTextEdit#aiAssistantInput:focus { border-color: %3; }"
    "QPushButton#aiAssistantSend:enabled { background: %4; color: %5;"
    " border: 2px solid %4; border-radius: {rounded.sm}px; }"
    "QPushButton#aiAssistantSend:enabled:hover, QPushButton#aiAssistantSend:pressed"
    " { background: %6; border-color: %6; }"
    "QPushButton#aiAssistantSend:focus { border-color: %3; }"))
    .arg(t.surface.name(), t.border.name(), t.focusRing.name(),
         t.primary.name(), t.onPrimary.name(), t.primaryHover.name());
}
}

void AiAssistDock::buildSessionHeader(QVBoxLayout *layout) {
  auto *row = new QHBoxLayout;
  row->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *label = new QLabel(tr("会话"), this);
  label->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_sessions = new QComboBox(this);
  m_sessions->setObjectName(QStringLiteral("aiAssistantSessions"));
  m_sessions->setAccessibleName(tr("当前会话"));
  m_sessions->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_sessions->setMinimumContentsLength(4);
  label->setBuddy(m_sessions);
  auto *newSession = new QPushButton(tr("新会话"), this);
  newSession->setObjectName(QStringLiteral("aiAssistantNew"));
  auto *more = new QToolButton(this);
  more->setObjectName(QStringLiteral("aiAssistantMore"));
  more->setAccessibleName(tr("更多会话操作"));
  more->setText(tr("更多"));
  more->setFocusPolicy(Qt::StrongFocus);
  more->setToolButtonStyle(Qt::ToolButtonTextOnly);
  more->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(more);
  menu->setObjectName(QStringLiteral("aiAssistantMoreMenu"));
  more->setMenu(menu);

  m_resend = menu->addAction(tr("重发上一条消息"));
  m_resend->setObjectName(QStringLiteral("aiAssistantResend"));
  m_copy = menu->addAction(tr("复制对话"));
  m_copy->setObjectName(QStringLiteral("aiAssistantCopy"));
  m_export = menu->addAction(tr("导出会话…"));
  m_export->setObjectName(QStringLiteral("aiAssistantExport"));
  menu->addSeparator();
  m_rename = menu->addAction(tr("重命名会话…"));
  m_rename->setObjectName(QStringLiteral("aiAssistantRename"));
  m_delete = menu->addAction(tr("删除会话…"));
  m_delete->setObjectName(QStringLiteral("aiAssistantDelete"));
  row->addWidget(label);
  row->addWidget(m_sessions, 1);
  row->addWidget(newSession);
  row->addWidget(more);
  layout->addLayout(row);

  connect(newSession, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->newSession();
  });
  connect(m_sessions, &QComboBox::activated, this, [this](int index) {
    const QString id = m_sessionIds.value(index);
    if (m_controller && !id.isEmpty() && id != m_controller->session().id)
      emit openSessionRequested(id);
  });
  connect(m_copy, &QAction::triggered, this, &AiAssistDock::copyTranscript);
  connect(m_export, &QAction::triggered, this, &AiAssistDock::exportSession);
  connect(m_resend, &QAction::triggered, this, [this] {
    if (m_controller)
      m_controller->retryLast();
  });
  connect(m_rename, &QAction::triggered, this, &AiAssistDock::renameComboSession);
  connect(m_delete, &QAction::triggered, this, [this] {
    const QString id = m_sessionIds.value(m_sessions->currentIndex());
    if (!id.isEmpty())
      emit deleteSessionRequested(id);
  });
}

void AiAssistDock::buildComposer(QVBoxLayout *layout) {
  const auto &t = PaleoTheme::tokens();
  auto *composer = new QFrame(this);
  composer->setObjectName(QStringLiteral("aiAssistantComposer"));
  PaleoTheme::applyThemedStyleSheet(composer, composerStyle);
  auto *content = new QVBoxLayout(composer);
  content->setContentsMargins(t.spacingSm, t.spacingXs, t.spacingSm, t.spacingSm);
  content->setSpacing(t.spacingXs);
  m_input = new QTextEdit(composer);
  m_input->setObjectName(QStringLiteral("aiAssistantInput"));
  m_input->setAccessibleName(tr("消息输入框"));
  m_input->setPlaceholderText(tr("输入消息…（Shift+Enter 换行）"));
  m_input->setFont(PaleoTheme::bodyFont());
  m_input->setAcceptRichText(false);
  m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * 2
                          + 2 * m_input->frameWidth() + t.spacingSm);
  m_input->installEventFilter(this);
  content->addWidget(m_input);

  auto *actions = new QHBoxLayout;
  actions->setSpacing(t.spacingXs);
  buildFileImportControls(actions, content);
  m_status = new QLabel(composer);
  m_status->setObjectName(QStringLiteral("aiAssistantStatus"));
  m_status->setTextFormat(Qt::PlainText);
  m_status->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_status->setWordWrap(true);
  m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  PaleoTheme::applyThemedStyleSheet(m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  auto *configure = new QPushButton(tr("模型设置…"), composer);
  configure->setObjectName(QStringLiteral("aiAssistantConfigure"));
  m_send = new QPushButton(tr("发送"), composer);
  m_send->setObjectName(QStringLiteral("aiAssistantSend"));
  m_stop = new QPushButton(tr("停止生成"), composer);
  m_stop->setObjectName(QStringLiteral("aiAssistantStop"));
  actions->addSpacing(t.spacingXs);
  actions->addWidget(m_status, 1);
  actions->addWidget(configure);
  actions->addWidget(m_send);
  actions->addWidget(m_stop);
  content->addLayout(actions);
  layout->addWidget(composer);

  connect(m_send, &QPushButton::clicked, this, &AiAssistDock::sendCurrentText);
  connect(m_stop, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->cancel();
  });
  connect(configure, &QPushButton::clicked, this, &AiAssistDock::configureRequested);
  connect(m_input, &QTextEdit::textChanged, this, [this] {
    updateSendButton();
    if (!m_restoringDraft)
      saveCurrentDraft();
  });
}
