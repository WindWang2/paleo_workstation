// 层：视图
#pragma once
#include "../../ai/chat/chatmessage.h"
#include <QWidget>

class AiChatController;
class QLabel;
class QPushButton;
class QTextBrowser;
class QTextEdit;
class QVBoxLayout;

// ui/ai/ — AI 地质对话助手 dock（方向51）。
//
// 层站位：本面板只渲染 + 只发意图。所有编排（会话/流式/工具分发/落盘）在
// workflow/aichatcontroller；本文件不碰网络、不碰文件、不发业务动作。
//
// DESIGN.md：字体/色/spacing 一律经 PaleoTheme 取（面板代码不得自带色值），
// 状态胶囊走 capsuleLabel（Neutral/Warning/Error 三态对应未配置/流式中/错误）。
class AiAssistDock : public QWidget {
  Q_OBJECT
public:
  explicit AiAssistDock(AiChatController *controller, QWidget *parent = nullptr);
  // 重挂编排器（装配幂等分支用）：旧控制器先解绑，再连新的并刷新。
  void attachController(AiChatController *controller);

public slots:
  void refresh(); // 按 controller 状态重画（状态行/按钮可用性/历史）

signals:
  // 面板不自己弹对话框：配置端点/密钥属于宿主（主窗口）的职责。
  void configureRequested();
  void openSessionRequested(const QString &sessionId);

private:
  void sendCurrentText();
  void appendMessage(const ChatMessage &message);
  void appendDelta(const QString &text);
  void appendError(const QString &message);
  void addToolCard(const ChatToolCall &call, const QString &statusText);
  void clearToolCards();
  void writeBlock(const QString &html);
  void bindController();

  AiChatController *m_controller = nullptr;
  QLabel *m_status = nullptr;
  QTextBrowser *m_transcript = nullptr;
  QWidget *m_cards = nullptr;
  QVBoxLayout *m_cardLayout = nullptr;
  QTextEdit *m_input = nullptr;
  QPushButton *m_send = nullptr;
  QPushButton *m_stop = nullptr;
  QPushButton *m_new = nullptr;
  QPushButton *m_configure = nullptr;
};
