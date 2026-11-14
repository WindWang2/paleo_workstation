// 层：功能
// workflow/capabilityrecovery.h — 共享操作协调；数值/推理只通过冻结 worker 接缝。
#pragma once
#include "capabilitygateway.h"
#include "algorithmpublish.h"
#include "services/hostcallcontext.h"
#include "catalog/publicationreceipt.h"
#include <QObject>
#include <QPointer>
#include <atomic>

class PaleoTask;
class PaleoTaskService;
namespace paleo::artifacts { class AuthenticatedArtifactReader; }

namespace paleo::capability {
inline constexpr char OperationContract[] = "paleo.operation.v2";
struct OperationWorkContext {
  QString operationId, attemptId, privateDir, projectDir;
  QJsonObject frozenPlan;
  AlgorithmSpec outputSpec;
  QVector<AlgorithmOutputTarget> targets;
  std::function<bool()> cancelled;
};
struct OperationWorkResult {
  bool ok = false;
  QString code;
  QJsonObject summary;
  AlgorithmRunResult outputs;
  DerivedPreparedSet prepared;
  AlgorithmRunResult diagnostics; // Private, host-created logs; never formal publish members.
};
using OperationWorker = std::function<OperationWorkResult(const OperationWorkContext &)>;
using OperationInputValidator = std::function<QString(const OperationWorkContext &)>;

struct FrozenOperation {
  QString capabilityId, implementationVersion, callerScope, clientRequestId;
  int contractVersion = 1;
  QJsonObject intent; // canonical original intent, before current defaults/provider resolution
  QJsonObject frozenPlan;
  QString phase; // prepare | compute-only | publish
  AlgorithmSpec outputSpec;
  QStringList outputPorts, parentVersionIds;
  QHash<QString, QString> targetNames;
  QJsonObject presentationIntent; // exact target, expected state and exact output version
  QJsonObject externalRequest; // immutable endpoint/model/input hashes; no credentials
  QString sourceOperationId; // publish an already computed immutable result, without numerical work
  OperationWorker worker;
  OperationInputValidator inputValidator;
  unsigned requiredEffects = 0; // Set by trusted adapters, never copied from tool JSON.
};

struct RecoveryHost {
  host::CallContext context;
  QString projectDir, operationDir;
  DataCatalog *catalog = nullptr;
  std::shared_ptr<const ProjectWriteClaim> writeClaim;
  std::function<QString(const OperationLog::Record &)> validateFrozen;
  // "presented" means these exact frozen member versions are already applied;
  // otherwise return the opaque live target state for expectedState CAS.
  std::function<QString(const QJsonObject &)> presentationState;
  std::function<QString(const QJsonObject &, const PublicationReceipt &)> present;
  // Execution adapter supplied by the app. No duplicate defaults/capability registry.
  std::function<OperationWorker(const OperationLog::Record &, QString *)> restoreWorker;
  std::function<OperationInputValidator(const OperationLog::Record &, QString *)> restoreInputValidator;
};

QString operationDigest(const QJsonObject &value);
QJsonObject publicOperation(const OperationLog::Record &record);

class CapabilityRecovery : public QObject {
  Q_OBJECT
public:
  struct Accepted { bool ok = false; QString code; OperationLog::Record record; };
  using HostProvider = std::function<RecoveryHost()>;
  explicit CapabilityRecovery(QObject *parent = nullptr);
  ~CapabilityRecovery() override;
  void setHostProvider(HostProvider provider);
  void invalidateRuntimeHooks(bool cancelRunning = true);
  void setTaskService(PaleoTaskService *tasks);
  Accepted accept(const FrozenOperation &plan);
  Accepted beginExternal(const FrozenOperation &plan);
  QString noteRemoteAccepted(const QString &callerScope, const QString &operationId, const QString &jobId);
  QString reportExternal(const QString &callerScope, const QString &operationId, const QString &jobId,
                         const QString &outcome, const QJsonObject &directCompletionProof = {},
                         bool retainOwnershipForResults = false);
  PaleoTask *attachExternalResults(const QString &callerScope, const QString &operationId, OperationWorker worker,
                                  QObject *subscriber, std::function<void(ExecuteResult)> finished, QString *error = nullptr);
  OperationLog::Accept lookup(const QString &callerScope, const QString &clientRequestId);
  QJsonObject get(const QString &callerScope, const QString &operationId);
  QJsonObject events(const QString &callerScope, const QString &operationId,
                    quint64 afterRevision = 0, int pageSize = 32, int maxBytes = 8 * 1024);
  QJsonObject cancel(const QString &callerScope, const QString &operationId);
  PaleoTask *start(const QString &callerScope, const QString &operationId, QObject *subscriber,
                   const QString &title, std::function<void(ExecuteResult)> finished,
                   bool resume, QString *error = nullptr);
  QJsonObject results(const QString &callerScope, const QString &operationId);
  // Completion is checked/delivered on the recovery owner thread; subscriber bounds its lifetime.
  PaleoTask *read(const QString &callerScope, const QString &operationId, const QString &portId,
                  qint64 offset, int maxBytes, QObject *subscriber,
                  std::function<void(QJsonObject)> finished, QString *error = nullptr);
  // Host-only fault injection. Test child processes exit at these durable boundaries.
  void setCheckpointHook(std::function<void(const QString &)> hook);

private:
  RecoveryHost live();
  OperationLog::Record reconcile(const RecoveryHost &host, OperationLog::Record record);
  ExecuteResult finish(const RecoveryHost &host, OperationLog::Record record,
                       const OperationWorkResult &work);
  OperationLog::Record presentCommitted(const RecoveryHost &host, OperationLog::Record record);
  HostProvider m_host;
  QPointer<PaleoTaskService> m_tasks;
  struct RuntimeHooks {
    OperationWorker worker;
    OperationInputValidator inputValidator;
  };
  QHash<QString, RuntimeHooks> m_runtimeHooks;
  QString m_runtimeDocument, m_runtimeProject;
  quint64 m_runtimeEpoch = 0;
  std::shared_ptr<std::atomic_bool> m_executionInvalidated = std::make_shared<std::atomic_bool>(false);
  QHash<QString, std::shared_ptr<void>> m_externalActive;
  QHash<QString, RecoveryHost> m_externalHosts;
  std::function<void(const QString &)> m_checkpoint;
  std::shared_ptr<artifacts::AuthenticatedArtifactReader> m_resultReader;
};
} // namespace paleo::capability
