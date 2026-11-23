// 层：视图
#pragma once
#include <QWidget>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

// ui/ai/ — 模型接入诊断面板（H6-W7）。
//
// 层站位：只发意图、只展示状态。网络与报告逻辑在功能层
// （workflow/llmconnectiondiagnostics），装配在 aiconfigurationwiring。
// 本面板不做任何网络调用、不读配置文件、不碰钥匙串。
//
// 诚实面：
//   · 所有动作显式启动（按钮触发）；不做任何自动联网。
//   · 报告区为纯文本（QPlainTextEdit）：服务端来源的模型名/错误文本一律
//     按文本显示——不进入富文本/链接行为（不加载远端图片/URL）。
//   · 工具往返按钮的提示写明「最多两次小型模型请求」。
//   · 导出只写出当前摘要文本（功能层渲染，已脱敏）；导出失败如实提示，
//     不冒充成功、不改任何配置。
class LlmDiagnosticsPanel : public QWidget {
  Q_OBJECT
public:
  // 动作种类（与 workflow 层 LlmDiagnosticsAction 数值一一对应；Cancel 额外）。
  enum class Action { Catalog = 0, Text = 1, Stream = 2, Tool = 3, Cancel = 4 };
  explicit LlmDiagnosticsPanel(QWidget *parent = nullptr);

  // 功能层状态回灌（无结构化解释——视图只展示）。
  void setSummary(const QString &summary);
  void setBusy(bool busy);
  void setModelCandidates(const QStringList &models);
  // 本地拒绝/导出失败提示（有界单行；不合并进报告正文）。
  void showNotice(const QString &reason);

signals:
  void actionRequested(int action);
  // 选择目录条目只填草稿（模型字段），不自动应用配置。
  void modelChosen(const QString &model);

private:
  void exportSummary();

  QLabel *m_title = nullptr;
  QLabel *m_hint = nullptr;
  QLabel *m_notice = nullptr;
  QPushButton *m_catalogButton = nullptr;
  QPushButton *m_textButton = nullptr;
  QPushButton *m_streamButton = nullptr;
  QPushButton *m_toolButton = nullptr;
  QPushButton *m_cancelButton = nullptr;
  QPushButton *m_useModelButton = nullptr;
  QPushButton *m_exportButton = nullptr;
  QListWidget *m_modelList = nullptr;
  QPlainTextEdit *m_report = nullptr;
  QString m_lastSummary;
};
