// 层：测试壳
// G2-W5/W6：Pi 桥终态结算与有界协议矩阵。FakeNode 经线程安全 channel 顶替
// Node 会话（PiChatBridge::setWorkerDriver 测试缝）：相位机、控制时限、清理
// 与结算全部走真实桥路径，脚本只控制 peer 侧行为与消费节奏。
#include "workflow/pichatbridge.h"
#include "services/paleotaskservice.h"
#include "services/piconversation.h"
#include "services/piprotocol.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <functional>

namespace {
// 受控 Node：paleo.pi-conversation.v1 的 peer 侧。所有跨线程状态为原子量或
// start 前注入的只读回调；不做任何 UI/宿主对象访问。
class FakeNode {
public:
  enum class Script {
    HappyTools,        // 全链：2 调用工具轮 → 第二模型轮终答 → idle ok → closed
    IdentityViolation, // opened 握手违约 → 桥 fail → 停止收尾
    NoIdleStall,       // model_end 后不发 idle → 控制超时（有界）
    DuplicateSpam,     // 重复在途/已完成 tool_request 风暴 → 不刷新控制阶段
    Backpressure,      // 暂停消费 outgoing → 16 帧环满 → 恢复后续传
    WrongRound,        // tool_request 错 round → pi_tool_state
    ReapFailure,       // 停止后 worker 报回收失败（收尾补充信号）
  };
  Script script = Script::HappyTools;

  std::atomic<int> toolResultsReceived{0};   // peer 收到的 tool_result 帧数（含重发）
  std::atomic<int> modelCallsReceived{0};    // model_call 帧数
  std::atomic<int> modelDeltasReceived{0};   // model_delta 帧数（含分片）
  std::atomic<int> idleFramesSent{0};
  std::atomic<bool> proceedAfterHold{false}; // Backpressure：恢复消费
  // 工具轮第一项结果后、第二项 tool_request 前的宿主侧门（磁盘断言用）。
  std::function<bool()> beforeSecondToolRequest;

  QString run(PaleoTask *task, const std::shared_ptr<paleo::pi::ConversationChannel> &channel,
              const std::function<void()> &notify) {
    QElapsedTimer clock, stopClock, spamClock, holdClock;
    clock.start();
    bool stopping = false, idleSent = false, closedSeen = false, closeSeen = false;
    bool holding = false, heldOnce = false;
    int round = 0, callsDone = 0;
    QStringList pendingCalls;
    QVector<QString> roundOrder; // 帧序断言：deltas < calls < end
    QByteArray buffer;
    quint64 peerSequence = 0;
    const auto peer = [&](const QString &type, const QJsonObject &payload) {
      bool wake = false;
      const auto &identity = channel->identity();
      const QJsonObject envelope{{"protocol", QLatin1String(paleo::pi::ConversationProtocol)}, {"type", type},
        {"sessionId", identity.sessionId}, {"generation", identity.generation}, {"turnId", identity.turnId},
        {"nonce", identity.nonce}, {"sequence", QString::number(++peerSequence)}, {"payload", payload}};
      if (!channel->acceptPeer(envelope, wake))
        return false;
      if (wake && notify) notify();
      return true;
    };
    while (!closedSeen) {
      if (clock.elapsed() > 120000) return QStringLiteral("test_driver_timeout");
      const auto channelError = channel->error();
      if (!channelError.isEmpty() && channelError != QLatin1String("pi_conversation_busy"))
        return channelError;
      if (!stopping && (task->cancelRequested() || channel->stopReason())) {
        stopping = true; stopClock.start(); spamClock.start();
      }
      if (stopping) {
        // Node 侧优雅停机：idle(原因) → 等 close → closed（close 由下方帧循环
        // 应答；3s 未等到则按停止码退出，同产线有界收尾口径）。
        if (!idleSent) {
          const auto code = channel->stopReason() == 1 ? QStringLiteral("cancelled") : QStringLiteral("interrupted");
          if (!peer("idle", {{"code", code}})) return channel->error();
          ++idleFramesSent; idleSent = true;
        }
        if (stopClock.elapsed() > 3000) break;
      }
      // Backpressure：握手后暂停消费 outgoing，等宿主放行（一次性）。
      if (script == Script::Backpressure && holding && !heldOnce && proceedAfterHold.load()) {
        holding = false; heldOnce = true; holdClock.start();
      }
      if (!holding) {
        const auto outgoing = channel->takeOutgoing();
        if (!outgoing.isEmpty()) buffer += outgoing;
        QJsonObject frame; bool complete = false; QString error;
        while (paleo::pi::takeFrame(buffer, frame, complete, error)) {
          if (!complete) break;
          const auto type = frame.value("type").toString();
          const auto payload = frame.value("payload").toObject();
          if (type == QLatin1String("open")) {
            if (script == Script::IdentityViolation) {
              if (!peer("opened", {{"upstreamCommit", "u"}, {"coreVersion", "c"}, {"nodeVersion", "n"},
                                   {"toolExecution", "parallel"}, {"modelTransport", "host-only"}, {"sandboxed", false}}))
                return channel->error();
            } else if (!peer("opened", {{"upstreamCommit", "u"}, {"coreVersion", "c"}, {"nodeVersion", "n"},
                                        {"toolExecution", "sequential"}, {"modelTransport", "host-only"}, {"sandboxed", false}})) {
              return channel->error();
            }
          } else if (type == QLatin1String("tool_definition")) {
            if (!peer("configured", {{"name", payload.value("name").toString()}})) return channel->error();
          } else if (type == QLatin1String("start")) {
            round = 1;
            if (!peer("model_request", {{"round", QStringLiteral("1")}})) return channel->error();
            if (script == Script::Backpressure) { holding = true; holdClock.start(); }
          } else if (type == QLatin1String("model_delta")) {
            ++modelDeltasReceived; roundOrder.append(QStringLiteral("delta:%1").arg(payload.value("round").toString()));
          } else if (type == QLatin1String("model_call")) {
            ++modelCallsReceived; pendingCalls.append(payload.value("callId").toString());
            roundOrder.append(QStringLiteral("call:%1").arg(payload.value("round").toString()));
          } else if (type == QLatin1String("model_end")) {
            roundOrder.append(QStringLiteral("end:%1").arg(payload.value("round").toString()));
            callsDone = 0;
            if (pendingCalls.isEmpty()) {
              if (script != Script::NoIdleStall && !peer("idle", {{"code", QStringLiteral("ok")}}))
                return channel->error();
            } else {
              if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.first()}}))
                return channel->error();
              if (script == Script::WrongRound &&
                  !peer("tool_request", {{"round", QStringLiteral("9")}, {"callId", pendingCalls.first()}}))
                return channel->error();
            }
          } else if (type == QLatin1String("tool_result")) {
            ++toolResultsReceived; ++callsDone;
            if (!stopping && callsDone < pendingCalls.size()) {
              if (script == Script::DuplicateSpam) {
                // 停在这里：持续重发已完成的第一项请求（合法重发），等待控制
                // 超时证明重复帧不刷新控制阶段。
                spamClock.restart();
                while (!task->cancelRequested() && !channel->stopReason() && clock.elapsed() < 110000) {
                  if (spamClock.elapsed() >= 100) {
                    spamClock.restart();
                    if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.first()}}))
                      return channel->error();
                    const auto pending = channel->takeOutgoing();
                    if (!pending.isEmpty()) ++toolResultsReceived; // 已完成结果的合法重发：消费计数
                  }
                  QThread::msleep(10);
                }
                return channel->stopReason() == 1 ? QStringLiteral("cancelled") : QStringLiteral("interrupted");
              }
              if (beforeSecondToolRequest && !beforeSecondToolRequest())
                return QStringLiteral("test_gate_timeout");
              if (!peer("tool_request", {{"round", QString::number(round)}, {"callId", pendingCalls.at(callsDone)}}))
                return channel->error();
            } else if (!stopping) {
              pendingCalls.clear();
              ++round;
              if (!peer("model_request", {{"round", QString::number(round)}})) return channel->error();
            }
          } else if (type == QLatin1String("close")) {
            closeSeen = true;
            if (!peer("closed", {})) return channel->error();
            closedSeen = true; break;
          }
        }
      } else if (!heldOnce) {
        // 保持满环压力：既不消费也不应答（等待宿主放行）。
      }
      QThread::msleep(2);
    }
    if (script == Script::ReapFailure) return QStringLiteral("pi_process_reap_failed");
    return {};
  }
};

ChatToolCall call(const QString &id) {
  return {id, QStringLiteral("paleo.query_project"), QStringLiteral("{\"topic\":\"wells\"}")};
}
} // namespace

class TestG2PiTerminal : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { qRegisterMetaType<ChatToolCall>("ChatToolCall"); }
  void queuedWorkerGetsTheFullOpeningDeadline() {
    QSemaphore occupied, release;
    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(2);
    FakeNode node;
    PiChatBridge bridge;
    auto cleanup = qScopeGuard([&] { release.release(2); tasks.shutdown(10000, true); });
    for (int i = 0; i < 2; ++i) {
      tasks.start("occupied", [&](PaleoTask *) {
        occupied.release();
        release.acquire();
        return QString();
      });
    }
    QVERIFY(occupied.tryAcquire(2, 5000));
    bridge.setWorkerDriver([&](PaleoTask *task, const auto &channel, const auto &notify) {
      return node.run(task, channel, notify);
    });
    connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      QVERIFY(bridge.modelFinished({}, QStringLiteral("stop")));
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "queued", "first", {}, [] { return true; }), QString());
    QTest::qWait(15500); // Longer than the opening handshake is allowed to take.
    QVERIFY(failed.isEmpty());
    QVERIFY(finished.isEmpty());
    release.release(2);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
    QCOMPARE(finished.first().first().toString(), QStringLiteral("ok"));
  }
  // 正常闭环：两调用工具轮 → 第二模型轮终答 → closed → finished(ok) 各一次。
  void happyToolsRoundSettlesOk() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed),
        cleanups(&bridge, &PiChatBridge::cleanupNotice), modelRequested(&bridge, &PiChatBridge::modelRequested),
        toolRequested(&bridge, &PiChatBridge::toolRequested);
    int round = 0;
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      if (++round == 1) {
        QVERIFY(bridge.modelDelta(QStringLiteral("partial")));
        QVERIFY(bridge.modelFinished(QVector<ChatToolCall>{call("host-a"), call("host-b")}, QStringLiteral("tool_calls")));
      } else {
        QVERIFY(bridge.modelDelta(QStringLiteral("final answer")));
        QVERIFY(bridge.modelFinished({}, QStringLiteral("stop")));
      }
    });
    QObject::connect(&bridge, &PiChatBridge::toolRequested, &bridge, [&](const ChatToolCall &request) {
      QVERIFY(bridge.toolFinished(request, true, QStringLiteral("{\"topic\":\"wells\"}")));
    });
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QVERIFY(bridge.busy());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
    QCOMPARE(finished.first().first().toString(), QString("ok"));
    QVERIFY(failed.isEmpty()); QVERIFY(cleanups.isEmpty());
    QCOMPARE(modelRequested.size(), 2); QCOMPARE(toolRequested.size(), 2);
    QCOMPARE(node.toolResultsReceived.load(), 2); QCOMPARE(node.modelCallsReceived.load(), 2);
    QVERIFY(!bridge.busy());
    tasks.shutdown(10000, true);
  }
  // W5：握手违约 → failed 即刻一次（主因），finished 收尾完成且不降级。
  void protocolFailureKeepsRootCauseThroughCleanup() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::IdentityViolation;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
    QCOMPARE(failed.first().first().toString(), QString("pi_conversation_identity"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QCOMPARE(finished.first().first().toString(), QString("pi_conversation_identity")); // 不是 interrupted
    QVERIFY(!bridge.busy());
    tasks.shutdown(10000, true);
  }
  // W5：用户 cancel 后 worker 报进程回收失败——finished 主因仍是 cancelled，
  // 回收失败以收尾补充各一次呈现，不互相冒充。
  void userCancelKeepsCauseAndReportsReapFailureAsCleanupNotice() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::ReapFailure;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed),
        cleanups(&bridge, &PiChatBridge::cleanupNotice);
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      bridge.abort(true); // 用户主动取消：停止请求 ≠ 已回收
      bridge.modelFailed(QStringLiteral("llm_error_after_stop"));
    });
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QCOMPARE(finished.first().first().toString(), QString("cancelled"));
    QCOMPARE(cleanups.size(), 1); QCOMPARE(cleanups.first().first().toString(), QString("pi_process_reap_failed"));
    QVERIFY(failed.isEmpty()); QVERIFY(!bridge.busy());
    tasks.shutdown(10000, true);
  }
  // W5：任务/服务销毁（控制心跳兜底）→ 终态与根因一致。
  void destroyedTaskServiceSettlesRootCause() {
    auto tasks = std::make_unique<PaleoTaskService>(); tasks->setMaxWorkerThreads(2);
    auto running = std::make_shared<std::atomic_int>(0);
    for (int i = 0; i < 2; ++i) tasks->start("occupy", [running](PaleoTask *task) {
      ++*running; while (!task->cancelRequested()) QThread::msleep(1); return QString();
    });
    QTRY_COMPARE_WITH_TIMEOUT(running->load(), 2, 10000);
    FakeNode node; PiChatBridge bridge;
    bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QCOMPARE(bridge.start({true, {}, {}}, tasks.get(), "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QVERIFY(bridge.busy());
    tasks.reset();
    QTRY_VERIFY_WITH_TIMEOUT(!bridge.busy(), 10000);
    QCOMPARE(finished.size(), 1); QCOMPARE(failed.size(), 1);
    QCOMPARE(finished.first().first().toString(), QString("pi_task_unavailable"));
    QCOMPARE(failed.first().first().toString(), QString("pi_task_unavailable"));
  }
  // W6：满 16 帧环是可恢复背压——不失败、不丢帧，恢复后按序续传。
  void fullOutgoingRingStallsThenFlushesInOrder() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::Backpressure;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    int round = 0;
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      if (++round != 1) { // 第二轮终答（调用 id 逐轮新鲜——宿主身份含轮次）
        QVERIFY(bridge.modelDelta(QStringLiteral("final")));
        QVERIFY(bridge.modelFinished({}, QStringLiteral("stop")));
        return;
      }
      // 65536 字符 pending 预算内 → 16 个 4096 分片 + 2 call + end 超出 16 帧环。
      QVERIFY(bridge.modelDelta(QString(16 * 4096, QLatin1Char('x'))));
      QVERIFY(bridge.modelFinished(QVector<ChatToolCall>{call("host-a"), call("host-b")}, QStringLiteral("tool_calls")));
    });
    QObject::connect(&bridge, &PiChatBridge::toolRequested, &bridge, [&](const ChatToolCall &request) {
      QVERIFY(bridge.toolFinished(request, true, QStringLiteral("{\"topic\":\"wells\"}")));
    });
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTest::qWait(300); // 环满窗口：桥必须保持 busy、不失败、不丢内容
    QVERIFY(bridge.busy()); QVERIFY2(failed.isEmpty(), "backpressure must be retryable, not fatal");
    node.proceedAfterHold = true; // 放行：flush 恢复
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 30000);
    QCOMPARE(finished.first().first().toString(), QString("ok"));
    QCOMPARE(node.modelDeltasReceived.load(), 17); // 第一轮 16 分片一片不丢 + 第二轮终答 1 片
    QCOMPARE(node.modelCallsReceived.load(), 2);
    QCOMPARE(node.toolResultsReceived.load(), 2);
    tasks.shutdown(10000, true);
  }
  // W6：重复在途/已完成 tool_request 只等待或重发结果——控制阶段不被无进展
  // 帧刷新，停滞在既有 15s 控制时限内可判失败。
  void duplicateFramesNeverRefreshTheControlDeadline() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::DuplicateSpam;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed),
        toolRequested(&bridge, &PiChatBridge::toolRequested);
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      QVERIFY(bridge.modelFinished(QVector<ChatToolCall>{call("host-a"), call("host-b")}, QStringLiteral("tool_calls")));
    });
    QObject::connect(&bridge, &PiChatBridge::toolRequested, &bridge, [&](const ChatToolCall &request) {
      QVERIFY(bridge.toolFinished(request, true, QStringLiteral("{\"topic\":\"wells\"}")));
    });
    QElapsedTimer wall; wall.start();
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 25000); // 15s 控制时限 + 余量
    QCOMPARE(finished.first().first().toString(), QString("pi_control_timeout"));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(toolRequested.size(), 1);      // 重复请求从未二次派发
    QVERIFY(node.toolResultsReceived.load() >= 2); // 已完成结果的合法重发发生了
    QVERIFY(wall.elapsed() < 24000);        // 有界：不是外层测试超时兜的
    QVERIFY(!bridge.busy());
    tasks.shutdown(10000, true);
  }
  // W6：model_end 后缺 idle/closed → 控制超时，不产生虚假后续结果。
  void missingIdleIsABoundedControlTimeout() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::NoIdleStall;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed),
        toolRequested(&bridge, &PiChatBridge::toolRequested);
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      QVERIFY(bridge.modelDelta(QStringLiteral("answer")));
      QVERIFY(bridge.modelFinished({}, QStringLiteral("stop")));
    });
    QElapsedTimer wall; wall.start();
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 25000);
    QCOMPARE(finished.first().first().toString(), QString("pi_control_timeout"));
    QVERIFY(toolRequested.isEmpty()); // 没有工具被凭空派发
    QVERIFY(wall.elapsed() < 24000);
    tasks.shutdown(10000, true);
  }
  // W6：错 round 的 tool_request → pi_tool_state，立即失败收尾。
  void staleRoundToolRequestFailsDeterministically() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    node.script = FakeNode::Script::WrongRound;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      QVERIFY(bridge.modelFinished(QVector<ChatToolCall>{call("host-a")}, QStringLiteral("tool_calls")));
    });
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
    QCOMPARE(finished.first().first().toString(), QString("pi_tool_state"));
    QCOMPARE(failed.size(), 1);
    tasks.shutdown(10000, true);
  }
  // W6：相位不符时 modelFinished 拒绝（返回 false）且不发失败信号——判定性
  // 拒绝交由 controller 转为本轮失败（见 tst_g2_turncheckpoint）。
  void modelFinishedOutsideModelPhaseIsRejectedWithoutSignals() {
    PaleoTaskService tasks; tasks.setMaxWorkerThreads(3); FakeNode node;
    PiChatBridge bridge; bridge.setWorkerDriver([&node](PaleoTask *task,
        const std::shared_ptr<paleo::pi::ConversationChannel> &channel, const std::function<void()> &notify) {
      return node.run(task, channel, notify);
    });
    QSignalSpy finished(&bridge, &PiChatBridge::finished), failed(&bridge, &PiChatBridge::failed);
    QObject::connect(&bridge, &PiChatBridge::modelRequested, &bridge, [&] {
      QVERIFY(bridge.modelFinished({}, QStringLiteral("stop"))); // Model 相位内：接受
    });
    QCOMPARE(bridge.start({true, {}, {}}, &tasks, "g2-session", "g2-turn", {}, [] { return true; }), QString());
    QTRY_VERIFY_WITH_TIMEOUT(!bridge.busy(), 15000); // 正常收尾
    QCOMPARE(finished.first().first().toString(), QString("ok"));
    // 已关闭：任何迟到完成都被拒绝，且不再产生信号（单次结算）。
    QVERIFY(!bridge.modelFinished({}, QStringLiteral("stop")));
    QVERIFY(!bridge.modelDelta(QStringLiteral("late")));
    QVERIFY(!bridge.toolFinished(call("host-a"), true, QStringLiteral("{}")));
    QCOMPARE(finished.size(), 1); QVERIFY(failed.isEmpty());
    tasks.shutdown(10000, true);
  }
};

QTEST_GUILESS_MAIN(TestG2PiTerminal)
#include "tst_g2_piterminal.moc"
