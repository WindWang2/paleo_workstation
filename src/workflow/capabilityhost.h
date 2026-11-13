// 层：功能
#pragma once
#include "capabilitygateway.h"
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
  // Both callbacks run on the owner thread. They must read actual host state.
  std::function<QString(ExecuteRequest &)> freeze;
  std::function<QString(const ExecuteRequest &)> validateInputs;
};

struct HostPrepared {
  bool ok = false;
  QString code, handle;
  QVariantMap effectiveParameters;
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
  QString callerScope(host::Caller caller) const;
  host::CallContext context() const;
  QString preparedCommand(const QString &handle) const;
  HostPrepared prepare(const ExecuteRequest &intent, host::Caller caller,
                       const QStringList &requiredContext = {}, const QString &commandId = {},
                       std::function<QString()> executionGuard = {});
  ExecuteResult execute(const QString &handle, const ExecuteRequest &originalIntent);
  PaleoTask *start(const QString &handle, const ExecuteRequest &originalIntent,
                   QObject *receiver, const QString &title,
                   std::function<void(ExecuteResult)> finished, QString *error = nullptr);
  void invalidate();

private:
  struct Prepared {
    ExecuteRequest request;
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
  BindingProvider m_binding;
  QPointer<PaleoTaskService> m_tasks;
  QHash<QString, Prepared> m_prepared;
  QHash<int, QString> m_callers;
};

} // namespace paleo::capability
