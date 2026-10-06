// 层：视图
#pragma once
#include "../../ai/chat/llmclient.h"
#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// ui/ai/ — 大模型配置对话框（方向62：图形化配置收编「手改 JSON + 环境变量」）。
//
// 层站位：对话框只做表单编辑 + 结构校验提示，**不落盘、不碰钥匙串**——
// 确认时发 applyRequested(表单值, 新密钥意图, 清除意图)，落盘/钥匙串/
// 应用到编排器全在宿主接 AiChatController::applyConfig（功能层编排）。
//
// 安全口径（硬约束）：
//   · 密钥字段 Password 回显；已存密钥**永不回显明文**——状态行只说
//     「已设置 / 未设置」，测试按「明文不出现在任何 label」断言。
//   · 无 QtKeychain 的构建（或 PALEO_LLM_NO_KEYCHAIN=1）密钥区整体禁用，
//     如实说明可经 PALEO_LLM_API_KEY 提供（先例：llmkeystore 降级口径）。
//   · 结构校验失败（地址/模型）不发 apply；密钥缺失允许应用——
//     助手保持禁用态并在状态行如实说明（诚实面，不冒充可用）。
//   · 异步应用期间按钮禁用（busy 态），防止重复提交；结果经
//     onApplyResult 回流，失败停留并提示，成功才关窗。
class LlmConfigDialog : public QDialog {
  Q_OBJECT
public:
  explicit LlmConfigDialog(const LlmConfig &current, QWidget *parent = nullptr);

  // 宿主回灌应用结果（controller->applyConfig 的 done 回调落脚点）。
  void onApplyResult(bool ok, const QString &error);

signals:
  // 确认并通过结构校验后发出（一次提交至多一次；busy 期间再点无效）。
  // newKey 非空 = 写入新密钥；clearKey 且 newKey 空 = 清除；都空 = 不变。
  void applyRequested(const LlmConfig &formConfig, const QByteArray &newKey,
                      bool clearKey);

private:
  void accept() override;
  LlmConfig formConfig() const;
  void note(const QString &text);
  void setBusy(bool busy);

  LlmConfig m_current;
  QLineEdit *m_endpoint = nullptr;
  QLineEdit *m_model = nullptr;
  QSpinBox *m_maxTokens = nullptr;
  QSpinBox *m_timeout = nullptr;
  QCheckBox *m_stream = nullptr;
  QCheckBox *m_insecureHttp = nullptr;
  QWidget *m_keyArea = nullptr;
  QLabel *m_keyState = nullptr;
  QLineEdit *m_keyInput = nullptr;   // 新密钥（留空 = 不变）
  QPushButton *m_keyClear = nullptr; // 勾选 = 清除已存密钥
  QDialogButtonBox *m_buttons = nullptr;
  QLabel *m_note = nullptr;
  bool m_busy = false;
};
