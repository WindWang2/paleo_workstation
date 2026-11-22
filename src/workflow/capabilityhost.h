// 层：功能
#pragma once
#include "capabilitygateway.h"
#include "capabilityplan_outputs.h"
#include "capabilityrecovery.h"
#include "services/hostcallcontext.h"
#include <QObject>
#include <QHash>
#include <QPointer>

class PaleoTask;
class PaleoTaskService;

namespace paleo::capability {

struct HostBinding {
  host::CallContext context;
  QString projectDir, operationDir;
  DataCatalog *catalog = nullptr;
  std::shared_ptr<const ProjectWriteClaim> writeClaim;
  // Both callbacks run on the owner thread. They must read actual host state.
  std::function<QString(ExecuteRequest &)> freeze;
  std::function<QString(const ExecuteRequest &)> validateInputs;
};

struct HostPrepared {
  bool ok = false;
  QString code, handle;
  QVariantMap effectiveParameters;
};
using CapabilityPrepareWorker=std::function<FrozenOperation(const std::function<bool()> &,QString *)>;
struct CapabilityPreparation {
  CapabilityPrepareWorker worker;
  std::function<QString()> validate; // Owner-only identity check after materialization.
};
struct CapabilityExecutionAdapter {
  std::function<QJsonObject(const ExecuteRequest &)> intent;
  std::function<FrozenOperation(const ExecuteRequest &,const QJsonObject &,QString *)> freeze;
  std::function<OperationWorker(const OperationLog::Record &,QString *)> restoreWorker;
  std::function<OperationInputValidator(const OperationLog::Record &,QString *)> restoreInputValidator;
  std::function<QString(const OperationLog::Record &)> validateFrozen;
  std::function<CapabilityPreparation(const ExecuteRequest &,const QJsonObject &,QString *)> capture;
  // Native fallback preflight; registered adapters retain their own freeze/capture.
  std::function<QString(ExecuteRequest &,QVariantMap &)> normalize;
};
using NativeAdapterFactory = std::function<CapabilityExecutionAdapter(const HostBinding &)>;

// One policy/execution seam for product adapters. The host retains immutable
// requests; a public PreparedPlan::ok or edited JSON is never a permit.
class CapabilityHost : public QObject {
public:
  using BindingProvider = std::function<HostBinding()>;
  explicit CapabilityHost(QObject *parent = nullptr);
  // An empty factory is an explicit host profile with no native fallback.
  explicit CapabilityHost(NativeAdapterFactory nativeFactory, QObject *parent);
  ~CapabilityHost() override;
  void setBindingProvider(BindingProvider provider);
  void setTaskService(PaleoTaskService *tasks);
  void setCallerScope(host::Caller caller, const QString &scope);
  QString callerScope(host::Caller caller, const QString &trustedChatSessionId = {}) const;
  CapabilityRecovery *recovery() const { return m_recovery; }
  void setWorkerRestorer(std::function<OperationWorker(const OperationLog::Record &, QString *)> restorer);
  void setPresentationHandlers(std::function<QString(const QJsonObject &)> state,
      std::function<QString(const QJsonObject &, const PublicationReceipt &)> present);
  void setInputValidatorRestorer(std::function<OperationInputValidator(const OperationLog::Record &, QString *)> restorer);
  void setFrozenValidator(std::function<QString(const OperationLog::Record &)> validator);
  void registerExecutionAdapter(const QString &capabilityId,CapabilityExecutionAdapter adapter);
  host::CallContext context() const;
  QString preparedCommand(const QString &handle) const;
  HostPrepared prepare(const ExecuteRequest &intent, host::Caller caller,
                       const QStringList &requiredContext = {}, const QString &commandId = {},
                       std::function<QString()> executionGuard = {}, const QString &trustedChatSessionId = {});
  HostPrepared preparePlan(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges,
                           host::Caller caller,const QString &trustedChatSessionId = {});
  // H3 v2：显式 exports 的多成果计划。票据语义（进程内 opaque、容量/到期/
  // epoch/重入保护）与 v1 相同；exports 的增删/别名/来源/顺序变化都进入
  // 意图摘要——同 key 不等价请求报 idempotency_conflict，不建第二父操作。
  HostPrepared preparePlanV2(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,const QVector<PlanEdge> &edges,
                             const PlanExports &exports,host::Caller caller,const QString &trustedChatSessionId = {});
  ExecuteResult execute(const QString &handle, const ExecuteRequest &originalIntent);
  PaleoTask *start(const QString &handle, const ExecuteRequest &originalIntent,
                   QObject *receiver, const QString &title,
                   std::function<void(ExecuteResult)> finished, QString *error = nullptr);
  void invalidate();

private:
  struct Prepared {
    ExecuteRequest request;
    FrozenOperation frozen;
    QString recoveryId;
    host::CallContext context;
    QByteArray intentDigest;
    QStringList required;
    QString commandId;
    std::function<QString()> executionGuard;
    unsigned effects = 0;
    qint64 expiresAt = 0;
    CapabilityPreparation preparation;
  };
  // v1/v2 计划入口共享的票据装配路径；exports 为空指针表示 v1 单成果。
  HostPrepared preparePlanCommon(const ExecuteRequest &intent,const QVector<PlanNode> &nodes,
                                 const QVector<PlanEdge> &edges,const PlanExports *exports,
                                 host::Caller caller,const QString &trustedChatSessionId);
  QString check(const Prepared &prepared, const ExecuteRequest &intent, HostBinding &live) const;
  HostBinding binding() const;
  void prunePrepared(const host::CallContext &context);
  PaleoTask *startPreparing(const QString &handle,const Prepared &saved,const ExecuteRequest &intent,
      QObject *receiver,const QString &title,std::function<void(ExecuteResult)> finished,QString *error);
  static QByteArray requestDigest(const ExecuteRequest &request);
  BindingProvider m_binding;
  NativeAdapterFactory m_nativeAdapterFactory;
  QPointer<PaleoTaskService> m_tasks;
  QHash<QString, Prepared> m_prepared;
  QHash<int, QString> m_callers;
  CapabilityRecovery *m_recovery = nullptr;
  std::function<OperationWorker(const OperationLog::Record &, QString *)> m_restoreWorker;
  std::function<QString(const QJsonObject &)> m_presentationState;
  std::function<QString(const QJsonObject &, const PublicationReceipt &)> m_present;
  std::function<OperationInputValidator(const OperationLog::Record &, QString *)> m_restoreInputValidator;
  std::function<QString(const OperationLog::Record &)> m_validateFrozen;
  QHash<QString,CapabilityExecutionAdapter> m_adapters;
  QHash<QString,QByteArray> m_pendingPreparations;
  std::shared_ptr<std::atomic_bool> m_preparationInvalidated=std::make_shared<std::atomic_bool>(false);
};

} // namespace paleo::capability
