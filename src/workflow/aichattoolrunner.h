// 层：功能
#pragma once
#include "../ai/chat/chatmessage.h"
#include "../ai/horizonsuggest.h"
#include "../ai/wellfaciesservice.h"
#include "../services/paleotaskservice.h"
#include "../services/wellfaciesinputplan.h"
#include "aiassistworkflow.h"
#include "chatresultstore.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
namespace paleo::query {class ProjectQueryCapability;}
namespace paleo::python {class RuntimeProvider;class TrustedReadinessProvider;}
namespace paleo::document {struct SourceRef;struct SourceRecord;}

// workflow/ — 聊天领域工具执行器（方向61）。
//
// 把模型点名的 tool_calls（ChatToolCall）映射到既有执行面并异步执行：
//   · paleo.tile_classification_v2     → TileOperationService 三阶段操作
//   · paleo.tile_classification        → 显式注入的内部 legacy adapter
//   · paleo.horizon_tracking_suggestion→ AiAssistWorkflow::startSuggestion
//   · paleo.well_facies_prediction     → WellFaciesAgentRun（typed v2 引用）
//   · paleo.query_project              → runQuery（catalog 只读查询，方向77）
//   · paleo.asset_lineage              → runLineage（血缘只读服务，方向77）
//   · paleo.operation_*               → CapabilityRecovery 持久查询/恢复
//   · paleo.read_result               → 会话级不可变报告读取
//
// 分层站位：本类只编排（任务/HTTP + 世代号守卫），不画像素、不弹对话框。
// 数据上下文（层位名 / 网格取数 / 道窗取数 / 井曲线 / 工程目录）由装配根或
// 测试注入——未绑定时**如实报错**（错误结果回灌模型，不冒充成功）。
//
// 只读红线（方向77）：runQuery/runLineage 对 catalog 只触 const 查询面，
// 零 mutator、零 DerivedAsset 登记通道——工具执行前后 catalog 快照不变
// （测试断言口径）。同步查询也经 finishActive 统一异步回报（#281 纪律）。
//
// 执行语义（钉死）：
//   · 串行：一次只跑一个工具（FIFO）。lastProductStats 是 workflow 的单例
//     摘要面，并发跑两个 tile 分类会串台——串行是正确性约束，不是优化。
//   · cancel() 作废当前聊天订阅并请求任务取消。持久协调器仍记录已知提交/
//     远端完成事实；已提交成果保留，未发送的后续段停止，不能承诺零变化。
//   · 每帧必有终态：run() 的每个调用都会以 toolFinished 收尾（错误也是
//     结果——协议要求每个 tool_call_id 有应答），除非被 cancel 作废。
class DataCatalog;
struct AiToolSpec;
class PaleoOnnxService;
namespace paleo::capability { class CapabilityHost; }
namespace paleo {class TileOperationService;}

class AiChatToolRunner : public QObject {
  Q_OBJECT
public:
  struct TrackingInput {
    TraceWindowFetcher fetch;
    QJsonObject identity;
    std::function<QString()> ownerValidation;
  };
  using TrackingInputProvider = std::function<bool(const std::function<bool()> &cancelled,
      TrackingInput *out, QString *error)>;
  struct FrozenFaciesInput {
    paleo::wellfacies::InputJob job;
    std::function<QString()> ownerValidation;
  };
  using FrozenFaciesProvider = std::function<bool(const paleo::wellfacies::Request &,
      const WellFaciesModel &, FrozenFaciesInput &, QString &)>;

  struct Context {
    QString horizon;      // tile 产品归属层位（装配根按活动目标层位注入）
    AiAssistWorkflow::GridFetch gridFetch;   // tile 读域取数（含 halo）
    TraceWindowFetcher traceFetch;           // 层位建议道窗取数
    TrackingInputProvider trackingInput;     // Captured once per operation by the trusted owner.
    FrozenFaciesProvider frozenFaciesInput;
    std::function<std::optional<paleo::document::InterpretationContext>()> faciesContext;
    // 方向77：只读工程上下文（query/lineage 工具）——装配根在工程打开时
    // 注入；为空时两工具按调用如实报「须打开工程」。指针稳定性由装配根
    // 保证（importSvc 持有、open 原地重绑，不换实例）。
    DataCatalog *catalog = nullptr;
    QString documentId; // trusted host DocumentSession binding, required by typed queries
    // Pure data lookup owned by the same document/epoch; never accepts tool paths.
    std::function<paleo::document::SourceRecord(const paleo::document::SourceRef &)> sourceResolver;
    QPointer<paleo::capability::CapabilityHost> capabilityHost;
    bool requireCapabilityHost = false;
    QPointer<PaleoOnnxService> onnx;
    QPointer<paleo::TileOperationService> tileOperations;
    std::function<QString(const QString &, const QJsonObject &, bool)> runtimeReadiness;
    std::shared_ptr<paleo::python::RuntimeProvider> pythonRuntime;
    std::shared_ptr<paleo::python::TrustedReadinessProvider> pythonTrusted;
    quint64 documentEpoch = 0;
    QString projectDir;   // 工程根（出参 project_dir 引用基准）
  };

  explicit AiChatToolRunner(QObject *parent = nullptr);
  ~AiChatToolRunner() override;

  void setWorkflow(AiAssistWorkflow *workflow);
  // 测井相服务为本类私有实例；配置显式注入（app: WellFaciesConfig::load()，
  // 测试：loopback 假端点）——不隐藏读盘。
  void setFaciesConfig(const WellFaciesConfig &config);
  bool externalTransferConfigured() const;
  bool pythonExecutionAllowed() const;
  std::shared_ptr<paleo::python::RuntimeProvider> pythonRuntime() const { return m_context.pythonRuntime; }
  std::shared_ptr<paleo::python::TrustedReadinessProvider> pythonTrusted() const { return m_context.pythonTrusted; }
  void setContext(const Context &context);
  void setTaskService(PaleoTaskService *service);
  void setResultSession(const QString &session);
  QJsonObject modelResult(const ChatToolCall &call, const QString &rawJson,
                         bool ok, int maxBytes);
  PaleoTaskService *taskService() const { return m_taskService; }

  bool busy() const { return m_active || !m_queue.isEmpty(); }
  struct ToolReadiness { bool ready=false; QString code; unsigned effects=0; };
  ToolReadiness readiness(const QString &name,const QJsonObject &arguments={},bool forListing=false) const;
  QVector<AiToolSpec> availableTools() const;
  QString wellFaciesReadiness(const QJsonObject &args={},bool forListing=false) const;

  // 方向77：工程摘要段（井数/层位数/资产计数一行式，≤300 字符）——
  // 装配根绑定/重绑时算好交给 AiChatController::setProjectBrief 作 system
  // prompt 常驻轻注入。无工程时返回「未打开」的如实一句话。
  static QString projectBrief(const DataCatalog *catalog,
                              const QString &projectDir);

  // 入队执行（异步）。同帧重复 id 由调用方（controller）保证不出现。
  void run(const ChatToolCall &call);
  // 作废在途与排队（见类注释「取消 = 作废」）。
  void cancel();
  void interrupt(); // Host/session invalidation preserves resumable operation intent.

signals:
  void toolStarted(const ChatToolCall &call);
  // ok=true：resultJson 为结果摘要（JSON object）；ok=false：resultJson 为
  // {"error": ...}（诚实错误，回灌模型由其决定如何向用户解释）。
  // 一律异步 emit（#281）：绝不从 run() 的调用栈同步回报——同步失败分支
  // （实参非法/未路由/上下文未绑定）与异步执行同一语义，都经事件循环派发。
  void toolFinished(const ChatToolCall &call, bool ok,
                    const QString &resultJson);

private:
  void stopCurrentRound(bool explicitRequest);
  void startNext();
  void finishActive(bool ok, const QJsonObject &payload);
  void runCapability(const QString &title, const QJsonObject &args);
  void runPersistentOperation(const QJsonObject &args);
  void runTileV2(const QJsonObject &args);
  void runTile(const ChatToolCall &call, const QJsonObject &args);
  void runHorizon(const ChatToolCall &call, const QJsonObject &args);
  void runFacies(const QJsonObject &args);
  void runFaciesModels(const QJsonObject &args);
  void runFaciesInputs(const QJsonObject &args);
  void runLocalModels(const QJsonObject &args);
  void runPythonEnvironment();
  // 方向77：同步只读查询（内存表直读，无任务池）；仍经 finishActive 异步
  // 回报（#281 统一语义）。
  void runQuery(const ChatToolCall &call, const QJsonObject &args);
  void runLineage(const ChatToolCall &call, const QJsonObject &args);
  QJsonObject errorPayload(const QString &message) const;
  void invalidateContext();
  void rebindQueryScope();
  quint64 taskSession() const;
  bool activeContextIsCurrent() const;
  void refreshResultScope();
  ToolReadiness readinessForSpec(const AiToolSpec &spec, const QJsonObject &arguments, bool forListing) const;

  struct QueuedCall {
    ChatToolCall call;
    quint64 contextGeneration;
    quint64 taskSession;
    quint64 faciesEpoch;
  };

  AiAssistWorkflow *m_workflow = nullptr;
  Context m_context;
  QPointer<PaleoTaskService> m_taskService;
  WellFaciesService *m_facies = nullptr; // 私有（本类拥有）
  std::unique_ptr<paleo::query::ProjectQueryCapability> m_query;
  QString m_querySessionId;
  WellFaciesConfig m_faciesConfig;       // 注入快照（预检端点/密钥齐不齐）
  QVector<QueuedCall> m_queue;
  ChatToolCall m_activeCall;
  QPointer<PaleoTask> m_activeTask;
  bool m_active = false;
  bool m_activeIsFacies = false;
  quint64 m_faciesEpoch=0,m_activeFaciesEpoch=0;
  int m_generation = 0; // cancel()/析构 ++：作废迟到回调
  int m_explicitCancelGeneration = -1;
  quint64 m_contextGeneration = 0;
  quint64 m_activeContextGeneration = 0;
  quint64 m_activeTaskSession = 0;
  paleo::chat::ChatResultStore m_results;
  QString m_resultSession;
  quint64 m_resultEpoch = 0;
  quint64 m_resultTaskSession = 0;
};

// 装配/测试侧的简称（Context 语义上独立于 runner 实例存在）。
using AiChatToolContext = AiChatToolRunner::Context;
