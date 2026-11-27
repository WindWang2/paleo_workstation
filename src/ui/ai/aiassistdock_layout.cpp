// 层：视图
#include "aiassistdock.h"
#include "../../workflow/aichatcontroller.h"
#include "../paleoicons.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

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
    "QPushButton#aiAssistantSend:focus { border-color: %3; }"
    "QProgressBar#aiAssistantActivity {background: %2; border: 0;}"
    "QProgressBar#aiAssistantActivity::chunk {background: %7;}"))
    .arg(t.surface.name(), t.border.name(), t.focusRing.name(),
         t.primary.name(), t.onPrimary.name(), t.primaryHover.name(), t.textMuted.name());
}

QString welcomeStyle() {
  const auto &t = PaleoTheme::tokens();
  return PaleoTheme::metricStyleSheet(QStringLiteral(
    "QWidget#aiAssistantWelcomeBody { background: %1; }"
    "QPushButton[starter=true] {text-align: left; background: %1; color: %2;"
    "border: 1px solid %3; border-radius: {rounded.md}px; padding: {spacing.sm}px;}"
    "QPushButton[starter=true]:hover, QPushButton[starter=true]:pressed {background: %4;}"
    "QPushButton[starter=true]:focus {border: 2px solid %5;}"))
    .arg(t.surface.name(), t.text.name(), t.border.name(),
         t.surfaceAltRaised.name(), t.focusRing.name());
}
}

void AiAssistDock::buildConversationArea(QVBoxLayout *layout) {
  const auto &t = PaleoTheme::tokens();
  m_conversationArea = new QStackedWidget(this);
  m_welcome = new QScrollArea;
  m_welcome->setObjectName(QStringLiteral("aiAssistantWelcome"));
  m_welcome->setFrameShape(QFrame::NoFrame);
  m_welcome->setWidgetResizable(true);
  auto *body = new QWidget;
  body->setObjectName(QStringLiteral("aiAssistantWelcomeBody"));
  PaleoTheme::applyThemedStyleSheet(body, welcomeStyle);
  auto *welcome = new QVBoxLayout(body);
  welcome->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
  welcome->setSpacing(t.spacingSm);
  welcome->addStretch();
  m_welcomeIcon = new QLabel(body);
  updateWelcomeIcon();
  welcome->addWidget(m_welcomeIcon, 0, Qt::AlignLeft);
  auto *title = new QLabel(tr("开始新对话"), body);
  auto titleFont = PaleoTheme::bodyFont(t.displayPt);
  titleFont.setWeight(QFont::Medium);
  title->setFont(titleFont);
  welcome->addWidget(title);
  m_emptyState = new QLabel(body);
  m_emptyState->setObjectName(QStringLiteral("aiAssistantEmptyState"));
  m_emptyState->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(m_emptyState, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  welcome->addWidget(m_emptyState);
  welcome->addSpacing(t.spacingXs);
  auto starter = [&](const QString &name, const QString &label, const QString &command) {
    auto *button = new QPushButton(PaleoIcons::commandIcon(command), label, body);
    button->setObjectName(name);
    button->setProperty("starter", true);
    button->setIconSize(PaleoIcons::toolbarSize());
    welcome->addWidget(button);
    return button;
  };
  m_projectPrompt = starter(QStringLiteral("aiAssistantProjectPrompt"), tr("梳理工程资料"),
                            QStringLiteral("treeViewButton"));
  m_toolsPrompt = starter(QStringLiteral("aiAssistantToolsPrompt"), tr("查找可用工具"),
                          QStringLiteral("mamclButton"));
  m_welcomeImport = starter(QStringLiteral("aiAssistantWelcomeImport"), tr("选择工区文件…"),
                            QStringLiteral("importFiles"));
  m_welcomeConfigure = starter(QStringLiteral("aiAssistantWelcomeConfigure"), tr("配置对话模型…"),
                               QStringLiteral("storageGovernanceButton"));
  const auto preparePrompt = [this](QPushButton *button, const QString &prompt) {
    button->setToolTip(prompt);
    connect(button, &QPushButton::clicked, this, [this, prompt] {
      if (m_input->isEnabled() && m_input->toPlainText().isEmpty()) {
        m_input->setPlainText(prompt);
        m_input->moveCursor(QTextCursor::End);
        focusComposer();
      }
    });
  };
  preparePrompt(m_projectPrompt, tr("梳理当前工程的井、地震和层位资料，列出还需要核对的项目。"));
  preparePrompt(m_toolsPrompt, tr("当前有哪些工具可以用于资料检查和成图？请按用途列出。"));
  connect(m_welcomeImport, &QPushButton::clicked, this, &AiAssistDock::chooseFilesForImport);
  connect(m_welcomeConfigure, &QPushButton::clicked, this, &AiAssistDock::configureRequested);
  welcome->addStretch();
  m_welcome->setWidget(body);
  m_conversationArea->addWidget(m_welcome);
  m_transcript = createTranscript();
  m_transcript->setObjectName(QStringLiteral("aiAssistantTranscript"));
  m_transcript->setOpenExternalLinks(false);
  m_transcript->setFrameShape(QFrame::NoFrame);
  m_transcript->setFont(PaleoTheme::bodyFont());
  m_conversationArea->addWidget(m_transcript);
  layout->addWidget(m_conversationArea, 1);
  connect(m_transcript->document(), &QTextDocument::contentsChanged, this,
          &AiAssistDock::updateEmptyState);
}

void AiAssistDock::updateWelcomeIcon() {
  const int size = PaleoTheme::tokens().spacingXl;
  const auto icon = PaleoIcons::commandIcon(QStringLiteral("aiAssistant"));
  m_welcomeIcon->setPixmap(icon.pixmap(QSize(size, size), devicePixelRatioF()));
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
    if (m_controller) {
      m_controller->newSession();
      focusComposer();
    }
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
  m_input->setPlaceholderText(tr("询问 AI Agent…"));
  m_input->setFont(PaleoTheme::bodyFont());
  m_input->setAcceptRichText(false);
  m_input->setTabChangesFocus(true);
  const auto resizeInput = [this](const QSizeF &size) {
    const int line = m_input->fontMetrics().lineSpacing();
    const int padding = 2 * (m_input->frameWidth() + qCeil(m_input->document()->documentMargin()));
    m_input->setFixedHeight(qBound(2 * line + padding, qCeil(size.height()) + 2 * m_input->frameWidth(),
                                  5 * line + padding));
  };
  resizeInput(m_input->document()->size());
  connect(m_input->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
          this, resizeInput);
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
  m_configure = new QPushButton(tr("模型设置…"), composer);
  m_configure->setObjectName(QStringLiteral("aiAssistantConfigure"));
  m_configure->setFlat(true);
  m_send = new QPushButton(PaleoIcons::qgisTheme(QStringLiteral("mActionArrowUp.svg")),
                           tr("发送"), composer);
  m_send->setIconSize(PaleoIcons::toolbarSize());
  m_send->setObjectName(QStringLiteral("aiAssistantSend"));
  m_stop = new QPushButton(tr("停止生成"), composer);
  m_stop->setObjectName(QStringLiteral("aiAssistantStop"));
  actions->addStretch();
  actions->addWidget(m_send);
  actions->addWidget(m_stop);
  content->addLayout(actions);
  m_activity = new QProgressBar(composer);
  m_activity->setObjectName(QStringLiteral("aiAssistantActivity"));
  m_activity->setAccessibleName(tr("Agent 正在处理请求"));
  m_activity->setTextVisible(false);
  m_activity->setRange(0, 0);
  m_activity->setFixedHeight(t.spacingXs);
  content->addWidget(m_activity);
  auto *statusRow = new QHBoxLayout;
  statusRow->addWidget(m_status, 1);
  statusRow->addWidget(m_configure);
  content->addLayout(statusRow);
  layout->addWidget(composer);

  connect(m_send, &QPushButton::clicked, this, &AiAssistDock::sendCurrentText);
  connect(m_stop, &QPushButton::clicked, this, [this] {
    if (m_controller)
      m_controller->cancel();
  });
  connect(m_configure, &QPushButton::clicked, this, &AiAssistDock::configureRequested);
  connect(m_input, &QTextEdit::textChanged, this, [this] {
    updateSendButton();
    updateEmptyState();
    if (!m_restoringDraft)
      saveCurrentDraft();
  });
}

void AiAssistDock::focusComposer() {
  QWidget *target = m_configure;
  if (m_input->isEnabled())
    target = m_input;
  else if (m_stop->isEnabled())
    target = m_stop;
  target->setFocus(Qt::ShortcutFocusReason);
  // An expanded operation can put the composer below the floating dock's viewport.
  for (auto *parent = parentWidget(); parent; parent = parent->parentWidget())
    if (auto *scroll = qobject_cast<QScrollArea *>(parent))
      scroll->ensureWidgetVisible(target);
}
