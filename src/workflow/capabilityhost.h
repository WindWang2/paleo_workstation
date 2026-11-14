// 层：功能
#pragma once
#include "capabilitygateway.h"
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
struct CapabilityExecutionAdapter {
  std::function<QJsonObject(const ExecuteRequest &)> intent;
  std::function<FrozenOperation(const ExecuteRequest &,const QJsonObject &,QString *)> freeze;
  std::function<OperationWorker(const OperationLog::Record &,QString *)> restoreWorker;
  std::function<OperationInputValidator(const OperationLog::Record &,QString *)> restoreInputValidator;
  std::function<QString(const OperationLog::Record &)> validateFrozen;
};

// One policy/execution seam for product adapters. The host retains immutable
// requests; a public PreparedPlan::ok or edited JSON is never a permit.
class CapabilityHost : public QObject {
  Q_OBJECT
public:
  using BindingProvider = std::function<HostBinding()>;
  explicit CapabilityHost(QObject *parent = nullptr);
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
  };
  QString check(const Prepared &prepared, const ExecuteRequest &intent, HostBinding &live) const;
  HostBinding binding() const;
  void prunePrepared(const host::CallContext &context);
  static QByteArray requestDigest(const ExecuteRequest &request);
  BindingProvider m_binding;
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
};

} // namespace paleo::capability
