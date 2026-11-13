// 层：功能
// workflow/capabilitygateway.h — 试点能力的准备、执行、操作身份与预算。
// 不建立第二套算法注册表或任务线程池。
#pragma once

#include "../services/algorithmcontract.h"
#include "../services/capabilityspec.h"
#include "capabilityplan.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QByteArrayView>
#include <QMutex>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <memory>

class DataCatalog;
class ProjectWriteClaim;
namespace paleo::document { class DocumentSession; }

namespace paleo::capability {

struct ResolvedSeismicTraceInput;
struct SeismicTraceSelection;

struct ExecuteRequest {
  QString capabilityId;
  int contractVersion = 1;
  QString documentId;
  QString callerScope;
  QString clientRequestId;
  QString bindingMode;
  QString bindingId;
  QString frozenHead;
  QString currentHead;
  QVariantMap parameters;
  QVariantMap inputs;
  QString publishIntent; // prepare | compute-only | publish。空表示 prepare（约束）或既有计算（时深/地震）
  bool presentOk = true;
  std::function<bool()> cancelled;
  // 宿主在运行标记落下之后调用，便于把取消打进正在跑的循环。不是模型参数。
  std::function<void(const QString &operationId)> onRunning;
  QString operationDir;
  DataCatalog *catalog = nullptr;
  QString projectDir;
  std::shared_ptr<const ProjectWriteClaim> writeClaim; // host-only, never decoded from JSON
  // Synchronous worker-only sink for actual numerical values; owns no QObject,
  // Catalog or presentation callback. Empty means the legacy summary-only path.
  std::function<QString(const QString &portId, QByteArrayView bytes)> stageBytes;
  std::shared_ptr<const ResolvedSeismicTraceInput> seismicTrace;
};

struct ExecuteResult {
  bool ok = false;
  QString code;
  QString operationId;
  QString execution;
  QString commitState;
  QString presentation;
  QString requestHash;
  bool cacheHit = false;
  bool recomputed = false;
  QVariantMap summary;
  QVector<ContractIssue> issues;
  QByteArray responseJson;
};

// 宿主线程上已经冻结的点与约束。不含 Qgs 类型，避免网关再散布 QGIS 头。
struct FrozenIdwJob {
  QVariantList points; // x, y, z, well_id
  QVariantList lines;  // wkt, type
  QString field = QStringLiteral("z");
  double cellSize = 0;
  double aniso = 2;
  QString outputPath;
  QString gridWkt;
  std::function<bool()> cancelled;
};

struct FrozenIdwResult {
  bool ok = false;
  QString code;
  QString rasterPath;
  int width = 0;
  int height = 0;
  QString receipts;
};

FrozenIdwResult runFrozenConstraintIdw(const FrozenIdwJob &job);

class ProcessBudgetLedger {
public:
  static ProcessBudgetLedger &instance();
  void setLimit(qint64 bytes);
  qint64 limit() const;
  qint64 reserved() const;
  bool tryReserve(qint64 bytes);
  void release(qint64 bytes);

private:
  ProcessBudgetLedger() = default;
  mutable QMutex m_mutex;
  qint64 m_limit = 256LL * 1024LL * 1024LL;
  qint64 m_reserved = 0;
};

class OperationLog {
public:
  struct Record {
    int schemaVersion = 1;
    QString operationId;
    QString key;
    QString requestHash;
    QString execution;
    QString commitState;
    QString presentation;
    QString code;
    int computeCount = 0;
    QByteArray summaryJson;
    QString documentId, callerScope, clientRequestId;
    QString capabilityId, implementationVersion, planHash, storeGeneration;
    int contractVersion = 1;
    quint64 stateRevision = 0;
    QString ownerAttempt, ownerProcess;
    bool cancelRequested = false;
    QJsonObject frozenRequest, stagePlan, preparedManifest, artifactManifest, presentationIntent;
    QJsonObject externalRequest; // frozen IDs/hashes only, never credentials
    QJsonObject remoteCompletionProof; // direct synchronous response, bound to frozen request hash
    QString remoteJobId, externalOutcome;
    QJsonArray events;
    bool operator==(const Record &) const = default;
  };
  struct Accept {
    QString code;
    Record record;
  };

  explicit OperationLog(const QString &directory);
  OperationLog(const QString &directory, const QString &projectDir,
               std::shared_ptr<const ProjectWriteClaim> claim);
  // 测试故障注入：先放过 succeedBeforeFail 次 operations.json 写入，再让下一次失败且不改文件。
  // 负数关闭。缓存文件失败不会撤销已经写上的 operation 回执。
  static void armOperationFileWriteFault(int succeedBeforeFail);
  Accept accept(const QString &documentId, const QString &callerScope, const QString &clientRequestId,
                const QString &requestHash);
  Record recordById(const QString &operationId);
  Record artifactOwner(const QString &documentId, const QString &callerScope, const QString &relativePath);
  Accept lookup(const QString &documentId, const QString &callerScope, const QString &clientRequestId);
  Accept acceptDurable(const Record &seed);
  enum class Transition { Advance, Resume, Reconcile };
  QString saveDurable(const Record &record, quint64 expectedRevision, const QString &expectedAttempt,
                      Record *current = nullptr, Transition transition = Transition::Advance);
  bool ownsWrites() const;
  // current receives the persisted record, including on a rejected transition.
  bool save(const Record &record, Record *current = nullptr);
  QString cancel(const QString &operationId);
  QByteArray cacheGet(const QString &fingerprint);
  void cachePut(const QString &fingerprint, const QByteArray &summaryJson);

private:
  bool loadOperations();
  bool persistOperations() const;
  QString m_path;
  QString m_cachePath;
  QString m_projectDir;
  std::shared_ptr<const ProjectWriteClaim> m_claim;
  QHash<QString, Record> m_byKey;
  QHash<QString, QString> m_idToKey;
};

QVariantMap effectiveScalarParams(const QString &algorithmId, int contractVersion, const QVariantMap &user,
                                  QString *code);

ExecuteResult execute(const ExecuteRequest &request);
// Resolve the document's source on the owner thread and retain only immutable
// value references for the computation worker.
QString bindSeismicTrace(ExecuteRequest &request, const document::DocumentSession &document,
                        const SeismicTraceSelection &selection);
ExecuteResult executePlan(const ExecuteRequest &host, const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges);
QByteArray boundToolResponse(const QJsonObject &object, int budgetBytes = 64 * 1024);
// Converts a chat request without reading model data or starting work.
// An empty object indicates that the host can dispatch the returned value.
QJsonObject prepareChatToolRequest(ExecuteRequest &out, const QString &toolName, const QJsonObject &arguments,
                                   const QString &hostScope, const QString &documentId, const QString &operationDir,
                                   const QString &hostHead = QString());
QJsonObject chatToolResponse(const ExecuteResult &result);
// hostHead 由宿主传入。arguments 里的 current_head 不作为提交事实。
QJsonObject runChatTool(const QString &toolName, const QJsonObject &arguments, const QString &hostScope,
                        const QString &documentId, const QString &operationDir,
                        const QString &hostHead = QString(),
                        const std::function<bool()> &cancelled = {},
                        DataCatalog *catalog = nullptr, const QString &projectDir = QString());
bool cancelRequested(const QString &operationId);
void noteIssuedResult(const QString &documentId, const QString &path);
bool isIssuedResult(const QString &documentId, const QString &path);

} // namespace paleo::capability
