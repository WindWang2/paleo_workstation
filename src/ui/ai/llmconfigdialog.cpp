// 层：视图
#include "llmconfigdialog.h"

#include "../../ai/chat/llmkeystore.h"
#include "../paleotheme.h"
#include "llmdiagnosticspanel.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

// ui/ai/ — 大模型配置对话框实现（DESIGN.md：表单走 QFormLayout 标准
// 形态，间距 spacingSm/spacingMd；不新增 token，控件原生渲染）。
//
// 持久化不在本文件：accept() 只做结构校验 + 发 applyRequested 意图，
// 落盘/钥匙串/应用编排由宿主接 AiChatController::applyConfig 完成，
// 结果经 onApplyResult 回流（失败停留提示，成功关窗）。

LlmConfigDialog::LlmConfigDialog(const LlmConfig &current, QWidget *parent)
  : QDialog(parent), m_current(current) {
  setWindowTitle(tr("大模型配置"));
  auto *root = new QVBoxLayout(this);
  const PaleoTheme::ThemeTokens &tokens = PaleoTheme::tokens();
  root->setContentsMargins(tokens.spacingMd, tokens.spacingMd, tokens.spacingMd,
                           tokens.spacingMd);
  root->setSpacing(tokens.spacingSm);

  auto *form = new QFormLayout;
  form->setSpacing(tokens.spacingSm);
  m_endpoint = new QLineEdit(current.endpoint.toString(), this);
  m_endpoint->setObjectName(QStringLiteral("llmConfigEndpoint"));
  m_endpoint->setPlaceholderText(tr("https://服务地址/v1"));
  m_model = new QLineEdit(current.model, this);
  m_model->setObjectName(QStringLiteral("llmConfigModel"));
  m_maxTokens = new QSpinBox(this);
  m_maxTokens->setObjectName(QStringLiteral("llmConfigMaxTokens"));
  m_maxTokens->setRange(1, 200000);
  m_maxTokens->setValue(current.maxTokens > 0 ? current.maxTokens : 1024);
  m_timeout = new QSpinBox(this);
  m_timeout->setObjectName(QStringLiteral("llmConfigTimeout"));
  m_timeout->setRange(1000, 600000);
  m_timeout->setSingleStep(1000);
  m_timeout->setValue(current.timeoutMs > 0 ? current.timeoutMs : 30000);
  m_timeout->setSuffix(tr(" ms"));
  m_stream = new QCheckBox(tr("流式输出（SSE 增量）"), this);
  m_stream->setObjectName(QStringLiteral("llmConfigStream"));
  m_stream->setChecked(current.stream);
  m_insecureHttp = new QCheckBox(tr("允许非本机 http 端点（明文传输，自担风险）"),
                                 this);
  m_insecureHttp->setObjectName(QStringLiteral("llmConfigInsecureHttp"));
  m_insecureHttp->setChecked(current.allowInsecureHttp);
  form->addRow(tr("服务地址"), m_endpoint);
  form->addRow(tr("模型"), m_model);
  form->addRow(tr("最大输出 token"), m_maxTokens);
  form->addRow(tr("空闲超时"), m_timeout);
  form->addRow(QString(), m_stream);
  form->addRow(QString(), m_insecureHttp);
  root->addLayout(form);

  // 密钥区：状态行只报「已设置/未设置」，永不回显明文。
  m_keyArea = new QWidget(this);
  m_keyArea->setObjectName(QStringLiteral("llmConfigKeyArea"));
  auto *keyLayout = new QVBoxLayout(m_keyArea);
  keyLayout->setContentsMargins(0, 0, 0, 0);
  keyLayout->setSpacing(tokens.spacingXs);
  m_keyState = new QLabel(
    current.apiKey.isEmpty() ? tr("未设置 API 密钥") : tr("已设置 API 密钥（不回显）"),
    m_keyArea);
  m_keyState->setObjectName(QStringLiteral("llmConfigKeyState"));
  m_keyState->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  keyLayout->addWidget(m_keyState);
  auto *keyRow = new QHBoxLayout;
  m_keyInput = new QLineEdit(m_keyArea);
  m_keyInput->setObjectName(QStringLiteral("llmConfigKeyInput"));
  m_keyInput->setEchoMode(QLineEdit::Password);
  m_keyInput->setPlaceholderText(tr("输入新密钥（留空 = 保持现状）"));
  m_keyClear = new QPushButton(tr("清除已存密钥"), m_keyArea);
  m_keyClear->setObjectName(QStringLiteral("llmConfigKeyClear"));
  m_keyClear->setCheckable(true);
  keyRow->addWidget(m_keyInput, 1);
  keyRow->addWidget(m_keyClear);
  keyLayout->addLayout(keyRow);
  root->addWidget(m_keyArea);
  if (!LlmKeyStore::available()) {
    // 如实禁用：没有钥匙串就不提供「写钥匙串」的输入（不降级成明文文件）。
    m_keyArea->setEnabled(false);
    m_keyState->setText(tr(
      "系统钥匙串不可用——密钥可经环境变量 PALEO_LLM_API_KEY 提供"));
  }

  // H6 诊断面板（显式启动；诊断不落盘/不碰钥匙串/不调 applyConfig）。
  m_diagnostics = new LlmDiagnosticsPanel(this);
  root->addWidget(m_diagnostics);
  connect(m_diagnostics, &LlmDiagnosticsPanel::actionRequested, this,
          [this](int action) { emit diagnosticsActionRequested(action); });
  connect(m_diagnostics, &LlmDiagnosticsPanel::modelChosen, this,
          &LlmConfigDialog::setDraftModel);

  auto *folderRow = new QHBoxLayout;
  auto *openFolder = new QPushButton(tr("打开配置文件所在目录"), this);
  openFolder->setObjectName(QStringLiteral("llmConfigOpenFolder"));
  // 便利入口（打开目录，非业务动作）；目录打开失败不阻塞配置流程。
  connect(openFolder, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(
      QFileInfo(LlmConfig::path()).absolutePath()));
  });
  m_note = new QLabel(this);
  m_note->setObjectName(QStringLiteral("llmConfigNote"));
  m_note->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_note->setWordWrap(true);
  folderRow->addWidget(openFolder);
  m_pythonEnvironment = new QPushButton(tr("Python 计算环境…"), this);
  m_pythonEnvironment->setObjectName(QStringLiteral("llmConfigPythonEnvironment"));
  connect(m_pythonEnvironment, &QPushButton::clicked, this,
          &LlmConfigDialog::pythonEnvironmentRequested);
  folderRow->addWidget(m_pythonEnvironment);
  folderRow->addWidget(m_note, 1);
  root->addLayout(folderRow);

  // H6-W1：已有超域值诚实提示——保留显示不裁剪，应用时可见拒绝（不无声
  // 裁剪、不抬 load 上限）。
  if (m_maxTokens->value() > 32768)
    note(tr("当前最大输出 token %1 超出持久化域 [1, 32768]：显示保留原值；"
              "应用前请调整（见「应用」提示）。")
           .arg(m_maxTokens->value()));

  m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                   this);
  connect(m_buttons, &QDialogButtonBox::accepted, this,
          &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(m_buttons);
}

LlmConfig LlmConfigDialog::formConfig() const {
  LlmConfig config;
  config.endpoint = QUrl(m_endpoint->text().trimmed());
  config.model = m_model->text().trimmed();
  config.maxTokens = m_maxTokens->value();
  config.timeoutMs = m_timeout->value();
  config.stream = m_stream->isChecked();
  config.allowInsecureHttp = m_insecureHttp->isChecked();
  // H6-W1 表单往返修正：totalTimeoutMs 无 UI 配置项（G1 口径），未编辑时
  // 必须保持当前值原样流转——不得被默认 300000 无声覆盖（真实路径：
  // 打开→只改模型名→原 apply→重载，其余字段不变）。
  config.totalTimeoutMs = m_current.totalTimeoutMs;
  return config;
}

void LlmConfigDialog::note(const QString &text) {
  m_note->setText(text);
}

void LlmConfigDialog::setBusy(bool busy) {
  m_busy = busy;
  m_pythonEnvironment->setEnabled(!busy);
  m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!busy);
  m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!busy);
}

void LlmConfigDialog::accept() {
  if (m_busy)
    return; // 异步应用在途：防重复提交
  // 结构校验（地址/模型）——密钥不在此判（缺失允许应用，助手如实禁用）。
  LlmConfig probe = formConfig();
  probe.apiKey = QByteArrayLiteral("probe-placeholder");
  const QString structural = probe.validate();
  if (!structural.isEmpty()) {
    note(structural);
    return;
  }
  // H6-W1 maxTokens 持久化域守卫：load 的真实读取域为 [1, 32768]（钳位）。
  // 超域值可见拒绝——不无声裁剪、不保存成重启后读回不同的值；也不抬高
  // load 上限迁就 UI（诚实处理：当前值超域时保留显示并明确提示）。
  const LlmConfig form = formConfig();
  if (form.maxTokens > 32768) {
    note(tr("最大输出 token %1 超出持久化域 [1, 32768]——为避免保存后重启"
            "读回不同的值，应用被拒绝。请调整后再应用。")
           .arg(form.maxTokens));
    return;
  }
  const QByteArray typed = m_keyInput->text().toUtf8();
  const bool clearRequested =
    m_keyClear->isChecked() && m_keyClear->isEnabled();
  setBusy(true);
  note(tr("正在应用配置…"));
  emit applyRequested(formConfig(), typed, clearRequested && typed.isEmpty());
}

void LlmConfigDialog::onApplyResult(bool ok, const QString &error) {
  if (!ok) {
    setBusy(false);
    note(error.isEmpty() ? tr("应用失败") : error);
    return;
  }
  QDialog::accept();
}

void LlmConfigDialog::reject() {
  // busy 在途（密钥异步写入）时拦截 Esc / Cancel / 编程式 reject：关窗会让
  // 宿主侧完成回调随对话框连接一起作废，配置已落盘却未应用到运行时。
  if (m_busy)
    return;
  QDialog::reject();
}

void LlmConfigDialog::closeEvent(QCloseEvent *event) {
  // 系统关闭（标题栏 X / Alt+F4 / closeAllWindows）走 closeEvent，不经
  // reject()——busy 在途同样拦截，理由同 reject()。
  if (m_busy) {
    event->ignore();
    return;
  }
  QDialog::closeEvent(event);
}

// ---------------------------------------------------------------------------
// H6 诊断接缝（视图只发意图/展示；网络与报告在功能层协调器）
// ---------------------------------------------------------------------------

paleo::chat::LlmDiagnosticCandidate LlmConfigDialog::buildDiagnosticCandidate() const {
  paleo::chat::LlmDiagnosticCandidate candidate;
  candidate.endpoint = QUrl(m_endpoint->text().trimmed());
  candidate.model = m_model->text().trimmed();
  candidate.stream = m_stream->isChecked();
  candidate.timeoutMs = m_timeout->value();
  candidate.totalTimeoutMs = m_current.totalTimeoutMs;
  candidate.maxTokens = m_maxTokens->value();
  candidate.allowInsecureHttp = m_insecureHttp->isChecked();
  const QByteArray typed = m_keyInput->text().toUtf8();
  const bool clearRequested = m_keyClear->isChecked() && m_keyClear->isEnabled();
  // 凭据来源判定（W2 规则在功能层助手）：显式输入优先；清除意图不复用；
  // 端点身份完全一致（含 base path）才沿用当前内存值；端点已变更且未输入
  // 目标凭据 → 无凭据（诊断本地拒绝），绝不静默搬运旧 key。
  candidate.keySource = paleo::chat::llmResolveDiagnosticKeySource(
    !typed.isEmpty(), clearRequested, candidate.endpoint, m_current.endpoint,
    !m_current.apiKey.isEmpty());
  using KeySource = paleo::chat::LlmDiagnosticCandidate::KeySource;
  if (candidate.keySource == KeySource::ExplicitInput)
    candidate.key = typed;
  else if (candidate.keySource == KeySource::InMemoryCurrent)
    candidate.key = m_current.apiKey;
  return candidate;
}

void LlmConfigDialog::setDiagnosticsSummary(const QString &summary) {
  m_diagnostics->setSummary(summary);
}

void LlmConfigDialog::setDiagnosticsBusy(bool busy) {
  m_diagnostics->setBusy(busy);
}

void LlmConfigDialog::setModelCandidates(const QStringList &models) {
  m_diagnostics->setModelCandidates(models);
}

void LlmConfigDialog::setDraftModel(const QString &model) {
  // 只改草稿；生效仍走原「应用/确定」流程（诊断不调 applyConfig）。
  if (model.isEmpty())
    return;
  m_model->setText(model);
}
