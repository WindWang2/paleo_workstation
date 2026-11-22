// 层：视图
#pragma once
#include "../../ai/chat/chatmessage.h"
#include "../../workflow/aiconversationpresentation.h"
#include "../paleotheme.h"
#include <QHash>
#include <QString>
#include <QWidget>

class AiChatController;
class AiConversationCard;
class AiOperationDetailWidget;
class AiContextBar;
namespace paleo::chat { class AiOperationPresentation; }
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QFrame;
class QTextBrowser;
class QTextEdit;
class QVBoxLayout;

// ui/ai/ — AI 地质对话助手 dock（方向51 骨架，方向61 工具两态卡片、方向62
// 补全用户可达面；G5 会话工作台：上下文归属条、实时+历史统一工具卡、
// 操作详情面与草稿/键盘生命周期）。
//
// 层站位：本面板只渲染 + 只发意图。所有编排（会话/流式/工具分发/落盘）在
// workflow/aichatcontroller；操作查询/取消/恢复经 AiOperationPresentation
// adapter 走既有 CapabilityRecovery——面板不实现第二套执行/恢复引擎。
//
// DESIGN.md：字体/色/spacing 一律经 PaleoTheme 取（面板代码不得自带色值），
// 状态胶囊走 capsuleLabel；对话气泡不引入（transcript 平铺是既有形态）。
class AiAssistDock : public QWidget {
  Q_OBJECT
public:
  explicit AiAssistDock(AiChatController *controller, QWidget *parent = nullptr);
  // 重挂编排器（装配幂等分支用）：旧控制器先解绑，再连新的并刷新。
  void attachController(AiChatController *controller);
  // 宿主接线用（openSessionRequested 等信号落到哪个编排器由宿主决定，
  // 幂等重挂后取到的是最新控制器）。
  AiChatController *controller() const { return m_controller; }
  // G5：注入/重挂操作展示 adapter（幂等；null = 解绑）。adapter 由组装根
  // 按真实 CapabilityHost/DocumentSession/controller 创建，面板不建服务。
  void attachOperationPresenter(paleo::chat::AiOperationPresentation *presenter);

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

protected:
  // W7：输入框键盘生命周期（Enter 发送/Shift+Enter 换行/IME 组字/重复键）。
  bool eventFilter(QObject *watched, QEvent *event) override;
  // W8：主题切换刷新既有卡与状态样式（不只影响新建卡）。
  void changeEvent(QEvent *event) override;

private:
  void sendCurrentText();
  void appendMessage(const ChatMessage &message);
  void appendDelta(const QString &text);
  void appendError(const QString &message);
  void addToolCard(const ChatToolCall &call, const QString &statusText);
  void markToolRunning(const ChatToolCall &call);
  void markToolResult(const ChatToolCall &call, bool ok, const QString &summary);
  // G5：完整出参（raw）→ 统一投影卡。直连 AiChatToolRunner::toolFinished
  // （全量 JSON）；历史 envelope 走同一 renderer（buildHistoryCards）。
  void showToolResult(const ChatToolCall &call, bool ok, const QString &resultJson);
  void clearToolCards();
  void syncCardOperationControls(AiConversationCard *card);
  void retireCard(AiConversationCard *card);
  QList<AiConversationCard *> operationCards(const QString &operationId) const;
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
  // G5 会话工作台
  void buildHistoryCards();          // W2：历史工具卡恢复（有界窗口 + 惰性卡）
  void navigateHistory(int delta);   // W8：更早/更新结果卡窗口翻页
  void enforceExpandedCap(AiConversationCard *card); // W8：展开上限（LRU 收敛）
  void refreshContextBar();          // W1：上下文条现值刷新
  void saveCurrentDraft();           // W7：当前输入 → 会话草稿
  void restoreDraft(const QString &sessionId); // W7：会话草稿 → 输入框
  void handleCardOperationIntents(AiConversationCard *card); // 卡意图 → adapter
  void handlePresenterSignals();     // adapter 信号 → 详情面/卡忙碌

  AiChatController *m_controller = nullptr;
  paleo::chat::AiOperationPresentation *m_presenter = nullptr; // 弱引用（组装根拥有）
  AiContextBar *m_contextBar = nullptr;
  AiOperationDetailWidget *m_detail = nullptr;
  QLabel *m_status = nullptr;
  QComboBox *m_sessions = nullptr;
  QPushButton *m_rename = nullptr;
  QPushButton *m_delete = nullptr;
  QTextBrowser *m_transcript = nullptr;
  class PaleoEmptyStateLabel *m_emptyState = nullptr; // #870 对话区空态指引
  void updateEmptyState();
  QWidget *m_cards = nullptr;
  QVBoxLayout *m_cardLayout = nullptr;
  QLabel *m_historyNote = nullptr;    // W8：更早结果窗口注记
  QPushButton *m_historyOlder = nullptr;
  QPushButton *m_historyNewer = nullptr;
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
  // W7：每会话未发送草稿（UI 内保存/恢复；不进模型历史/会话存储）。
  QHash<QString, QString> m_drafts;
  bool m_restoringDraft = false;
  // 流式块记账：起点位置 + 增量累积（finalize 时一次性替换为 markdown）。
  int m_assistantStart = -1;
  QString m_assistantDraft;
  QHash<QString, AiConversationCard *> m_toolCards; // key = tool_call id（实时）
  QVector<paleo::chat::HistoryToolCall> m_historyTools; // W2 只读投影
  QList<AiConversationCard *> m_historyCards;           // 当前窗口内的历史卡
  int m_historyWindowStart = 0; // W8 卡窗口起点（kCardWidgetCap 张为一页）
  QList<AiConversationCard *> m_expandedOrder;  // W8 展开次序（超限 LRU 收敛）
  static constexpr int kCardWidgetCap = 64;   // 活跃卡 widget 上限
  static constexpr int kExpandedCardCap = 8;  // 同时展开卡上限
};
