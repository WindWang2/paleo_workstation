// 层：视图
#pragma once
#include "../../ai/chat/chatmessage.h"
#include "../paleotheme.h"
#include <QHash>
#include <QString>
#include <QWidget>

class AiChatController;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QFrame;
class QTextBrowser;
class QTextEdit;
class QVBoxLayout;

// ui/ai/ — AI 地质对话助手 dock（方向51 骨架，方向61 工具两态卡片、方向62 补全用户可达面：
// 会话下拉/重命名/删除、markdown 渲染、复制/重发/导出）。
//
// 层站位：本面板只渲染 + 只发意图。所有编排（会话/流式/工具分发/落盘）在
// workflow/aichatcontroller；本文件不碰网络、不碰文件、不发业务动作。
//
// DESIGN.md：字体/色/spacing 一律经 PaleoTheme 取（面板代码不得自带色值），
// 状态胶囊走 capsuleLabel（Neutral/Warning/Error 三态对应未配置/流式中/错误；
// 工具卡两态为 Neutral 执行中 / Success 完成 / Error 失败 / Warning 未接线）。
// 方向62 未新增 token：会话行/操作按钮沿用既有 dock 面板口径（QComboBox +
// QPushButton 原生控件），对话气泡不引入（DESIGN.md 无此组件，transcript
// 平铺是既有形态——见 .goal-loop-ledger-ai-ux.md 决策记录）。
class AiAssistDock : public QWidget {
  Q_OBJECT
public:
  explicit AiAssistDock(AiChatController *controller, QWidget *parent = nullptr);
  // 重挂编排器（装配幂等分支用）：旧控制器先解绑，再连新的并刷新。
  void attachController(AiChatController *controller);
  // 宿主接线用（openSessionRequested 等信号落到哪个编排器由宿主决定，
  // 幂等重挂后取到的是最新控制器）。
  AiChatController *controller() const { return m_controller; }

public slots:
  void refresh(); // 按 controller 状态重画（状态行/按钮可用性/历史）

signals:
  // 面板不自己弹对话框：配置/删除确认属于宿主（主窗口）的职责。
  void configureRequested();
  void openSessionRequested(const QString &sessionId);
  void renameSessionRequested(const QString &sessionId, const QString &title);
  void deleteSessionRequested(const QString &sessionId);
  // 方向 93：结果卡导航意图（定位不是数据修改）——宿主接线消费
  // （先例：DataPage::assetActivated 通道 / focusVersion / selectAssetsForEntities）。
  void assetNavigateRequested(const QString &assetId);
  void entityNavigateRequested(const QString &entityId);
  void lineageNavigateRequested(const QString &assetId, const QString &versionId);

private:
  void sendCurrentText();
  void appendMessage(const ChatMessage &message);
  void appendDelta(const QString &text);
  void appendError(const QString &message);
  void addToolCard(const ChatToolCall &call, const QString &statusText);
  void markToolRunning(const ChatToolCall &call);
  void markToolResult(const ChatToolCall &call, bool ok, const QString &summary);
  // 方向 93：完整出参 → 卡内结构化视图（表格/键值对/血缘小图/兜底折叠）。
  // 直连 AiChatToolRunner::toolFinished（全量 JSON；toolResultReady 只有
  // 160 字符摘要）——workflow 零改动，呈现层自取所需。
  void showToolResult(const ChatToolCall &call, bool ok, const QString &resultJson);
  void clearToolCards();
  void writeBlock(const QString &html);
  void bindController();
  // 会话面（方向62）
  void renderSession();    // 按编排器历史整页重画（切换/重发截断后）
  void refreshSessions();  // 会话下拉重建（当前会话固定首位）
  void renameComboSession(); // 弹标题输入（视图本地输入框，非业务对话框）
  // 消息面（方向62）
  void finalizeAssistantBlock(); // 流结束：增量纯文本 → markdown 渲染定型
  void copyTranscript();
  void exportSession();
  void updateActionStates(); // 操作按钮可用性（复制/导出/重发/重命名/删除）

  // 工具卡两态（方向61）：同一张卡上「执行中 → 结果」翻面；胶囊状态色随
  // CapsuleKind 重建（capsuleLabel 的样式在构造时按 kind 钉死）。
  // 方向 93：结果态在卡内追加结构化视图（AiToolResultView）。
  struct ToolCard {
    QFrame *card = nullptr;
    class QHBoxLayout *row = nullptr;
    QLabel *capsule = nullptr;
    QLabel *summary = nullptr;
    class AiToolResultView *resultView = nullptr;
  };
  void swapCapsule(ToolCard &card, const QString &text,
                   PaleoTheme::CapsuleKind kind);

  AiChatController *m_controller = nullptr;
  QLabel *m_status = nullptr;
  QComboBox *m_sessions = nullptr;
  QPushButton *m_rename = nullptr;
  QPushButton *m_delete = nullptr;
  QTextBrowser *m_transcript = nullptr;
  QWidget *m_cards = nullptr;
  QVBoxLayout *m_cardLayout = nullptr;
  QTextEdit *m_input = nullptr;
  QPushButton *m_send = nullptr;
  QPushButton *m_stop = nullptr;
  QPushButton *m_new = nullptr;
  QPushButton *m_configure = nullptr;
  QPushButton *m_copy = nullptr;
  QPushButton *m_export = nullptr;
  QPushButton *m_resend = nullptr;
  // 会话下拉的 id 列表（与下拉项一一对应；重建期间阻塞 activated）。
  QStringList m_sessionIds;
  QString m_currentSessionId;
  // 每会话滚动位置（切换回来时还原；首见会话落底）。
  QHash<QString, int> m_scrollPositions;
  // 流式块记账：起点位置 + 增量累积（finalize 时一次性替换为 markdown）。
  int m_assistantStart = -1;
  QString m_assistantDraft;
  QHash<QString, ToolCard> m_toolCards; // key = tool_call id
};
