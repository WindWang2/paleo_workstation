// 层：数据
#include "pythonenvironmentconfig_internal.h"
#include "pythondelivery_files.h"
#include "pythondelivery_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <limits>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace paleo::python {
namespace {
using namespace environment_config;
bool privateConfiguration(QFile &file) {
#ifndef Q_OS_WIN
  struct stat info{};
  return ::fstat(file.handle(), &info) == 0 && (info.st_uid == ::geteuid() || info.st_uid == 0) && !(info.st_mode & 0022);
#else
  Q_UNUSED(file)
  return true;
#endif
}
EnvironmentConfigurationResult failure(const QString &code) { EnvironmentConfigurationResult out; out.code = code; return out; }
bool writesIntoGeneration(const EnvironmentConfiguration &configuration, const QString &path) {
#ifdef Q_OS_WIN
  constexpr auto comparison = Qt::CaseInsensitive;
#else
  constexpr auto comparison = Qt::CaseSensitive;
#endif
  for (const auto &identity : configuration.approved)
    for (const auto &root : {identity.runtimeRoot, identity.scriptsRoot})
      if (!root.isEmpty() && (path.compare(root, comparison) == 0 || path.startsWith(root + '/', comparison))) return true;
  return false;
}
} // namespace
namespace environment_config {
bool syncConfigurationFile(QFileDevice &file) {
  if (!file.flush()) return false;
#ifdef Q_OS_WIN
  return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != FALSE;
#else
  return ::fsync(file.handle()) == 0;
#endif
}
} // namespace environment_config

PythonEnvironmentStore::PythonEnvironmentStore(QString path) : m_path(delivery::checkedAbsolutePath(path)) {}
EnvironmentConfigurationResult PythonEnvironmentStore::load() const {
  if (!QDir::isAbsolutePath(m_path) || QDir::cleanPath(m_path) != m_path) return failure("python_environment_config_path_invalid");
  const QFileInfo info(m_path);
  if (!info.exists() && !info.isSymLink() && !info.isJunction()) return failure("ok");
  QFile input;
  if (!delivery::openRegular(input, m_path) || !privateConfiguration(input) || input.size() < 1 || input.size() > MaxConfigurationBytes)
    return failure("python_environment_config_invalid");
  const auto bytes = input.read(MaxConfigurationBytes + 1);
  EnvironmentConfigurationResult result;
  if (bytes.size() != input.size() || !parseConfiguration(bytes, result.configuration)) return failure("python_environment_config_invalid");
  result.code = "ok"; return result;
}
EnvironmentConfigurationResult PythonEnvironmentStore::save(EnvironmentConfiguration configuration, quint64 expectedRevision) const {
  if (!QDir::isAbsolutePath(m_path) || QDir::cleanPath(m_path) != m_path || writesIntoGeneration(configuration, m_path) ||
      !delivery::directory(QFileInfo(m_path).absolutePath(), true))
    return failure("python_environment_config_path_invalid");
  delivery::StoreLock lock(m_path + ".lock"); if (!lock.locked()) return failure("python_environment_config_busy");
  auto current = load(); if (!current.ok()) return current;
  if (current.configuration.revision != expectedRevision) return failure("python_environment_config_changed");
  if (expectedRevision == std::numeric_limits<quint64>::max() || configuration.approved.size() > MaxApproved || configuration.revoked.size() > 256)
    return failure("python_environment_history_budget");
  configuration.revision = expectedRevision + 1; configuration.exists = true;
  const auto bytes = canonical(configuration.json());
  if (bytes.size() > MaxConfigurationBytes) return failure("python_environment_config_budget");
  QSaveFile file(m_path); file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(bytes) != bytes.size() || !syncConfigurationFile(file) || !file.commit())
    return failure("python_environment_config_write_failed");
  EnvironmentConfigurationResult result; result.configuration = configuration;
  result.code = delivery::syncDirectory(QFileInfo(m_path).absolutePath()) ? "ok" : "python_environment_config_commit_uncertain";
  return result;
}
EnvironmentConfigurationResult PythonEnvironmentStore::activate(const EnvironmentCandidate &candidate, quint64 expectedRevision,
    const std::function<bool()> &cancelled) const {
  auto previous = load(); if (!previous.ok()) return previous;
  if (previous.configuration.revision != expectedRevision) return failure("python_environment_config_changed");
  EnvironmentIdentity identity; identity.runtimeRoot = candidate.runtimeRoot; identity.runtimeSha256 = candidate.runtimeSha256;
  identity.scriptsRoot = candidate.scriptsRoot; identity.scriptsSha256 = candidate.scriptsSha256;
  identity.policy = candidate.policy; identity.compatibility = environmentCompatibility();
  std::shared_ptr<RuntimeProvider> provider;
  const auto code = verifyEnvironmentIdentity(identity, provider, cancelled); if (!code.isEmpty()) return failure(code);
  auto configuration = previous.configuration; bool known = false;
  for (const auto &approved : configuration.approved) known = known || approved.id == identity.id;
  if (!known) configuration.approved.append(identity);
  configuration.revoked.remove(identity.id); configuration.activeId = identity.id; configuration.enabled = true;
  configuration.permissionCeiling = identity.policy;
  if (cancelled && cancelled()) return failure("cancelled");
  auto result = save(configuration, expectedRevision);
  if (result.ok()) { result.verifiedRuntime = std::move(provider); result.verifiedIdentityId = identity.id; }
  return result;
}
EnvironmentConfigurationResult PythonEnvironmentStore::disable(quint64 expectedRevision) const {
  auto current = load(); if (!current.ok()) return current;
  current.configuration.enabled = false; return save(current.configuration, expectedRevision);
}
EnvironmentConfigurationResult PythonEnvironmentStore::revoke(const QString &id, quint64 expectedRevision) const {
  auto current = load(); if (!current.ok()) return current;
  if (!delivery::digest(id)) return failure("python_environment_identity_unapproved");
  current.configuration.revoked.insert(id); return save(current.configuration, expectedRevision);
}
EnvironmentConfigurationResult PythonEnvironmentStore::rollback(const QString &id, quint64 expectedRevision,
    const std::function<bool()> &cancelled) const {
  const auto current = load(); if (!current.ok()) return current;
  if (current.configuration.revoked.contains(id)) return failure("python_environment_identity_revoked");
  for (const auto &identity : current.configuration.approved)
    if (identity.id == id) {
      auto selected = identity; std::shared_ptr<RuntimeProvider> provider;
      const auto code = verifyEnvironmentIdentity(selected, provider, cancelled);
      if (!code.isEmpty()) return failure(code);
      if (selected.id != id) return failure("python_environment_identity_mismatch");
      auto configuration = current.configuration; configuration.activeId = id;
      if (cancelled && cancelled()) return failure("cancelled");
      auto result = save(configuration, expectedRevision);
      if (result.ok()) { result.verifiedRuntime = provider; result.verifiedIdentityId = id; }
      return result;
    }
  return failure("python_environment_identity_unapproved");
}
} // namespace paleo::python
