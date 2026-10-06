// 层：视图
#include "pythonconsolepanel.h"

#include "../../domain/projectclassifier.h"
#include "../paleotheme.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

PythonConsolePanel::PythonConsolePanel(PythonConsoleController *controller,
                                       QWidget *parent)
    : QWidget(parent), m_controller(controller)
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm,
                             tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);

  // 安全口径如实上墙（无沙箱；契约文档位置一并给出）。
  auto *security = new QLabel(
      tr("脚本以当前用户权限运行、无沙箱——只运行可信脚本；输出 JSON 行协议"
         "（可选）见 tools/reference/scripts/README.md"),
      this);
  security->setObjectName(QStringLiteral("pythonSecurityHint"));
  security->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  security->setWordWrap(true);
  layout->addWidget(security);

  auto *interpreterRow = new QHBoxLayout;
  interpreterRow->setSpacing(tokens.spacingXs);
  m_interpreterLabel = new QLabel(this);
  m_interpreterLabel->setObjectName(QStringLiteral("pythonInterpreterLabel"));
  m_interpreterLabel->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  interpreterRow->addWidget(m_interpreterLabel, 1);
  m_recheckButton = new QPushButton(tr("重新检测"), this);
  m_recheckButton->setObjectName(QStringLiteral("pythonRecheck"));
  m_recheckButton->setToolTip(tr("重新按 PALEO_PYTHON → PATH 的顺序探测解释器"));
  interpreterRow->addWidget(m_recheckButton);
  layout->addLayout(interpreterRow);

  auto *scriptRow = new QHBoxLayout;
  scriptRow->setSpacing(tokens.spacingXs);
  m_scriptEdit = new QLineEdit(this);
  m_scriptEdit->setObjectName(QStringLiteral("pythonScriptPath"));
  m_scriptEdit->setPlaceholderText(tr("选择或输入 .py 脚本路径"));
  scriptRow->addWidget(m_scriptEdit, 1);
  auto *browseScript = new QPushButton(tr("浏览…"), this);
  browseScript->setObjectName(QStringLiteral("pythonBrowseScript"));
  scriptRow->addWidget(browseScript);
  layout->addLayout(scriptRow);

  auto *argsRow = new QHBoxLayout;
  argsRow->setSpacing(tokens.spacingXs);
  m_argsEdit = new QLineEdit(this);
  m_argsEdit->setObjectName(QStringLiteral("pythonArgs"));
  m_argsEdit->setPlaceholderText(tr("参数（按空格拆分透传 argv，不支持引号）"));
  argsRow->addWidget(m_argsEdit, 1);
  m_workdirEdit = new QLineEdit(this);
  m_workdirEdit->setObjectName(QStringLiteral("pythonWorkdir"));
  m_workdirEdit->setPlaceholderText(tr("工作目录（空 = 脚本所在目录）"));
  argsRow->addWidget(m_workdirEdit, 1);
  auto *browseWorkdir = new QPushButton(tr("选择目录…"), this);
  browseWorkdir->setObjectName(QStringLiteral("pythonBrowseWorkdir"));
  argsRow->addWidget(browseWorkdir);
  layout->addLayout(argsRow);

  auto *runRow = new QHBoxLayout;
  runRow->setSpacing(tokens.spacingXs);
  m_runButton = new QPushButton(tr("运行"), this);
  m_runButton->setObjectName(QStringLiteral("pythonRun"));
  runRow->addWidget(m_runButton);
  m_stopButton = new QPushButton(tr("停止"), this);
  m_stopButton->setObjectName(QStringLiteral("pythonStop"));
  runRow->addWidget(m_stopButton);
  m_progressBar = new QProgressBar(this);
  m_progressBar->setObjectName(QStringLiteral("pythonProgress"));
  m_progressBar->setRange(0, 100);
  m_progressBar->setValue(0);
  m_progressBar->setVisible(false);
  runRow->addWidget(m_progressBar, 1);
  layout->addLayout(runRow);

  m_output = new QTextBrowser(this);
  m_output->setObjectName(QStringLiteral("pythonOutput"));
  m_output->setFont(PaleoTheme::monoFont());
  m_output->setPlaceholderText(tr("脚本输出将显示在这里（stdout 正文 / stderr "
                                  "警告色 / 系统消息暗色 / 协议错误红色）"));
  layout->addWidget(m_output, 1);

  auto *bottomSplit = new QSplitter(this);
  bottomSplit->setObjectName(QStringLiteral("pythonBottomSplit"));
  auto *resultsBox = new QWidget(bottomSplit);
  auto *resultsLayout = new QVBoxLayout(resultsBox);
  resultsLayout->setContentsMargins(0, 0, 0, 0);
  resultsLayout->setSpacing(tokens.spacingXs);
  auto *resultsTitle = new QLabel(tr("脚本产出"), resultsBox);
  resultsTitle->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  resultsLayout->addWidget(resultsTitle);
  m_resultsList = new QListWidget(resultsBox);
  m_resultsList->setObjectName(QStringLiteral("pythonResultsList"));
  resultsLayout->addWidget(m_resultsList, 1);
  m_importButton = new QPushButton(tr("导入所选结果"), resultsBox);
  m_importButton->setObjectName(QStringLiteral("pythonImportResult"));
  m_importButton->setEnabled(false);
  resultsLayout->addWidget(m_importButton);
  bottomSplit->addWidget(resultsBox);

  auto *historyBox = new QWidget(bottomSplit);
  auto *historyLayout = new QVBoxLayout(historyBox);
  historyLayout->setContentsMargins(0, 0, 0, 0);
  historyLayout->setSpacing(tokens.spacingXs);
  auto *historyTitle = new QLabel(tr("运行历史（双击回填脚本与参数）"), historyBox);
  historyTitle->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
  historyLayout->addWidget(historyTitle);
  m_historyList = new QListWidget(historyBox);
  m_historyList->setObjectName(QStringLiteral("pythonHistoryList"));
  historyLayout->addWidget(m_historyList, 1);
  bottomSplit->addWidget(historyBox);
  bottomSplit->setStretchFactor(0, 1);
  bottomSplit->setStretchFactor(1, 1);
  layout->addWidget(bottomSplit, 1);

  connect(browseScript, &QPushButton::clicked, this,
          &PythonConsolePanel::onBrowseScript);
  connect(browseWorkdir, &QPushButton::clicked, this,
          &PythonConsolePanel::onBrowseWorkdir);
  connect(m_recheckButton, &QPushButton::clicked, m_controller,
          &PythonConsoleController::refreshInterpreter);
  connect(m_runButton, &QPushButton::clicked, this,
          &PythonConsolePanel::onRunClicked);
  connect(m_stopButton, &QPushButton::clicked, this,
          &PythonConsolePanel::onStopClicked);
  connect(m_importButton, &QPushButton::clicked, this,
          &PythonConsolePanel::onImportClicked);
  connect(m_resultsList, &QListWidget::itemSelectionChanged, this,
          &PythonConsolePanel::onResultSelectionChanged);
  connect(m_historyList, &QListWidget::itemActivated, this,
          &PythonConsolePanel::onHistoryActivated);

  connect(m_controller, &PythonConsoleController::interpreterChanged, this,
          [this](const QString &) {
            updateInterpreterRow();
            refresh();
          });
  connect(m_controller, &PythonConsoleController::runQueued, this,
          [this](qint64, const QString &scriptPath) {
            appendSystemLine(tr("排队等待运行：%1").arg(scriptPath));
          });
  connect(m_controller, &PythonConsoleController::runStarted, this,
          [this](qint64, const QString &scriptPath) {
            appendSystemLine(tr("开始运行：%1").arg(scriptPath));
            m_progressBar->setValue(0);
            m_progressBar->setVisible(true);
          });
  connect(m_controller, &PythonConsoleController::consoleLine, this,
          &PythonConsolePanel::appendOutput);
  connect(m_controller, &PythonConsoleController::progressChanged, this,
          [this](qint64, int percent, const QString &) {
            m_progressBar->setValue(percent);
          });
  connect(m_controller, &PythonConsoleController::resultProduced, this,
          &PythonConsolePanel::addResultItem);
  connect(m_controller, &PythonConsoleController::runFinished, this,
          &PythonConsolePanel::onRunFinished);
  connect(m_controller, &PythonConsoleController::busyChanged, this,
          [this](bool) { refresh(); });

  updateInterpreterRow();
  refresh();
}

void PythonConsolePanel::refresh()
{
  const bool available = !m_controller->interpreter().isEmpty();
  // 解释器缺失时运行禁用（诚实面）；解释器在时允许连点——闸内超出会排队，
  // 队列态由系统消息如实呈现。
  m_runButton->setEnabled(available);
  m_stopButton->setEnabled(m_controller->busy());
  if (!m_controller->busy())
    m_progressBar->setVisible(false);
}

void PythonConsolePanel::updateInterpreterRow()
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  QPalette palette = m_interpreterLabel->palette();
  const QString interpreter = m_controller->interpreter();
  if (interpreter.isEmpty())
  {
    m_interpreterLabel->setText(
        tr("未找到 Python 解释器——请安装 Python 3，或设置 PALEO_PYTHON "
           "环境变量后点「重新检测」"));
    palette.setColor(QPalette::WindowText, tokens.warning);
  }
  else
  {
    m_interpreterLabel->setText(tr("解释器：%1").arg(interpreter));
    palette.setColor(QPalette::WindowText, tokens.textMuted);
  }
  m_interpreterLabel->setPalette(palette);
}

void PythonConsolePanel::appendOutput(const QString &text, int channel)
{
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  QColor color = tokens.text;
  if (channel == PythonConsoleController::Stderr)
    color = tokens.warning;
  else if (channel == PythonConsoleController::Error)
    color = tokens.error;
  else if (channel == PythonConsoleController::System)
    color = tokens.textMuted;
  m_output->append(QStringLiteral("<div style=\"color:%1\">%2</div>")
                       .arg(color.name(), text.toHtmlEscaped()));
}

void PythonConsolePanel::appendSystemLine(const QString &message)
{
  appendOutput(message, PythonConsoleController::System);
}

void PythonConsolePanel::onRunClicked()
{
  const QString script = m_scriptEdit->text().trimmed();
  if (script.isEmpty())
  {
    appendSystemLine(tr("请先选择脚本文件"));
    return;
  }
  m_controller->runScript(script, m_argsEdit->text(),
                          m_workdirEdit->text().trimmed());
}

void PythonConsolePanel::onStopClicked()
{
  m_controller->cancelAll();
  appendSystemLine(tr("已请求停止（terminate→kill 递进）"));
}

void PythonConsolePanel::onBrowseScript()
{
  const QString picked = QFileDialog::getOpenFileName(
      this, tr("选择 Python 脚本"), m_scriptEdit->text(),
      tr("Python 脚本 (*.py);;所有文件 (*)"));
  if (!picked.isEmpty())
    m_scriptEdit->setText(picked);
}

void PythonConsolePanel::onBrowseWorkdir()
{
  const QString picked = QFileDialog::getExistingDirectory(
      this, tr("选择工作目录"), m_workdirEdit->text());
  if (!picked.isEmpty())
    m_workdirEdit->setText(picked);
}

void PythonConsolePanel::onRunFinished(
    const PythonConsoleController::RunRecord &record)
{
  const QString argsText = record.args.join(QLatin1Char(' '));
  const QString label = argsText.isEmpty()
      ? QFileInfo(record.scriptPath).fileName()
      : QFileInfo(record.scriptPath).fileName() + QStringLiteral(" ") + argsText;
  auto *item = new QListWidgetItem(
      tr("%1 —— %2").arg(label, PythonConsoleController::recordStatusText(record)));
  item->setData(Qt::UserRole, record.scriptPath);
  item->setData(Qt::UserRole + 1, argsText);
  item->setToolTip(record.scriptPath);
  if (record.cancelled || record.timedOut || record.crashed || record.exitCode != 0)
    PaleoTheme::setItemTextColor(item, PaleoTheme::ItemTextColor::Warning);
  m_historyList->insertItem(0, item);
  while (m_historyList->count() > PythonConsoleController::historyLimit)
    delete m_historyList->takeItem(m_historyList->count() - 1);
}

bool PythonConsolePanel::importable(const QString &path, QString *typeOut) const
{
  const ProjectClassification cls = classifyProjectPath(path);
  if (typeOut)
    *typeOut = cls.type;
  return cls.type != QLatin1String("unknown");
}

void PythonConsolePanel::addResultItem(const QString &path, const QString &kind,
                                       const QString &message)
{
  QString type;
  const bool canImport = importable(path, &type);
  const QString name = QFileInfo(path).fileName();
  auto *item = new QListWidgetItem(
      kind.isEmpty() ? name : tr("%1（%2）").arg(name, kind));
  item->setData(Qt::UserRole, path);
  item->setToolTip(canImport
                       ? tr("%1\n类型：%2——词表命中，可导入").arg(path, type)
                       : tr("%1\n导入词表未识别该类型——不可导入").arg(path));
  if (!canImport)
    PaleoTheme::setItemTextColor(item, PaleoTheme::ItemTextColor::Muted);
  m_resultsList->insertItem(0, item);
  // 展示位封顶（最新在前）；落在磁盘上的产出文件本身不受影响。
  while (m_resultsList->count() > 50)
    delete m_resultsList->takeItem(m_resultsList->count() - 1);
  Q_UNUSED(message);
}

void PythonConsolePanel::onResultSelectionChanged()
{
  QListWidgetItem *item = m_resultsList->currentItem();
  const bool canImport = item && importable(item->data(Qt::UserRole).toString());
  m_importButton->setEnabled(canImport);
  m_importButton->setToolTip(
      canImport ? tr("按导入词表识别类型并导入当前工程")
                : tr("脚本产出须命中导入词表（如 .geojson/.csv）才可导入"));
}

void PythonConsolePanel::onImportClicked()
{
  QListWidgetItem *item = m_resultsList->currentItem();
  if (item)
    emit importRequested(item->data(Qt::UserRole).toString());
}

void PythonConsolePanel::onHistoryActivated(QListWidgetItem *item)
{
  if (!item)
    return;
  m_scriptEdit->setText(item->data(Qt::UserRole).toString());
  m_argsEdit->setText(item->data(Qt::UserRole + 1).toString());
}
