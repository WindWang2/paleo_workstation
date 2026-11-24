// 层：功能
#pragma once
#include "../ai/chat/llmclient.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <deque>
#include <functional>

class PaleoTask;
class PaleoTaskService;

// workflow/ — 编图解释后台自动生成服务（独立工作线程 + 可选 AI 生成）。
//
// 职责：
//   1. 编图过程不阻塞主线程与画布编辑；所有提示构建、AI HTTP 请求/应答解析与
//      无 AI 时的本地事实摘要均在独立工作线程（QThread）执行。
//   2. 已配置可用 AI（LlmConfig::enabled()）时优先调用 LlmClient 生成简明中文
//      地质说明与修改目的；未配置 AI 或 AI 请求失败/超时时，如实退化为基于
//      冻结工区事实的本地摘要，并记录来源与退化原因，不编造地质观测。
//   3. 结果始终标为待复核草稿，不自动确认解释，也不直接触碰 QGIS/Catalog 对象。
struct MappingExplanationRequest {
  quint64 generation = 0;
  quint64 sequence = 0;
  QString requestId;
  QString projectDir;
  QString horizon;
  QString unitDisplayName;
  QString layerId;
  QString versionId;
  QString featureUid;
  QString geometryHash;
  int faciesCode = 0;
  QString faciesPath;
  int beforeFaciesCode = 0;
  QString beforeFaciesPath;
  QString operationKind;
  double areaSqm = 0.0;
  int featureCount = 0;
  QVariantList wellsInFeature;
  QStringList referenceTitles;
  QStringList referenceVersionIds;
  QString attributionSummary;
  bool applyToEditBasis = false;
  LlmConfig llmConfig;
};

struct MappingExplanationResult {
  quint64 generation = 0;
  quint64 sequence = 0;
  QString requestId;
  QString projectDir;
  QString horizon;
  QString layerId;
  QString versionId;
  QString featureUid;
  QString geometryHash;
  int faciesCode = 0;
  QString text;
  QString reason;
  QString generator;      // "ai" | "local_factual"
  QString model;          // LLM 模型名（仅 generator=="ai" 非空）
  QString fallbackReason; // AI 尝试失败后退化原因（如实披露）
  QString generatedAt;    // UTC ISO8601
  QStringList referenceVersionIds;
  bool applyToEditBasis = false;
  bool cancelled = false;
  quintptr workerThreadId = 0;

  QVariantMap toVariantMap() const;
  static MappingExplanationResult fromVariantMap(const QVariantMap &map);
};

Q_DECLARE_METATYPE(MappingExplanationRequest)
Q_DECLARE_METATYPE(MappingExplanationResult)

class MappingExplanationService : public QObject {
  Q_OBJECT
public:
  explicit MappingExplanationService(QObject *parent = nullptr);
  ~MappingExplanationService() override;

  void setTaskService(PaleoTaskService *tasks);
  void setLlmConfig(const LlmConfig &config);
  LlmConfig llmConfig() const { return m_llmConfig; }
  bool aiAvailable() const { return m_llmConfig.enabled(); }

  void setEnabled(bool enabled);
  bool enabled() const { return m_enabled; }
  bool busy() const { return m_busy; }
  QString statusText() const;

  // 提交后台解释任务（可由自动触发或用户显式点击触发）。
  // 同 (layerId, featureUid) 的未执行请求会被最新快照合并替代。
  QString enqueue(MappingExplanationRequest request);
  void cancelAll();

  // 纯逻辑助手（可在工作线程或单测直接调用）。
  static QVector<ChatMessage> buildPromptMessages(const MappingExplanationRequest &req);
  static bool parseAiResponse(const QString &raw, const MappingExplanationRequest &req,
                              QString *text, QString *reason);
  static void buildLocalFactualExplanation(const MappingExplanationRequest &req,
                                           QString *text, QString *reason);

signals:
  void busyChanged(bool busy, const QString &detail);
  void statusChanged();
  void explanationReady(const MappingExplanationResult &result);

private:
  class Worker;
  void ensureWorker();
  void beginTaskIfNeeded(const QString &layerId, bool useAi);
  void finishTaskIfIdle(const QString &error = {});
  void onWorkerFinished(const MappingExplanationResult &result, int remaining,
                        quint64 highWaterSeq);

  LlmConfig m_llmConfig;
  QPointer<PaleoTaskService> m_taskSvc;
  QPointer<PaleoTask> m_task;
  QThread m_thread;
  Worker *m_worker = nullptr;
  std::shared_ptr<std::atomic<quint64>> m_generation;
  quint64 m_lastEnqueuedSeq = 0;
  bool m_enabled = true;
  bool m_busy = false;
  QString m_activeDetail;
};
