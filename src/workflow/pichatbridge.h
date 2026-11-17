// 层：功能
#pragma once
#include "ai/chat/chatmessage.h"
#include "ai/chat/domaintools.h"
#include "services/piconversation.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class PaleoTaskService;
class PaleoTask;
// One fixed optional core loop. No model settings/auth client or tool executor
// lives here: the existing controller receives requests and remains authority.
class PiChatBridge : public QObject {
  Q_OBJECT
public:
  explicit PiChatBridge(QObject *parent = nullptr);
  ~PiChatBridge() override;
  QString start(const paleo::pi::RuntimeConfig &, PaleoTaskService *, const QString &session,
      const QString &turn, const QVector<AiToolSpec> &, std::function<bool()> contextCurrent);
  bool busy() const { return m_active; }
  bool modelDelta(const QString &);
  bool modelFinished(const QVector<ChatToolCall> &, const QString &reason);
  void modelFailed(const QString &code);
  bool toolFinished(const ChatToolCall &, bool ok, const QString &modelContent);
  void abort(bool explicitUser);
  QVector<AiToolSpec> toolSnapshot() const { return m_tools; }
signals:
  void modelRequested();
  void toolRequested(const ChatToolCall &);
  void finished(const QString &code);
  void failed(const QString &code); // Immediate host interruption; cleanup finishes asynchronously.
private:
  enum class Phase { Opening, Configuring, AwaitModel, Model, AwaitTool, Tool, AwaitIdle, Closing, Closed };
  void drain();
  void frame(const QJsonObject &);
  void configureNext();
  bool send(const QString &, const QJsonObject & = {});
  void fail(const QString &);
  void controlTick();
  void flushModel();
  std::shared_ptr<paleo::pi::ConversationChannel> m_channel;
  QPointer<PaleoTask> m_task;
  QPointer<PaleoTaskService> m_service;
  QVector<AiToolSpec> m_tools;
  QVector<ChatToolCall> m_calls;
  QHash<QString, QJsonObject> m_completed;
  std::function<bool()> m_contextCurrent;
  QTimer m_timer;
  QElapsedTimer m_controlAge;
  quint64 m_generation = 0, m_round = 0;
  int m_definition = 0, m_nextCall = 0;
  Phase m_phase = Phase::Closed;
  QString m_failure;
  QString m_pendingText, m_modelReason;
  qsizetype m_pendingTextBytes = 0;
  int m_sentCalls = 0;
  bool m_modelEnding = false;
  bool m_drainScheduled = false;
  bool m_active = false;
};
