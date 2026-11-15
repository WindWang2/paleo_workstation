// 层：数据
#pragma once
#include "pythonruntime.h"
#include <QMap>
#include <QSet>
#include <QVector>

namespace paleo::python {
inline constexpr char EnvironmentConfigSchema[] = "paleo.python-environment-config.v1";
struct EnvironmentIdentity {
  QString id, runtimeRoot, runtimeSha256, scriptsRoot, scriptsSha256;
  QJsonObject compatibility;
  ExecutionPolicy policy;
  bool legacyTemplates = false;
  QJsonObject json() const;
};
struct EnvironmentConfiguration {
  bool exists = false, enabled = false;
  quint64 revision = 0;
  QString activeId;
  ExecutionPolicy permissionCeiling;
  QVector<EnvironmentIdentity> approved;
  QSet<QString> revoked;
  QJsonObject json() const;
};
struct EnvironmentConfigurationResult {
  QString code;
  EnvironmentConfiguration configuration;
  std::shared_ptr<RuntimeProvider> verifiedRuntime;
  QString verifiedIdentityId;
  bool ok() const { return code == QLatin1String("ok"); }
};
struct EnvironmentCandidate {
  QString runtimeRoot, runtimeSha256, scriptsRoot, scriptsSha256;
  ExecutionPolicy policy;
};
// Only the five named legacy host variables are captured. No project/model data
// or provider credentials enter this configuration source.
using LegacyPythonEnvironment = QMap<QString, QString>;
LegacyPythonEnvironment captureLegacyPythonEnvironment();
QString environmentConfigPath();
QJsonObject environmentCompatibility();
bool environmentPolicy(const QJsonObject &, ExecutionPolicy &);
QString verifyEnvironmentIdentity(EnvironmentIdentity &, std::shared_ptr<RuntimeProvider> &,
    const std::function<bool()> &cancelled = {});
EnvironmentConfigurationResult legacyEnvironment(const LegacyPythonEnvironment &,
    const std::function<bool()> &cancelled = {});

// Synchronous, QtCore-only host storage. Mutators/verification run through the
// caller's existing task service; paths/anchors are never accepted from a tool.
class PythonEnvironmentStore {
public:
  explicit PythonEnvironmentStore(QString path = environmentConfigPath());
  QString path() const { return m_path; }
  EnvironmentConfigurationResult load() const;
  EnvironmentConfigurationResult activate(const EnvironmentCandidate &, quint64 expectedRevision,
      const std::function<bool()> &cancelled = {}) const;
  EnvironmentConfigurationResult disable(quint64 expectedRevision) const;
  EnvironmentConfigurationResult revoke(const QString &identityId, quint64 expectedRevision) const;
  EnvironmentConfigurationResult rollback(const QString &identityId, quint64 expectedRevision,
      const std::function<bool()> &cancelled = {}) const;
private:
  EnvironmentConfigurationResult save(EnvironmentConfiguration, quint64 expectedRevision) const;
  QString m_path;
};
} // namespace paleo::python
