// 层：功能
// workflow/capabilitygateway.h — 试点能力的准备、执行、操作身份与预算。
// 不建立第二套算法注册表或任务线程池。
#pragma once

#include "../services/algorithmcontract.h"
#include "../services/capabilityspec.h"

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <functional>

class DataCatalog;

namespace paleo::capability {

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

struct PlanNode {
  QString id;
  QString capabilityId;
  int contractVersion = 1;
  QString outputPort;
  QString intent; // prepare | compute-only | publish
  QVariantMap parameters;
  QVariantMap inputs;
};

struct PlanEdge {
  QString from;
  QString fromPort;
  QString to;
  QString toPort;
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
    QString operationId;
    QString key;
    QString requestHash;
    QString execution;
    QString commitState;
    QString presentation;
    QString code;
    int computeCount = 0;
    QByteArray summaryJson;
  };
  struct Accept {
    QString code;
    Record record;
  };

  explicit OperationLog(const QString &directory);
  // 测试故障注入：先放过 succeedBeforeFail 次 operations.json 写入，再让下一次失败且不改文件。
  // 负数关闭。缓存文件失败不会撤销已经写上的 operation 回执。
  static void armOperationFileWriteFault(int succeedBeforeFail);
  Accept accept(const QString &documentId, const QString &callerScope, const QString &clientRequestId,
                const QString &requestHash);
  Record recordById(const QString &operationId);
  bool save(const Record &record);
  QString cancel(const QString &operationId);
  QByteArray cacheGet(const QString &fingerprint);
  void cachePut(const QString &fingerprint, const QByteArray &summaryJson);

private:
  void load();
  bool persist() const;
  QString m_path;
  QString m_cachePath;
  QHash<QString, Record> m_byKey;
  QHash<QString, QString> m_idToKey;
  QHash<QString, QByteArray> m_cache;
};

QVariantMap effectiveScalarParams(const QString &algorithmId, int contractVersion, const QVariantMap &user,
                                  QString *code);

ExecuteResult execute(const ExecuteRequest &request);
ContractIssue checkPlan(const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges);
ExecuteResult executePlan(const ExecuteRequest &host, const QVector<PlanNode> &nodes, const QVector<PlanEdge> &edges);
QByteArray boundToolResponse(const QJsonObject &object, int budgetBytes = 64 * 1024);
// hostHead 由宿主传入。arguments 里的 current_head 不作为提交事实。
QJsonObject runChatTool(const QString &toolName, const QJsonObject &arguments, const QString &hostScope,
                        const QString &documentId, const QString &operationDir,
                        const QString &hostHead = QString(),
                        const std::function<bool()> &cancelled = {});
bool cancelRequested(const QString &operationId);
void noteIssuedResult(const QString &documentId, const QString &path);
bool isIssuedResult(const QString &documentId, const QString &path);

} // namespace paleo::capability
