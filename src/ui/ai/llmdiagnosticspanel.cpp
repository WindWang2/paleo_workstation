// 层：视图
#include "llmdiagnosticspanel.h"

#include "../paleotheme.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

// 模型接入诊断面板实现（DESIGN.md：原生控件、spacing token、无新 token；
// 报告纯文本呈现）。视图零逻辑：按钮→意图信号，状态→控件回灌。

namespace {
constexpr int kReportMaxChars = 8000; // 有界展示（与功能层渲染上限同族）
} // namespace

LlmDiagnosticsPanel::LlmDiagnosticsPanel(QWidget *parent) : QWidget(parent) {
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(tokens.spacingXs);

  m_title = new QLabel(tr("模型接入诊断"), this);
  m_title->setObjectName(QStringLiteral("llmDiagTitle"));
  m_title->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  root->addWidget(m_title);

  m_hint = new QLabel(
    tr("以下动作均为显式启动：不点击不联网。目录/文本/流式各至多一次请求；"
       "工具往返最多两次小型模型请求（合成消息，非工程数据）。"),
    this);
  m_hint->setObjectName(QStringLiteral("llmDiagHint"));
  m_hint->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_hint->setWordWrap(true);
  root->addWidget(m_hint);

  auto *buttonRow = new QHBoxLayout;
  buttonRow->setSpacing(tokens.spacingSm);
  m_catalogButton = new QPushButton(tr("获取模型列表"), this);
  m_catalogButton->setObjectName(QStringLiteral("llmDiagCatalog"));
  m_catalogButton->setToolTip(
    tr("查询 <服务地址>/models 目录（可缺席）；不需要先填模型名"));
  m_textButton = new QPushButton(tr("测试文本"), this);
  m_textButton->setObjectName(QStringLiteral("llmDiagText"));
  m_textButton->setToolTip(
    tr("发送一次最小合成文本请求（非流式；需要模型名与本次凭据）"));
  m_streamButton = new QPushButton(tr("测试流式请求"), this);
  m_streamButton->setObjectName(QStringLiteral("llmDiagStream"));
  m_streamButton->setToolTip(tr("发送一次最小合成流式请求（需要模型名与凭据）"));
  m_toolButton = new QPushButton(tr("测试工具往返"), this);
  m_toolButton->setObjectName(QStringLiteral("llmDiagTool"));
  m_toolButton->setToolTip(
    tr("最多两次小型模型请求：诊断专用回声工具（无业务副作用，不执行任何"
       "真实工具）"));
  m_cancelButton = new QPushButton(tr("取消诊断"), this);
  m_cancelButton->setObjectName(QStringLiteral("llmDiagCancel"));
  m_cancelButton->setEnabled(false);
  buttonRow->addWidget(m_catalogButton);
  buttonRow->addWidget(m_textButton);
  buttonRow->addWidget(m_streamButton);
  buttonRow->addWidget(m_toolButton);
  buttonRow->addWidget(m_cancelButton);
  buttonRow->addStretch(1);
  root->addLayout(buttonRow);

  auto *listRow = new QHBoxLayout;
  listRow->setSpacing(tokens.spacingSm);
  m_modelList = new QListWidget(this);
  m_modelList->setObjectName(QStringLiteral("llmDiagModelList"));
  m_modelList->setTextElideMode(Qt::ElideMiddle);
  m_modelList->setMaximumHeight(96);
  listRow->addWidget(m_modelList, 1);
  auto *listActions = new QVBoxLayout;
  m_useModelButton = new QPushButton(tr("填入草稿"), this);
  m_useModelButton->setObjectName(QStringLiteral("llmDiagUseModel"));
  m_useModelButton->setEnabled(false); // 有候选才可用（setModelCandidates 开关）
  m_useModelButton->setToolTip(
    tr("把所选模型 ID 填入上方模型字段（只改草稿；应用仍走原「确定」流程）"));
  listActions->addWidget(m_useModelButton);
  listActions->addStretch(1);
  listRow->addLayout(listActions);
  root->addLayout(listRow);

  m_notice = new QLabel(this);
  m_notice->setObjectName(QStringLiteral("llmDiagNotice"));
  m_notice->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_notice->setWordWrap(true);
  m_notice->hide();
  root->addWidget(m_notice);

  m_report = new QPlainTextEdit(this);
  m_report->setObjectName(QStringLiteral("llmDiagReport"));
  m_report->setReadOnly(true);
  m_report->setPlaceholderText(
    tr("尚未运行诊断——所有动作显式启动，不点击不联网"));
  m_report->setMaximumHeight(200);
  root->addWidget(m_report);

  auto *footerRow = new QHBoxLayout;
  m_exportButton = new QPushButton(tr("导出报告摘要…"), this);
  m_exportButton->setObjectName(QStringLiteral("llmDiagExport"));
  m_exportButton->setToolTip(tr("把当前摘要保存为文本文件（不含密钥与原始"
                                "请求/应答）"));
  footerRow->addWidget(m_exportButton);
  footerRow->addStretch(1);
  root->addLayout(footerRow);

  connect(m_catalogButton, &QPushButton::clicked, this, [this] {
    emit actionRequested(static_cast<int>(Action::Catalog));
  });
  connect(m_textButton, &QPushButton::clicked, this,
          [this] { emit actionRequested(static_cast<int>(Action::Text)); });
  connect(m_streamButton, &QPushButton::clicked, this,
          [this] { emit actionRequested(static_cast<int>(Action::Stream)); });
  connect(m_toolButton, &QPushButton::clicked, this,
          [this] { emit actionRequested(static_cast<int>(Action::Tool)); });
  connect(m_cancelButton, &QPushButton::clicked, this, [this] {
    emit actionRequested(static_cast<int>(Action::Cancel));
  });
  connect(m_useModelButton, &QPushButton::clicked, this, [this] {
    if (auto *item = m_modelList->currentItem())
      emit modelChosen(item->text());
  });
  connect(m_exportButton, &QPushButton::clicked, this,
          &LlmDiagnosticsPanel::exportSummary);
}

void LlmDiagnosticsPanel::setSummary(const QString &summary) {
  m_lastSummary = summary;
  // 有界展示：超长摘要截尾（功能层渲染本身已有行数上限，此处是纵深）。
  m_report->setPlainText(summary.size() > kReportMaxChars
                           ? summary.left(kReportMaxChars)
                           : summary);
  m_notice->hide();
}

void LlmDiagnosticsPanel::setBusy(bool busy) {
  m_catalogButton->setEnabled(!busy);
  m_textButton->setEnabled(!busy);
  m_streamButton->setEnabled(!busy);
  m_toolButton->setEnabled(!busy);
  m_cancelButton->setEnabled(busy);
}

void LlmDiagnosticsPanel::setModelCandidates(const QStringList &models) {
  m_modelList->clear();
  for (const QString &model : models)
    m_modelList->addItem(model); // 纯文本条目（不解释为富文本/链接）
  m_useModelButton->setEnabled(!models.isEmpty());
}

void LlmDiagnosticsPanel::showNotice(const QString &reason) {
  if (reason.isEmpty()) {
    m_notice->hide();
    return;
  }
  m_notice->setText(reason);
  m_notice->show();
}

void LlmDiagnosticsPanel::exportSummary() {
  // 显式导出：有限文本摘要（功能层渲染，已脱敏；不含原始请求/应答）。
  if (m_lastSummary.isEmpty()) {
    showNotice(tr("尚无报告可导出"));
    return;
  }
  const QString path = QFileDialog::getSaveFileName(
    this, tr("导出诊断摘要"), QStringLiteral("llm-diagnostics.txt"),
    tr("文本文件 (*.txt)"));
  if (path.isEmpty())
    return;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    showNotice(tr("导出失败：%1").arg(file.errorString()));
    return;
  }
  if (file.write(m_lastSummary.toUtf8()) < 0) {
    showNotice(tr("导出失败：%1").arg(file.errorString()));
    return;
  }
  showNotice(tr("已导出：%1").arg(path));
}
