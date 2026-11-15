// 层：数据
#pragma once
#include "pythonenvironmentconfig.h"
#include "pythonsandboxprovider.h"
#include "pythontrustedreadiness.h"
#include <QHash>
#include <atomic>

namespace paleo::python {
enum class EnvironmentMode { Trusted, Generated };
struct EnvironmentSelection {
  EnvironmentIdentity identity;
  std::shared_ptr<RuntimeProvider> runtime;
  std::shared_ptr<TrustedReadinessProvider> trusted;
  std::shared_ptr<SandboxProvider> sandbox;
  std::shared_ptr<std::atomic_bool> interrupted;
  std::shared_ptr<std::atomic_bool> globalInterruption;
  QString code;
  bool ok() const { return code.isEmpty() && runtime && interrupted && !interrupted->load() &&
      globalInterruption && !globalInterruption->load(); }
};

// A host-owned, thread-safe directory of approved identities. Snapshot/resolve do
// no filesystem I/O, hashing, waiting for process permits or process creation.
// No operation-provided paths are accepted by this resolver.
class PythonEnvironmentRegistry {
public:
  explicit PythonEnvironmentRegistry(QString path = environmentConfigPath());
  QString configurationPath() const { return m_store.path(); }
  quint64 beginRefresh();
  // Host-only immediate deny. No disk, task admission or execution permit.
  // Persistence failure never clears it; explicit verified activation can grant
  // a fresh token without reviving any previously interrupted worker.
  void denyInMemory(const QString &identityId = {});
  QString loadConfiguration(const LegacyPythonEnvironment &, quint64 refresh);
  QString refresh(const LegacyPythonEnvironment &, quint64 refresh,
      const std::function<bool()> &cancelled = {}, bool verifyCached = true);
  EnvironmentConfigurationResult activate(const EnvironmentCandidate &, quint64 expectedRevision,
      const std::function<bool()> &cancelled = {});
  EnvironmentConfigurationResult disable(quint64 expectedRevision);
  EnvironmentConfigurationResult revoke(const QString &identityId, quint64 expectedRevision);
  EnvironmentConfigurationResult rollback(const QString &identityId, quint64 expectedRevision,
      const std::function<bool()> &cancelled = {});
  QJsonObject configurationSnapshot() const; // Host-only: includes approved roots.
  QJsonObject status() const; // Bounded, path-free source/permission diagnostics.
  EnvironmentSelection active(EnvironmentMode) const;
  EnvironmentSelection resolve(const QString &identityId, const QString &runtimeSha256,
      const QJsonObject &frozenPolicy, EnvironmentMode) const;
  // Backend execution proof, independent of the original template directory.
  // New business captures must also pass active(mode)'s complete-input check.
  QJsonObject probe(const QString &identityId, EnvironmentMode,
      const std::function<bool()> &cancelled = {});
private:
  struct Entry {
    EnvironmentIdentity identity;
    std::shared_ptr<RuntimeProvider> runtime;
    std::shared_ptr<TrustedReadinessProvider> trusted;
    std::shared_ptr<SandboxProvider> sandbox;
    std::shared_ptr<std::atomic_bool> trustedInterrupted, generatedInterrupted;
    QString runtimeIntegrityCode = "python_environment_config_pending";
    QString integrityCode = "python_environment_config_pending";
  };
  bool apply(const EnvironmentConfigurationResult &, const QString &source,
      const LegacyPythonEnvironment &, quint64 refresh);
  EnvironmentConfigurationResult applyMutation(EnvironmentConfigurationResult, bool explicitActivation = false,
      quint64 authorizationEpoch = 0);
  bool permitted(const EnvironmentIdentity &, EnvironmentMode) const;
  EnvironmentSelection selected(const std::shared_ptr<Entry> &, EnvironmentMode, bool requireTemplates = true) const;
  PythonEnvironmentStore m_store;
  mutable QMutex m_mutex;
  EnvironmentConfiguration m_configuration;
  LegacyPythonEnvironment m_legacy;
  QHash<QString, std::shared_ptr<Entry>> m_entries;
  QString m_source = "pending", m_code = "python_environment_config_pending";
  QSet<QString> m_deniedIds;
  std::shared_ptr<std::atomic_bool> m_globalInterruption = std::make_shared<std::atomic_bool>(false);
  std::atomic<quint64> m_refresh{0};
  std::atomic<quint64> m_authorizationEpoch{0};
};
} // namespace paleo::python
