// 层：视图
#include "pythonreplpanel.h"

#include "../../workflow/pythonconsolecontroller.h"
#include "../paleotheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

PythonReplPanel::PythonReplPanel(PythonConsoleController *controller,
                                 QWidget *parent)
    : QWidget(parent), m_controller(controller)
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm,
                             tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);

  auto *header = new QHBoxLayout;
  header->setSpacing(tokens.spacingXs);
  // 实验性标注钉死在头部（诚实面）：会话桥只转发 stdin/stdout，
  // 无高亮/补全/内省。
  QLabel *badge = PaleoTheme::capsuleLabel(tr("实验性"),
                                           PaleoTheme::CapsuleKind::Warning, this);
  badge->setObjectName(QStringLiteral("pythonReplExperimentalBadge"));
  header->addWidget(badge);
  m_status = new QLabel(tr("会话未启动"), this);
  m_status->setObjectName(QStringLiteral("pythonReplStatus"));
  m_status->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_status->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  header->addWidget(m_status, 1);
  m_startButton = new QPushButton(tr("启动会话"), this);
  m_startButton->setObjectName(QStringLiteral("pythonReplStart"));
  header->addWidget(m_startButton);
  m_stopButton = new QPushButton(tr("停止"), this);
  m_stopButton->setObjectName(QStringLiteral("pythonReplStop"));
  header->addWidget(m_stopButton);
  layout->addLayout(header);

  // 安全口径与脚本控制台同墙上：REPL 同样以当前用户权限执行任意代码。
  auto *security = new QLabel(
      tr("会话以当前用户权限执行输入的代码、无沙箱——只输入可信语句"), this);
  security->setObjectName(QStringLiteral("pythonReplSecurityHint"));
  security->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  security->setWordWrap(true);
  layout->addWidget(security);

  m_output = new QPlainTextEdit(this);
  m_output->setObjectName(QStringLiteral("pythonReplOutput"));
  m_output->setReadOnly(true);
  m_output->setFont(PaleoTheme::monoFont());
  m_output->setPlaceholderText(
      tr("REPL 输出（含 >>> 提示符）将显示在这里；解释器的提示符与报错"
         "走 stderr，以警告色呈现属正常口径"));
  layout->addWidget(m_output, 1);

  m_input = new QLineEdit(this);
  m_input->setObjectName(QStringLiteral("pythonReplInput"));
  m_input->setFont(PaleoTheme::monoFont());
  m_input->setPlaceholderText(tr("输入 Python 语句，回车执行"));
  layout->addWidget(m_input);

  connect(m_startButton, &QPushButton::clicked, m_controller,
          &PythonConsoleController::startRepl);
  connect(m_stopButton, &QPushButton::clicked, m_controller,
          &PythonConsoleController::stopRepl);
  connect(m_input, &QLineEdit::returnPressed, this, &PythonReplPanel::onSend);
  connect(m_controller, &PythonConsoleController::replStarted, this,
          [this](const QString &interpreter) {
            m_status->setText(tr("会话运行中：%1").arg(interpreter));
            refresh();
          });
  connect(m_controller, &PythonConsoleController::replOutput, this,
          &PythonReplPanel::appendText);
  connect(m_controller, &PythonConsoleController::replFinished, this,
          [this](int exitCode, bool crashed) {
            m_status->setText(tr("会话未启动"));
            appendSystem(tr("会话已结束（退出码 %1%2）")
                             .arg(exitCode)
                             .arg(crashed ? tr("，异常") : QString()));
            refresh();
          });
  connect(m_controller, &PythonConsoleController::replFailed, this,
          [this](const QString &reason) {
            appendSystem(reason);
            refresh();
          });

  refresh();
}

void PythonReplPanel::refresh()
{
  const bool running = m_controller->replRunning();
  m_input->setEnabled(running);
  m_stopButton->setEnabled(running);
  // 解释器缺失时启动禁用（服务会诚实报 startFailed，这里先挡一步）。
  m_startButton->setEnabled(!running && !m_controller->interpreter().isEmpty());
}

void PythonReplPanel::appendText(const QString &text, bool stderrChannel)
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  QTextCharFormat format;
  if (stderrChannel)
    format.setForeground(tokens.warning);
  QTextCursor cursor = m_output->textCursor();
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text, format);
  m_output->setTextCursor(cursor);
  m_output->ensureCursorVisible();
}

void PythonReplPanel::appendSystem(const QString &text)
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  QTextCharFormat format;
  format.setForeground(tokens.textMuted);
  QTextCursor cursor = m_output->textCursor();
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(text + QLatin1Char('\n'), format);
  m_output->setTextCursor(cursor);
  m_output->ensureCursorVisible();
}

void PythonReplPanel::onSend()
{
  const QString line = m_input->text();
  if (line.isEmpty() || !m_controller->replRunning())
    return;
  // 管道 stdin 无终端回显——输入行本地回显，保持会话可读。
  appendSystem(QStringLiteral(">>> ") + line);
  m_controller->sendReplLine(line);
  m_input->clear();
}
