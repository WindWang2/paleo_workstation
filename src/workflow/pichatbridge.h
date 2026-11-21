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
  // G2 测试缝：受控进程内 peer 取代 Node 会话 worker（仅测试注入，生产为
  // 空）。桥的相位/控制/清理记账全部保持真实路径；driver 在 worker 线程只
  // 经线程安全 channel（takeOutgoing/acceptPeer）与 task->cancelRequested()
  // 交互，notify 即生产路径的 incoming 通知（投递到宿主线 drain），返回值
  // 空 = ok，否则为 ConversationResult.code。
  using WorkerDriver = std::function<QString(PaleoTask *, const std::shared_ptr<paleo::pi::ConversationChannel> &,
                                             const std::function<void()> &)>;
  void setWorkerDriver(WorkerDriver driver) { m_workerDriver = std::move(driver); }
signals:
  void modelRequested();
  void toolRequested(const ChatToolCall &);
  void finished(const QString &code);
  void failed(const QString &code); // Immediate host interruption; cleanup finishes asynchronously.
  // G2-W5：收尾补充——worker 侧报了与终态主因不同的失败（如进程回收失败）
  // 时各发一次，供诊断呈现；不改变 finished 的主因，也不撤销已提交工具。
  void cleanupNotice(const QString &workerCode);
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
  WorkerDriver m_workerDriver;
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
