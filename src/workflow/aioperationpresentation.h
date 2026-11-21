// 层：功能
#pragma once
// workflow/aioperationpresentation.h — G5 原操作显式查询/取消/恢复的展示适配器。
//
// 职责边界：把视图的「意图」（刷新/事件/结果/读页/取消/恢复）翻译成对现有
// CapabilityRecovery 服务的调用，并执行捕获-校验-代次纪律：
//   · 每次动作受理时从**当前**宿主捕获 context、非空 currentChatSessionId
//     与展示绑定代次；实际调用前在 owner 线程重新确认依赖存活、会话与
//     绑定未变化，重读 live context，经 host::checkContext(live, captured,
//     Read) 外层门，再取 callerScope(Caller::Agent, currentChatSessionId)。
//   · 本适配器不实现第二套调度/恢复事务：operationId 只是候选引用，效果、
//     写权限与原计划由既有服务最终裁决；不使用 Caller::Ui，不重用历史
//     scope/epoch 作为 permit。
//   · 异步结果按请求代次接收：过期回调（依赖销毁/换代/被更新请求顶替）丢弃。
// 无 QtWidgets；不读私有目录、不解析 resultRef 为路径。
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QSet>

#include "services/hostcallcontext.h"

class AiChatController;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; }

namespace paleo::chat {

class AiOperationPresentation : public QObject {
  Q_OBJECT
public:
  struct Deps {
    QPointer<AiChatController> chat;
    QPointer<paleo::capability::CapabilityHost> host;
    QPointer<paleo::document::DocumentSession> document; // 通知刷新；动作门不依赖它
  };

  // W1 上下文快照（纯展示值，现读不缓存；不构成任何权限）。
  struct HostContextView {
    bool available = false;
    QString unavailableCode;
    QString documentId, horizon, unitId;
    quint64 epoch = 0;
    int selectionCount = 0;
    bool open = false, readOnly = true;
    QString chatSessionId;
    QString hostTurnId;       // 当前回合冻结归属（无回合为空）
    QJsonObject turnContext; // 宿主冻结身份快照（documentId/epoch/generation/taskSession）
  };

  explicit AiOperationPresentation(const Deps &deps, QObject *parent = nullptr);

  HostContextView currentContext() const;

  // 忙碌去重（按 operation+action；控制请求双击不重复发起）。
  bool busy(const QString &operationId, const QString &action) const;

  // ---- 显式意图。返回 false = 前置拒绝（reason 给出 code），未触服务 ----
  bool requestRefresh(const QString &operationId, QString *reason = nullptr);
  bool requestEvents(const QString &operationId, quint64 afterRevision,
                     QString *reason = nullptr);
  bool requestResults(const QString &operationId, QString *reason = nullptr);
  bool requestPortPage(const QString &operationId, const QString &portId,
                       qint64 offset, int maxBytes, QString *reason = nullptr);
  bool requestCancel(const QString &operationId, QString *reason = nullptr);
  bool requestResume(const QString &operationId, QString *reason = nullptr);

signals:
  // 宿主/会话重绑通知（展示刷新用；动作受理时仍现读 live 值）。
  void contextChanged();
  void operationStateChanged(const QString &operationId, const QJsonObject &state);
  void operationEventsArrived(const QString &operationId, const QJsonObject &events);
  void operationResultsArrived(const QString &operationId, const QJsonObject &results);
  void operationPortPage(const QString &operationId, const QString &portId,
                         const QJsonObject &page);
  // action ∈ {cancel, resume}；response 为服务事实（含错误码）。
  void operationActionFinished(const QString &operationId, const QString &action,
                               const QJsonObject &response);
  void busyChanged(const QString &operationId, const QString &action, bool busy);

private:
  struct Guard { // 捕获快照：动作期间重绑/销毁/换会话即失效。
    QString chatSessionId;
    paleo::host::CallContext capturedContext;
    bool valid() const { return !chatSessionId.isEmpty(); }
  };
  bool beginAction(const QString &operationId, const QString &action, Guard *guard,
                   QString *reason);
  void releaseBusy(const QString &operationId, const QString &action);
  bool checkGuard(const Guard &guard) const;
  QString scopeFor(const Guard &guard, QString *reason) const;

  Deps m_deps;
  quint64 m_bindingEpoch = 1;      // DocumentSession 重绑/换代递增
  quint64 m_requestSequence = 0;   // 异步请求代次（新请求顶替旧回调）
  QSet<QString> m_busy;            // "action:operationId"
};

} // namespace paleo::chat
