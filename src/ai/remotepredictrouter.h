// 层：功能
#pragma once
#include "remotepredictionservice.h"
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class PaleoOnnxService;

// ai/ — 远端推理路由（原 goal/ai-geological-assist 范围6，方向51 重写传输为异步）。
//
// 策略：先健康检查（/health，超时判死）→ 健康走远端预测（/predict，超时判
// 失败）→ 任一环节失败且允许降级 → 本地同网格重跑（请求须携带 samples 数据
// ——没有数据就不造假，如实失败）。全部环节错误链保留原始信息。无默认外网
// 端点：base URL 由调用方显式给（测试用 loopback）。
//
// 方向51 两条硬约束：
//  ① 全链路异步：传输不许阻塞任何线程（旧实现嵌套事件循环等
//     QNetworkReply::finished，GUI 线程会被卡拉住）。超时用 QTimer 判，
//     取消/重入用轮次号（runId）作废迟到的应答。
//  ② 本地降级抽象化：router 只认 LocalPredictor 接口，不再直接调 ORT——
//     没有 ONNX 运行库的构建里降级语义仍可被测（桩 Predictor 走同一条路）。

// ---------------------------------------------------------------------------
// LocalPredictor — 本地降级引擎接口（实现见 onnxlocalpredictor.cpp（ORT））
// ---------------------------------------------------------------------------
class LocalPredictor {
public:
  virtual ~LocalPredictor();
  // 引擎在场且可跑；返回 false = 降级不可用（router 走如实失败，不造假）。
  virtual bool isConfigured() const = 0;
  // 结果 method 字段（如 "local-ort:seg3"），同时用于 lastRoute()。
  virtual QString engineId() const = 0;
  // 同网格重跑：成功填 cells（columns×rows 行主序相码，255=nodata）。
  virtual bool predict(const RemotePredictionRequest &request,
                       QVector<int> &cells, QString *error) = 0;
};

// ---------------------------------------------------------------------------
// RemoteTransport — 传输抽象（异步；测试注入 loopback 假服务器）
// ---------------------------------------------------------------------------
class RemoteTransport : public QObject {
  Q_OBJECT
public:
  explicit RemoteTransport(QObject *parent = nullptr);
  ~RemoteTransport() override;
  // 三个方法都立即返回；结果经信号回。runId 是**调用方给的轮次号**，
  // 实现必须在信号里原样带回——调用方据此丢弃上一轮迟到的应答
  // （取消/重入后到达的结果绝不能被当成新结论）。
  virtual void healthCheck(int timeoutMs, quint64 runId) = 0;
  virtual void predict(const RemotePredictionRequest &request, int timeoutMs,
                       quint64 runId) = 0;
  // 中止进行中的请求（不等也不回信号）；由 router 的 cancel() 调用。
  virtual void abort() = 0;
signals:
  void healthDone(quint64 runId, bool ok, const QString &error);
  void wellPredictDone(quint64 runId, const QVariantList &points);
  void predictDone(quint64 runId, const QVector<int> &cells,
                   const QString &error);
};

// HTTP 实现（QNetworkAccessManager）。全异步：finished 信号处理 + QTimer
// 超时；超时先 bump 世代号再 abort，保证 finishing 的 reply 被判为过期。
class HttpRemoteTransport : public RemoteTransport {
  Q_OBJECT
public:
  explicit HttpRemoteTransport(const QUrl &base, QObject *parent = nullptr);
  ~HttpRemoteTransport() override;
  void healthCheck(int timeoutMs, quint64 runId) override;
  void predict(const RemotePredictionRequest &request, int timeoutMs,
               quint64 runId) override;
  void abort() override;

  // 诊断：传输层自身的作废计数（测试可按此断言 abort 确实发生过）。
  int generation() const { return m_generation; }

private:
  // 进行中的操作种类：决定超时/结束时应回 healthDone 还是 predictDone。
  enum class Op { None, Health, Predict };
  void begin(int timeoutMs, Op op, QNetworkReply *reply, quint64 runId);

  Op m_op = Op::None;
  quint64 m_runId = 0;       // 在跑那一轮的轮次号（回包原样带回）
  int m_opTimeoutMs = 0;
  int m_gridColumns = 0;
  int m_gridRows = 0;
  bool m_wellsRequest = false;
  QUrl m_base;
  QNetworkAccessManager *m_nam = nullptr;
  QPointer<QNetworkReply> m_reply;
  QTimer *m_timer = nullptr;
  int m_generation = 0;
};

// ---------------------------------------------------------------------------
// RemotePredictionRouter — 实现既有 RemotePredictionService 接口
// （mappingworkbench 消费面零改动）
// ---------------------------------------------------------------------------
class RemotePredictionRouter : public RemotePredictionService {
  Q_OBJECT
public:
  explicit RemotePredictionRouter(RemoteTransport *transport,
                                  QObject *parent = nullptr);
  ~RemotePredictionRouter() override;

  // 本地降级引擎（可选；所有权不转移）。router 只按接口调用。
  void setLocalPredictor(LocalPredictor *predictor);
  // ORT 便捷构造：内部建 OnnxLocalPredictor（实现在 onnxlocalpredictor.cpp，
  // 无 ONNX 运行库的构建不编译该 TU）。
  void setLocalFallback(PaleoOnnxService *onnx, const QString &model);
  void setFallbackEnabled(bool enabled); // 默认 true（绑了本地才有意义）
  void setHealthTimeoutMs(int ms) { m_healthTimeoutMs = ms; }
  void setPredictTimeoutMs(int ms) { m_predictTimeoutMs = ms; }
  int healthTimeoutMs() const { return m_healthTimeoutMs; }
  int predictTimeoutMs() const { return m_predictTimeoutMs; }

  void start(const RemotePredictionRequest &request) override;
  void cancel() override;
  // 当前链是否仍在跑（cancel 后为 false）。
  bool busy() const { return m_running; }

  // 诊断：最近一次请求走了哪条路（"remote" / "<engineId>" / 空）。
  QString lastRoute() const { return m_lastRoute; }

private:
  void beginChain(quint64 runId, const RemotePredictionRequest &request);
  void onHealth(quint64 runId, bool ok, const QString &error);
  void onPredict(quint64 runId, const QVector<int> &cells,
                 const QString &error);
  void degradeOrDie(quint64 runId, const QString &stageName,
                    const QString &stageError);
  void settleFail(const QString &reason);

  QPointer<RemoteTransport> m_transport;
  LocalPredictor *m_local = nullptr;             // 非拥有（调用方持有）
  std::unique_ptr<LocalPredictor> m_ownedLocal; // setLocalFallback 建的引擎
  bool m_fallbackEnabled = true;
  int m_healthTimeoutMs = 2000;
  int m_predictTimeoutMs = 10000;
  QString m_lastRoute;
  RemotePredictionRequest m_request;
  quint64 m_runId = 0;      // 轮次号：每次 start 递增；cancel 置 0 作废在飞的一轮
  quint64 m_nextRunId = 0;  // 只增不复用，避免旧回包撞上新轮次
  bool m_running = false;
  bool m_cancelled = false;
};
