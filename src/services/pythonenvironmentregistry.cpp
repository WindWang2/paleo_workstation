// 层：数据
#include "pythonenvironmentregistry.h"
#include "pythonenvironmentconfig_internal.h"
#include <QJsonArray>
#include <QMutexLocker>
#include <QScopeGuard>

namespace paleo::python {
namespace {
bool within(const ExecutionPolicy &policy, const ExecutionPolicy &ceiling) {
  return policy.validate().isEmpty() && ceiling.validate().isEmpty() && policy.codeBytes <= ceiling.codeBytes && policy.inputBytes <= ceiling.inputBytes &&
      policy.outputBytes <= ceiling.outputBytes && policy.logBytes <= ceiling.logBytes && policy.diskBytes <= ceiling.diskBytes &&
      policy.memoryBytes <= ceiling.memoryBytes && policy.cpuSeconds <= ceiling.cpuSeconds && policy.wallSeconds <= ceiling.wallSeconds;
}
bool enabled(const ExecutionPolicy &policy, EnvironmentMode mode) {
  return mode == EnvironmentMode::Trusted ? policy.allowTrusted : policy.allowGenerated;
}
QString permissionName(EnvironmentMode mode) {
  return mode == EnvironmentMode::Trusted ? "PALEO_AGENT_PYTHON_ENABLE_TRUSTED" : "PALEO_AGENT_PYTHON_ENABLE_GENERATED";
}
} // namespace
PythonEnvironmentRegistry::PythonEnvironmentRegistry(QString path) : m_store(std::move(path)) {}
quint64 PythonEnvironmentRegistry::beginRefresh() { return ++m_refresh; }
void PythonEnvironmentRegistry::denyInMemory(const QString &id) {
  if (id.isEmpty()) std::atomic_load(&m_globalInterruption)->store(true);
  QMutexLocker lock(&m_mutex); ++m_authorizationEpoch;
  if (id.isEmpty() || m_deniedIds.size() >= 256) {
    std::atomic_load(&m_globalInterruption)->store(true);
    for (const auto &entry : m_entries) {
      if (entry->trustedInterrupted) entry->trustedInterrupted->store(true);
      if (entry->generatedInterrupted) entry->generatedInterrupted->store(true);
    }
    return;
  }
  m_deniedIds.insert(id);
  const auto entry = m_entries.value(id);
  if (entry) {
    if (entry->trustedInterrupted) entry->trustedInterrupted->store(true);
    if (entry->generatedInterrupted) entry->generatedInterrupted->store(true);
  }
}
bool PythonEnvironmentRegistry::permitted(const EnvironmentIdentity &identity, EnvironmentMode mode) const {
  if (!m_code.isEmpty() || !m_configuration.enabled || m_configuration.revoked.contains(identity.id) ||
      std::atomic_load(&m_globalInterruption)->load() || m_deniedIds.contains(identity.id) ||
      identity.compatibility != environmentCompatibility() || !enabled(identity.policy, mode) ||
      m_legacy.value(permissionName(mode)) == "0") return false;
  return enabled(m_configuration.permissionCeiling, mode) && within(identity.policy, m_configuration.permissionCeiling);
}
bool PythonEnvironmentRegistry::apply(const EnvironmentConfigurationResult &loaded, const QString &source,
    const LegacyPythonEnvironment &legacy, quint64 refresh) {
  QMutexLocker lock(&m_mutex);
  if (m_refresh.load() != refresh) return false;
  if (source == "host-config" && loaded.ok() && m_configuration.exists &&
      loaded.configuration.revision < m_configuration.revision) return false;
  m_configuration = loaded.configuration; m_source = source; m_legacy = legacy;
  m_code = loaded.ok() ? QString() : loaded.code;
  auto previous = m_entries; QHash<QString, std::shared_ptr<Entry>> entries;
  auto identities = m_configuration.approved;
  // The legacy source was already host-authorized in this process. Keep its
  // captured workers alive during ordinary migration; no such approval is
  // invented after restart or read from an operation file.
  if (loaded.ok() && loaded.configuration.exists)
    for (const auto &entry : previous)
      if (entry->identity.legacyTemplates && !m_configuration.revoked.contains(entry->identity.id)) identities.append(entry->identity);
  for (const auto &identity : identities) {
    auto entry = previous.value(identity.id);
    if (!entry) {
      entry = std::make_shared<Entry>(); entry->identity = identity;
      entry->runtime = loaded.verifiedIdentityId == identity.id && loaded.verifiedRuntime ? loaded.verifiedRuntime :
          std::make_shared<RuntimeProvider>(RuntimeConfig{identity.runtimeRoot, identity.runtimeSha256, true}, identity.policy);
      entry->trusted = std::make_shared<TrustedReadinessProvider>(entry->runtime);
      entry->sandbox = std::make_shared<SandboxProvider>(entry->runtime);
    }
    if (loaded.verifiedIdentityId == identity.id && loaded.verifiedRuntime) {
      // Never replace the provider member of an Entry retained by another
      // verification worker. Old selections keep immutable provider pointers.
      entry = std::make_shared<Entry>(*entry);
      entry->runtime = loaded.verifiedRuntime;
      entry->trusted = std::make_shared<TrustedReadinessProvider>(entry->runtime);
      entry->sandbox = std::make_shared<SandboxProvider>(entry->runtime);
      entry->runtimeIntegrityCode.clear();
      entry->integrityCode.clear();
    }
    const auto token = [](std::shared_ptr<std::atomic_bool> &value, bool allowed) {
      if (!allowed) { if (value) value->store(true); else value = std::make_shared<std::atomic_bool>(true); }
      else if (!value || value->load()) value = std::make_shared<std::atomic_bool>(false);
    };
    token(entry->trustedInterrupted, permitted(identity, EnvironmentMode::Trusted));
    token(entry->generatedInterrupted, permitted(identity, EnvironmentMode::Generated));
    entries.insert(identity.id, entry); previous.remove(identity.id);
  }
  for (const auto &entry : previous) {
    if (entry->trustedInterrupted) entry->trustedInterrupted->store(true);
    if (entry->generatedInterrupted) entry->generatedInterrupted->store(true);
  }
  m_entries = std::move(entries); return true;
}
QString PythonEnvironmentRegistry::loadConfiguration(const LegacyPythonEnvironment &legacy, quint64 refresh) {
  auto loaded = m_store.load(); QString source = "host-config";
  if (loaded.ok() && !loaded.configuration.exists) { loaded = legacyEnvironment(legacy); source = legacy.isEmpty() ? "disabled" : "legacy-env"; }
  if (!apply(loaded, source, legacy, refresh)) return "python_environment_config_superseded";
  return loaded.ok() ? QString() : loaded.code;
}
QString PythonEnvironmentRegistry::refresh(const LegacyPythonEnvironment &legacy, quint64 refresh,
    const std::function<bool()> &cancelled, bool verifyCached) {
  const auto loadCode = loadConfiguration(legacy, refresh); if (!loadCode.isEmpty()) return loadCode;
  QVector<std::shared_ptr<Entry>> entries;
  { QMutexLocker lock(&m_mutex);
    if (m_entries.contains(m_configuration.activeId)) entries.append(m_entries.value(m_configuration.activeId));
    for (const auto &entry : m_entries) if (entry->identity.id != m_configuration.activeId) entries.append(entry);
  }
  const auto stopped = [&] { return m_refresh.load() != refresh || (cancelled && cancelled()); };
  QString activeCode;
  for (const auto &entry : entries) {
    if (stopped()) return "cancelled";
    if (entry->identity.compatibility != environmentCompatibility()) continue;
    QString previousCode;
    { QMutexLocker lock(&m_mutex); previousCode = entry->integrityCode; }
    if (!verifyCached && previousCode.isEmpty() && entry->runtime->snapshot().ready()) continue;
    const auto result = entry->runtime->inspectIntegrity(stopped);
    const auto runtimeCode = result.ready() ? QString() : result.code;
    auto code = runtimeCode.isEmpty() ? environment_config::verifyScripts(entry->identity, stopped) : runtimeCode;
    { QMutexLocker lock(&m_mutex); if (m_refresh.load() == refresh && m_entries.value(entry->identity.id) == entry) {
      entry->runtimeIntegrityCode = runtimeCode; entry->integrityCode = code;
    } }
    if (entry == entries.first()) activeCode = code;
  }
  return activeCode;
}
EnvironmentConfigurationResult PythonEnvironmentRegistry::applyMutation(EnvironmentConfigurationResult result, bool explicitActivation,
    quint64 authorizationEpoch) {
  LegacyPythonEnvironment legacy;
  { QMutexLocker lock(&m_mutex); legacy = m_legacy; }
  // A directory-sync error may follow the committed rename. Reload that fact;
  // never retain an earlier enabled snapshot merely because fsync was uncertain.
  if (result.ok() || result.code == "python_environment_config_commit_uncertain") {
    auto loaded = m_store.load();
    if (result.ok() && !loaded.ok()) result.code = loaded.code;
    if (result.ok()) { loaded.verifiedRuntime = result.verifiedRuntime; loaded.verifiedIdentityId = result.verifiedIdentityId; }
    if (result.ok() && explicitActivation) {
      QMutexLocker lock(&m_mutex);
      if (m_authorizationEpoch.load() == authorizationEpoch) {
        std::atomic_store(&m_globalInterruption, std::make_shared<std::atomic_bool>(false));
        m_deniedIds.remove(result.verifiedIdentityId);
      } else result.code = "python_environment_activation_interrupted";
    }
    apply(loaded, "host-config", legacy, beginRefresh());
  }
  return result;
}
EnvironmentConfigurationResult PythonEnvironmentRegistry::activate(const EnvironmentCandidate &candidate, quint64 revision,
    const std::function<bool()> &cancelled) {
  const auto epoch = m_authorizationEpoch.load();
  const auto stopped = [&] { const bool userStopped = cancelled && cancelled(); return userStopped || m_authorizationEpoch.load() != epoch; };
  return applyMutation(m_store.activate(candidate, revision, stopped), true, epoch);
}
EnvironmentConfigurationResult PythonEnvironmentRegistry::disable(quint64 revision) {
  return applyMutation(m_store.disable(revision));
}
EnvironmentConfigurationResult PythonEnvironmentRegistry::revoke(const QString &id, quint64 revision) {
  return applyMutation(m_store.revoke(id, revision));
}
EnvironmentConfigurationResult PythonEnvironmentRegistry::rollback(const QString &id, quint64 revision,
    const std::function<bool()> &cancelled) {
  const auto epoch = m_authorizationEpoch.load();
  const auto stopped = [&] { const bool userStopped = cancelled && cancelled(); return userStopped || m_authorizationEpoch.load() != epoch; };
  return applyMutation(m_store.rollback(id, revision, stopped));
}
EnvironmentSelection PythonEnvironmentRegistry::selected(const std::shared_ptr<Entry> &entry, EnvironmentMode mode, bool requireTemplates) const {
  EnvironmentSelection out;
  if (!entry) { out.code = m_code.isEmpty() ? QStringLiteral("python_environment_identity_unapproved") : m_code; return out; }
  out.identity = entry->identity; out.runtime = entry->runtime; out.trusted = entry->trusted; out.sandbox = entry->sandbox;
  out.globalInterruption = std::atomic_load(&m_globalInterruption);
  out.interrupted = mode == EnvironmentMode::Trusted ? entry->trustedInterrupted : entry->generatedInterrupted;
  if (!permitted(entry->identity, mode) || !out.interrupted || out.interrupted->load())
    out.code = m_configuration.revoked.contains(entry->identity.id) ? "python_environment_identity_revoked" : "python_execution_denied";
  // A restored operation owns its compiled-SHA code in its frozen InputBundle;
  // it does not depend on the original delivery directory remaining installed.
  if (out.code.isEmpty()) out.code = requireTemplates && mode == EnvironmentMode::Trusted ? entry->integrityCode : entry->runtimeIntegrityCode;
  return out;
}
EnvironmentSelection PythonEnvironmentRegistry::active(EnvironmentMode mode) const {
  if (!m_mutex.tryLock()) { EnvironmentSelection out; out.code = "python_environment_config_pending"; return out; }
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  return selected(m_entries.value(m_configuration.activeId), mode);
}
EnvironmentSelection PythonEnvironmentRegistry::resolve(const QString &id, const QString &runtimeSha,
    const QJsonObject &policy, EnvironmentMode mode) const {
  if (!m_mutex.tryLock()) { EnvironmentSelection out; out.code = "python_environment_config_pending"; return out; }
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  auto matching = [&](const std::shared_ptr<Entry> &entry) {
    return entry && entry->identity.runtimeSha256 == runtimeSha && entry->identity.policy.json() == policy;
  };
  if (!id.isEmpty()) {
    auto out = selected(m_entries.value(id), mode, false);
    if (out.code.isEmpty() && !matching(m_entries.value(id))) out.code = "python_environment_identity_mismatch";
    return out;
  }
  // Compatibility for pre-E2 operations: only known host identities are searched,
  // never a root supplied by the record. Prefer an equivalent active identity.
  const auto current = m_entries.value(m_configuration.activeId);
  if (matching(current) && permitted(current->identity, mode)) return selected(current, mode, false);
  for (const auto &entry : m_entries)
    if (matching(entry) && permitted(entry->identity, mode)) return selected(entry, mode, false);
  EnvironmentSelection out; out.code = "python_environment_identity_unapproved"; return out;
}
QJsonObject PythonEnvironmentRegistry::configurationSnapshot() const {
  if (!m_mutex.tryLock()) return {{"code", "python_environment_config_pending"}};
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  auto object = m_configuration.json(); object.insert("source", m_source);
  object.insert("code", m_code.isEmpty() ? QStringLiteral("ok") : m_code); return object;
}
QJsonObject PythonEnvironmentRegistry::status() const {
  if (!m_mutex.tryLock()) return {{"code", "python_environment_config_pending"}, {"allowTrusted", false}, {"allowGenerated", false}};
  const auto unlock = qScopeGuard([this] { m_mutex.unlock(); });
  const auto entry = m_entries.value(m_configuration.activeId);
  QJsonObject out{{"source", m_source}, {"revision", QString::number(m_configuration.revision)},
      {"activeIdentityId", m_configuration.activeId}, {"enabled", m_configuration.enabled},
      {"approvedCount", m_configuration.approved.size()}, {"code", m_code.isEmpty() ? QStringLiteral("ok") : m_code},
      {"allowTrusted", entry && permitted(entry->identity, EnvironmentMode::Trusted)},
      {"allowGenerated", entry && permitted(entry->identity, EnvironmentMode::Generated)},
      {"emergencyDenied", std::atomic_load(&m_globalInterruption)->load()}, {"memoryDeniedCount", m_deniedIds.size()},
      {"inMemoryDeny", std::atomic_load(&m_globalInterruption)->load() || m_deniedIds.contains(m_configuration.activeId)},
      {"permissionCeiling", m_configuration.permissionCeiling.json()},
      {"legacyEnvironmentIgnored", m_source == "host-config" && !m_legacy.isEmpty()},
      {"legacyDenyTrusted", m_legacy.value("PALEO_AGENT_PYTHON_ENABLE_TRUSTED") == "0"},
      {"legacyDenyGenerated", m_legacy.value("PALEO_AGENT_PYTHON_ENABLE_GENERATED") == "0"}};
  if (entry) {
    out.insert("legacyTemplates", entry->identity.legacyTemplates);
    out.insert("compatible", entry->identity.compatibility == environmentCompatibility());
    out.insert("runtimeIntegrityCode", entry->runtimeIntegrityCode.isEmpty() ? QStringLiteral("ok") : entry->runtimeIntegrityCode);
    out.insert("integrityCode", entry->integrityCode.isEmpty() ? QStringLiteral("ok") : entry->integrityCode);
  }
  return out;
}
QJsonObject PythonEnvironmentRegistry::probe(const QString &id, EnvironmentMode mode, const std::function<bool()> &cancelled) {
  EnvironmentSelection selection;
  { QMutexLocker lock(&m_mutex); selection = selected(m_entries.value(id), mode, false); }
  if (!selection.ok()) return {{"ready", false}, {"code", selection.code}};
  const auto stopped = [selection, cancelled] { return selection.interrupted->load() || selection.globalInterruption->load() || (cancelled && cancelled()); };
  if (mode == EnvironmentMode::Generated) return selection.sandbox->inspect(stopped);
  return selection.trusted->inspect(stopped);
}
} // namespace paleo::python
