// 层：功能
#pragma once
// workflow/aiplanpresentation.h — H5 计划提案的展示/执行适配器与共享纯值函数。
//
// 职责边界（与 G5 AiOperationPresentation 同族纪律）：
//   · 共享纯函数：严格解码（不可信模型 JSON → 有界 PlanNode/PlanEdge）、
//     validatePlan 结果 → paleo.plan-preview.v1 有界提案契约、规范化图的
//     稳定 digest、由 (documentId, graphDigest) 确定性推导的执行请求身份。
//     全部无 I/O、无副作用；aichattoolrunner_plan.cpp 的 runPlanPreview 与
//     本适配器的「编辑后重新预检」共用同一实现，不在 UI 手抄第二份校验。
//   · 适配器：管理提案草案（原始/当前修订、绑定上下文、预检结果），把用户
//     的显式意图（重新预检/计算私有成果/查看原操作）翻译成既有宿主链：
//     CapabilityHost::preparePlan → start → CapabilityRecovery。模型文本、
//     工具实参、历史 JSON 都不能自授 actor/approved/路径/发布权限；执行
//     效果由宿主 effects 门最终裁决，本类不另造 allow-list、不提升 Caller。
//   · 生命周期：动作受理时捕获当前 chat session 与宿主上下文，实际调用前
//     在 owner 线程复核依赖存活、会话未换、checkContext(live,captured,Read)；
//     异步结果按（世代 + 草案 + 请求身份）接收，过期回调丢弃——晚结果只
//     更新其对应提案，不跨绑到新草案。
// 无 QtWidgets；不读私有目录、不把 resultRef 解析为路径。
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "capabilityplan.h"
#include "services/hostcallcontext.h"

class AiChatController;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; }

namespace paleo::chat {

// ---- 共享纯值函数（runner 工具入口与审阅面板同一实现）----

// 严格解码结果：code 为机器错误码，field 为首个违规字段的 JSON 路径
// （如 "nodes.2.parameters.CELL_SIZE"）；ok=false 时 nodes/edges 为空。
struct PlanPreviewDecode {
  bool ok = false;
  QString code, field;
  QVector<paleo::capability::PlanNode> nodes;
  QVector<paleo::capability::PlanEdge> edges;
  QString title, note;
};
// 工具实参形态：{title?, note?, nodes[], edges[]}。封闭词表、深度/元素/
// 字符串预算先于容器扩张检查；approved/caller/documentId/路径等一切未
// 声明字段在解码层即被拒（不进入执行授权）。
PlanPreviewDecode decodePlanPreviewArguments(const QJsonObject &arguments);
// 面板编辑形态：{nodes[], edges[]}（同一严格节点/边解码器）。
PlanPreviewDecode decodePlanPreviewGraph(const QJsonObject &graph);

// validatePlan → 有界提案契约（正常/失败同一形状；预检失败时 graph 回显
// 解码归一值、issues 只含验证器真实返回的问题——不伪造完整问题集合）。
QJsonObject planPreviewPayload(const paleo::capability::ValidatedPlan &plan,
                               const QVector<paleo::capability::PlanNode> &nodes,
                               const QVector<paleo::capability::PlanEdge> &edges,
                               const QString &title, const QString &note);

// 规范化图 JSON 的稳定 digest（节点按 id、边按四元组排序；Qt JSON 对象
// 键序即字典序）。图变更 → 新 digest；与提交顺序无关。
QString planGraphDigest(const QJsonObject &normalizedGraph);

// 执行请求身份：同一 (document, graph) 的重复显式计算复用同一请求 id
// （宿主幂等重放，不重算）；改图/换工程自然产生新身份，原请求保留。
QString planExecutionRequestId(const QString &documentId, const QString &graphDigest);

// ---- 适配器 ----

class AiPlanPresentation : public QObject {
  Q_OBJECT
public:
  struct Deps {
    QPointer<AiChatController> chat;
    QPointer<paleo::capability::CapabilityHost> host;
    QPointer<paleo::document::DocumentSession> document; // 通知刷新；动作门不依赖它
  };

  // 草案只读视图（纯展示值；不构成任何权限）。
  struct DraftView {
    QString draftId;
    QString sourceToolCallId;
    int revision = 0;
    bool preflightValid = false;     // 最近一次预检的图判定
    bool preflightDone = false;
    QJsonObject lastPayload;         // paleo.plan-preview.v1（最近预检/提案）
    QJsonObject originGraph;         // 原始提案规范化图（对照显示）
    QJsonObject currentGraph;        // 当前修订规范化图
    QString title, note;
    bool executionEligible = false;  // 预检通过且绑定仍当前
    QString ineligibleReason;
    QString clientRequestId;         // 由 (document, graphDigest) 推导
    QString operationId;             // 已知原操作（计算/查询后）
    QString execution, commitState, presentation, requestHash;
    QString acceptedNote;            // 受理状态注记（如「待确认」）
  };

  explicit AiPlanPresentation(const Deps &deps, QObject *parent = nullptr);

  // 打开/重开提案（payload = paleo.plan-preview.v1 结果；同 toolCallId 重开
  // 复用草案，只刷新提案来源）。返回草案 id；空 = 拒绝（reason 给码）。
  QString openProposal(const QJsonObject &payload, const QString &sourceToolCallId,
                       QString *reason = nullptr);
  DraftView draft(const QString &draftId) const;
  QStringList draftIds() const;

  // 用户编辑：提交新的规范化图 JSON（同一严格解码器）；有效编辑产生新的
  // draft revision，原提案保留；编辑使先前执行资格失效（需重新预检）。
  bool updateDraftGraph(const QString &draftId, const QJsonObject &graph,
                        QString *reason = nullptr);

  // 显式意图（返回 false = 前置拒绝，reason 给码，未触服务/宿主）。
  bool requestPreflight(const QString &draftId, QString *reason = nullptr);
  bool requestCompute(const QString &draftId, QString *reason = nullptr);
  bool requestOriginalOperation(const QString &draftId, QString *reason = nullptr);

  // 计算/预检/查询的同一当前状态判定（按钮 enabled 与 handler 共用）。
  bool computeAllowed(const QString &draftId, QString *reason = nullptr) const;
  bool busy(const QString &draftId, const QString &action) const;

  // 宿主是否就绪（面板兜底提示用；不构成权限）。
  bool hostReady(QString *code = nullptr) const;

  // 测试/遥测内省：preparePlan / start 的真实调用计数（验收 oracle 用，
  // 例如「纯预检与历史渲染路径计数为零」）。
  unsigned prepareAttempts() const { return m_prepareAttempts; }
  unsigned startAttempts() const { return m_startAttempts; }

signals:
  void contextChanged();
  void draftChanged(const QString &draftId);
  void preflightFinished(const QString &draftId, const QJsonObject &payload);
  // action ∈ {compute, original}; facts 为服务事实（含错误码）。
  void computeFinished(const QString &draftId, const QJsonObject &facts);
  void originalOperationResolved(const QString &draftId, const QJsonObject &record);
  void busyChanged(const QString &draftId, const QString &action, bool busy);

private:
  struct Guard {
    QString chatSessionId;
    paleo::host::CallContext capturedContext;
    quint64 generation = 0;
  };
  struct Draft {
    QString id, sourceToolCallId;
    int revision = 0;
    QJsonObject originGraph, originPayload, currentGraph, lastPayload;
    QString title, note;
    QString chatSessionId;                 // 最近一次预检绑定的会话
    paleo::host::CallContext boundContext; // 最近一次预检绑定的宿主上下文
    bool preflightValid = false, preflightDone = false;
    QString graphDigest;
    bool executionEligible = false;
    QString ineligibleReason;
    QString operationId, execution, commitState, presentation, requestHash, acceptedNote;
    quint64 execGeneration = 0;            // 在途执行匹配（晚结果归属）
  };
  QString guardError(const Guard &guard) const;
  bool beginGuarded(const QString &draftId, const QString &action, Guard &guard,
                    QString *reason);
  void releaseBusy(const QString &draftId, const QString &action);
  void invalidateEligibility(const QString &reason);
  Draft *findDraft(const QString &draftId);
  const Draft *findDraft(const QString &draftId) const;

  Deps m_deps;
  quint64 m_generation = 0;
  quint64 m_draftSequence = 0;
  QHash<QString, Draft> m_drafts;
  QStringList m_draftOrder;
  QSet<QString> m_busy;
  unsigned m_prepareAttempts = 0;
  unsigned m_startAttempts = 0;
};

} // namespace paleo::chat
