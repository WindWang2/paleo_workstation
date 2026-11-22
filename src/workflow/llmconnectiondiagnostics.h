// 层：功能
#pragma once
#include "llmdiagnosticcontract.h"
#include "llmmodelinventory.h"
#include "../ai/chat/chatmessage.h"
#include "../ai/chat/domaintools.h"
#include "../ai/chat/llmclient.h"
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;

namespace paleo::chat {

// H6：模型接入诊断协调器（显式启动、隔离生命周期、分项观测）。
//
// 生命周期（W2）：
//   · 用户点击某项动作才产生一次有界诊断——打开设置/编辑字段/恢复配置/
//     启动应用零联网（测试钉住）。
//   · 单飞：同一时刻至多一项动作在途；在途再启动 = 本地拒绝（该次点击的
//     单次终态），零网络。
//   · 目录/文本/流式各至多一次 HTTP；工具往返至多两次模型请求（第二次仅在
//     首请求完整成功且恰好一个严格匹配帧后发生）。
//   · 失败不自动换服务/换模型/关 stream/循环重试（请求次数由测试对账）。
//   · 候选、凭据、代次在启动时冻结：编辑草稿不影响在途快照；迟到结果不
//     覆盖新候选（LlmClient 世代 + 本对象单飞双重保证）。
//   · cancel 只取消诊断自身的网络对象，绝不取消 G1 配置提交或业务 operation；
//     析构安全终止在途请求（专属 LlmClient/目录请求对象，不借 controller）。
// 隔离（硬约束）：
//   · 不调用 AiChatController、不经 AiChatToolRunner/CapabilityHost/Python/Pi、
//     不读写会话存储——工具往返是内存协议探测，非业务执行。
//   · 探测复用生产 LlmClient（同序列化/传输分类/错误终态）；诊断输出上限
//     不高于候选允许值，实际值进报告。
class LlmConnectionDiagnostics : public QObject {
  Q_OBJECT
public:
  // 探测输出的输出 token 上限（≤候选允许值；实际值记录进报告）。
  static constexpr int kProbeMaxTokens = 64;

  explicit LlmConnectionDiagnostics(QObject *parent = nullptr);
  ~LlmConnectionDiagnostics() override;

  // 测试注入口：为探测客户端与目录请求注入自定义 NAM（生产用默认构造）。
  void setNetworkAccessManagerForTesting(QNetworkAccessManager *nam);

  // 显式启动一项诊断（W1 本地规则先于任何网络：结构借 LlmConfig::validate
  // 的端点规则（占位值仅存在于临时校验对象）、凭据非空、探测需模型名）。
  void runAction(LlmDiagnosticsAction action,
                 const LlmDiagnosticCandidate &candidate);
  void cancel(); // 取消诊断自身在途请求；Running 分项落 Cancelled 单终态
  bool busy() const { return m_busy; }
  LlmDiagnosticReport report() const { return m_report; }

signals:
  void busyChanged(bool busy);
  // 渲染后的有界摘要（已脱敏；视图直接展示，不做任何富文本/链接解释）。
  void summaryChanged(const QString &summary);
  // 目录发现产出的精确 model ID 列表（仅候选；选择只填草稿）。
  void modelCandidatesChanged(const QStringList &models);
  // 本地拒绝（零网络）：结构/凭据/参数原因（面向用户，已翻译）。
  void candidateRejected(const QString &reason);
  void actionFinished(LlmDiagnosticsAction action);

private:
  struct ToolProbe {
    enum class Phase { Idle, AwaitingFrame, ResultSent };
    Phase phase = Phase::Idle;
    QString nonce;
    ChatToolCall frame; // 严格匹配后的真实调用帧（回放原样，不重建）
    int callCount = 0;
    bool observed = false;
  };

  // 本地规则（W1）
  QString structuralProblem(const LlmDiagnosticCandidate &candidate) const;
  QString missingCredentialReason(const LlmDiagnosticCandidate &candidate) const;
  LlmConfig probeConfig(const LlmDiagnosticCandidate &candidate, bool stream) const;
  ChatMessage syntheticUserMessage() const;
  ChatMessage toolPromptMessage() const;
  AiToolSpec echoToolSpec() const;
  // 探测实现
  void createNetworkObjects(); // 专属 inventory/clients 创建 + 信号接线
  void startCatalog(const LlmDiagnosticCandidate &candidate);
  void startTextProbe(const LlmDiagnosticCandidate &candidate);
  void startStreamProbe(const LlmDiagnosticCandidate &candidate);
  void startToolProbe(const LlmDiagnosticCandidate &candidate);
  // 信号收口（成员持久连接；全部只在对应分项 Running 时生效）
  void onProbeFinished(LlmProbeObservation::Kind kind, const QString &reason,
                       int promptTokens, int completionTokens);
  void onProbeError(LlmProbeObservation::Kind kind, LlmErrorKind kindError,
                    const QString &message);
  void onToolFrame(const ChatToolCall &call);
  void onToolFinished(const QString &reason, int promptTokens,
                      int completionTokens);
  void onToolError(LlmErrorKind kind, const QString &message);
  // 工具帧严格匹配（名称/唯一 ID/实参对象/精确 nonce）；detail 出口已脱敏
  bool frameStrictlyMatches(const ChatToolCall &call, QString *why) const;
  void sendToolFollowUp(); // 回放 m_tool.frame（原样帧，不重建）
  // 收口与状态
  void completeAction(LlmDiagnosticsAction action);
  void failObservation(LlmProbeObservation &observation,
                       LlmDiagnosticStatus status, QString detail);
  LlmProbeObservation &observationFor(LlmProbeObservation::Kind kind);
  void publishSummary();

  QNetworkAccessManager *m_testNam = nullptr; // 非拥有（测试注入）
  LlmModelInventory *m_inventory = nullptr;
  LlmClient *m_textClient = nullptr;
  LlmClient *m_streamClient = nullptr;
  LlmClient *m_toolClient = nullptr;
  bool m_busy = false;
  quint64 m_generation = 0; // 诊断代次（每次显式启动 +1；进报告头）
  LlmDiagnosticsAction m_action = LlmDiagnosticsAction::Catalog;
  LlmDiagnosticReport m_report;
  ToolProbe m_tool;
  QByteArray m_diagnosticKey; // 本次诊断凭据（脱敏基准；仅内存）
};

} // namespace paleo::chat
