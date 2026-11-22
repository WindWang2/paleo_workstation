// 层：功能
#pragma once
// H6（模型接入诊断与兼容探测）——纯契约值类型：诊断候选、观测结果、报告。
//
// 站位：本文件只定义诊断面共用的值形状（不含网络、不含 QtWidgets、不含
// 任何执行面）。候选值在内存中可携带本次诊断凭据（绝不落盘/不进报告/不进
// 日志）；观测与报告全部不含秘密——文本出口由 llmdiagnosticreport 渲染，
// 凭据防回显经 llmcredentialguard 复用。
//
// 与既有边界的衔接（只读复用，不改 G1 状态机）：
//   · 传输面：POST 探测复用生产 LlmClient（冻结 attempt、双时限、8 MiB
//     上限、手动重定向）；目录 GET 在 llmmodelinventory 自建有界请求。
//   · 结论口径：公开信号只能证明「文本请求可用」「stream=true 请求被当前
//     客户端接受」；真实 SSE 是否发生不可由公开信号区分——报告恒标
//     未独立确认（不凭 delta 次数/Content-Type/完成速度宣布）。
//   · 工具帧：诊断专用无副作用 echo function（paleo.diagnostic_echo），不进
//     全局 registry、不经 AiChatToolRunner/CapabilityHost/Python/Pi。
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace paleo::chat {

// 一次诊断动作的候选参数（含本次凭据——只在内存，永不出本进程/本对象）。
// 来源语义（W1）：不得从当前 LlmConfig 值反推丢失的来源信息——
// 密钥来源只能标注「本次显式输入 / 沿用当前内存值 / 无」，不能自称
// 来自环境或钥匙串。
struct LlmDiagnosticCandidate {
  // 诊断凭据来源（诚实标注，不猜测存储出处）。
  enum class KeySource {
    None,            // 无可用凭据（探测类动作本地拒绝并说明原因）
    ExplicitInput,   // 本次在配置对话框密钥框显式输入（仅用于本次诊断）
    InMemoryCurrent, // 沿用当前生效配置的内存密钥（仅当端点身份完全一致）
  };

  QUrl endpoint;
  QString model;
  bool stream = true;
  int timeoutMs = 30000;       // idle（与生产 LlmConfig 同义）
  int totalTimeoutMs = 300000; // 绝对总时限
  int maxTokens = 1024;
  bool allowInsecureHttp = false;
  QByteArray key;              // 仅内存；空 = 无
  KeySource keySource = KeySource::None;
};

// 稳定机器分类（报告/测试按枚举断言，不匹配文案）。目录与探测分项独立，
// 未测试/无法确定/被拒绝/失败不得合并成一个「已连接」。
enum class LlmDiagnosticStatus {
  Untested,      // 本代次未测试
  Running,       // 在途
  Passed,        // 有正面证据（分项口径，见各观测的语义注释）
  Unsupported,   // 目录未实现（HTTP 404/405）——不是文本能力不可用
  UnknownShape,  // 2xx 但不是已声明的目录形状（含非 JSON）
  Empty,         // 目录成功但零条目
  Unauthorized,  // HTTP 401/403
  RateLimited,   // HTTP 429
  Rejected,      // 其它 4xx
  ServerError,   // HTTP 5xx
  Network,       // 连接层失败（含「不跟随重定向」）
  Timeout,       // 空闲/总时限
  TooLarge,      // 字节预算超限
  Malformed,     // 形状/协议坏（含错误信封）
  Cancelled,     // 调用方取消（单次终态）
  Indeterminate, // 无法判定（如 auto 工具模式模型直接作答未调工具）
  LocalRejected, // 本地拒绝（凭据/参数规则未过）——零网络
};

// 目录发现的观测结果（有界：条目数/长度/字节在 llmmodelinventory 钳制）。
struct LlmCatalogObservation {
  LlmDiagnosticStatus status = LlmDiagnosticStatus::Untested;
  QStringList models;       // 精确 model ID（按服务序去重；不 trim/不大小写折叠）
  bool truncated = false;   // 条目超上限被截断（partial/coverage 明示）
  int duplicatesFiltered = 0;
  int malformedEntries = 0; // 形状非法的条目数（整目缺失字段也算）
  bool credentialFiltered = false; // 有条目回显凭据被整体剔除（不展示）
  QString detail;           // 已脱敏的人类可读说明（可为空）
};

// 单次模型探测（文本/流式/工具往返）的观测结果。
struct LlmProbeObservation {
  // 探测种类（报告分项呈现）。
  enum class Kind { Text, Stream, ToolRoundTrip };
  Kind kind = Kind::Text;
  LlmDiagnosticStatus status = LlmDiagnosticStatus::Untested;
  // 有界观测值（usage 为服务端报告值；缺省/未知不得显示成精确零）。
  QString finishReason;     // 客户端终态透传（stop/length/…；未知为空）
  bool usageReported = false; // 服务端是否报告了 usage
  int promptTokens = 0;
  int completionTokens = 0;
  int deltaCount = 0;       // 文本增量次数（仅记录，不据此宣布 SSE）
  int httpRequests = 0;     // 本项实际发出的模型请求次数（工具往返 ≤2）
  // 工具往返专属（其余探测为 false/空）：
  bool toolFrameObserved = false;    // 观察到匹配的诊断 echo 帧
  bool toolResultSentBack = false;   // 固定内存结果已按原 call ID 回送
  bool followUpTextCompleted = false; // 第二请求以文本终态完成
  bool secondToolCallSeen = false;   // 后续又请求工具（零第三轮，如实记录）
  QString detail;                    // 已脱敏说明（可为空）
};

// 一次诊断代次的结构化报告（时间戳 + 实际测试候选快照 + 分项观测）。
// 不含任何凭据；历史报告只说明当时快照，不构成当前配置/服务健康证明。
struct LlmDiagnosticReport {
  quint64 generation = 0;
  QDateTime startedAt;
  // 实际测试值快照（不含 key/keySource 之外的任何秘密；keySource 以
  // 诚实来源标注呈现）。
  QUrl endpoint;
  QString model;
  bool draftStream = true;     // 候选的草稿流式开关（上下文；流式探测恒 stream=true）
  int timeoutMs = 30000;
  int totalTimeoutMs = 300000;
  int probeMaxTokens = 0;      // 探测实际使用的输出上限（≤候选允许值）
  int candidateMaxTokens = 0;  // 候选允许值（不裁剪、如实呈现）
  LlmDiagnosticCandidate::KeySource keySource =
    LlmDiagnosticCandidate::KeySource::None;
  LlmCatalogObservation catalog;
  LlmProbeObservation text;
  LlmProbeObservation stream;
  LlmProbeObservation toolRoundTrip;
};

// 诊断动作种类（视图意图 → 控制器入口的一对一映射）。
enum class LlmDiagnosticsAction { Catalog = 0, Text = 1, Stream = 2, Tool = 3 };

// 诊断专用无副作用工具名（任务私有；与 domaintools/runner/pi 注册表全表
// 无冲突——2026-11-22 基线核对）。
inline QString llmDiagnosticEchoToolName() {
  return QStringLiteral("paleo.diagnostic_echo");
}

// 完整规范端点身份（scheme + host + 端口 + base path，尾斜杠不计）。
// W2：沿用当前内存密钥只在身份完全一致时允许——不能仅比较 host 后把旧
// key 发到新的租户路径。
inline bool llmEndpointIdentityMatches(const QUrl &a, const QUrl &b) {
  auto stripSlash = [](QString path) {
    while (path.endsWith(QLatin1Char('/')))
      path.chop(1);
    return path;
  };
  return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
         a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
         a.port() == b.port() &&
         stripSlash(a.path()) == stripSlash(b.path());
}

// 诊断候选的凭据来源判定（视图装配候选时调用；规则 W2）：
//   显式输入优先；清除意图（且无新输入）不复用旧值；端点身份完全一致才
//   沿用当前内存密钥；端点已变更且未输入目标凭据 → 无凭据（本地拒绝），
//   绝不静默搬运旧 key。本判定不改原 applyConfig 的提交合同。
inline LlmDiagnosticCandidate::KeySource llmResolveDiagnosticKeySource(
  bool explicitInput, bool clearRequested, const QUrl &draftEndpoint,
  const QUrl &currentEndpoint, bool hasCurrentKey) {
  if (explicitInput)
    return LlmDiagnosticCandidate::KeySource::ExplicitInput;
  if (clearRequested)
    return LlmDiagnosticCandidate::KeySource::None;
  if (hasCurrentKey && llmEndpointIdentityMatches(draftEndpoint, currentEndpoint))
    return LlmDiagnosticCandidate::KeySource::InMemoryCurrent;
  return LlmDiagnosticCandidate::KeySource::None;
}

} // namespace paleo::chat

// QSignalSpy/队列连接所需的元类型登记（同线程直连不强制，登记无条件无害）。
Q_DECLARE_METATYPE(paleo::chat::LlmCatalogObservation)
Q_DECLARE_METATYPE(paleo::chat::LlmProbeObservation)
Q_DECLARE_METATYPE(paleo::chat::LlmDiagnosticReport)
Q_DECLARE_METATYPE(paleo::chat::LlmDiagnosticsAction)
Q_DECLARE_METATYPE(paleo::chat::LlmDiagnosticStatus)
