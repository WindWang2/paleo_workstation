// 层：数据
#include "pythonenvironmentconfig_internal.h"
#include "pythondelivery_internal.h"
#include "pythondelivery_files.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QPair>
#include <algorithm>
#include <cmath>

namespace paleo::python {
namespace environment_config {
QByteArray canonical(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }
QString identityId(const EnvironmentIdentity &identity) {
  return QString::fromLatin1(QCryptographicHash::hash(canonical(identity.json()), QCryptographicHash::Sha256).toHex());
}
bool parseIdentity(const QJsonObject &object, EnvironmentIdentity &identity) {
  if (object.keys() != QStringList{"compatibility", "policy", "runtime", "scripts"} ||
      !object.value("compatibility").isObject() || !object.value("policy").isObject() ||
      !object.value("runtime").isObject() || !object.value("scripts").isObject()) return false;
  const auto runtime = object.value("runtime").toObject(), scripts = object.value("scripts").toObject();
  if (runtime.keys() != QStringList{"manifestSha256", "root"} || scripts.keys() != QStringList{"manifestSha256", "mode", "root"} ||
      !delivery::digest(runtime.value("manifestSha256").toString()) ||
      !runtime.value("root").isString() || !scripts.value("root").isString() || !scripts.value("manifestSha256").isString() ||
      !environmentPolicy(object.value("policy").toObject(), identity.policy)) return false;
  identity.runtimeRoot = runtime.value("root").toString(); identity.runtimeSha256 = runtime.value("manifestSha256").toString();
  identity.scriptsRoot = scripts.value("root").toString(); identity.scriptsSha256 = scripts.value("manifestSha256").toString();
  identity.compatibility = object.value("compatibility").toObject();
  const bool noScripts = scripts.value("mode") == "none" && identity.scriptsRoot.isEmpty() && identity.scriptsSha256.isEmpty() && !identity.policy.allowTrusted;
  const bool manifestScripts = scripts.value("mode") == "manifest" && QDir::isAbsolutePath(identity.scriptsRoot) && delivery::digest(identity.scriptsSha256);
  if ((!noScripts && !manifestScripts) || identity.runtimeRoot.size() > 4096 || identity.scriptsRoot.size() > 4096 ||
      !QDir::isAbsolutePath(identity.runtimeRoot) ||
      QDir::cleanPath(identity.runtimeRoot) != identity.runtimeRoot ||
      (!identity.scriptsRoot.isEmpty() && QDir::cleanPath(identity.scriptsRoot) != identity.scriptsRoot) ||
      identity.runtimeRoot.contains(QChar(0)) || identity.scriptsRoot.contains(QChar(0)) ||
      canonical(identity.compatibility).size() > 4096) return false;
  identity.id = identityId(identity); return true;
}
bool parseConfiguration(const QByteArray &raw, EnvironmentConfiguration &configuration) {
  const auto parsed = QJsonDocument::fromJson(raw);
  if (!parsed.isObject() || canonical(parsed.object()) != raw) return false;
  const auto object = parsed.object();
  if (object.keys() != QStringList{"activeId", "approved", "enabled", "permissionCeiling", "revision", "revoked", "schema"} ||
      object.value("schema") != QLatin1String(EnvironmentConfigSchema) || !object.value("enabled").isBool() ||
      !object.value("approved").isArray() || !object.value("revoked").isArray() || !object.value("revision").isString() ||
      !object.value("activeId").isString() || !object.value("permissionCeiling").isObject() ||
      !environmentPolicy(object.value("permissionCeiling").toObject(), configuration.permissionCeiling)) return false;
  const auto revision = object.value("revision").toString(); bool ok = false;
  configuration.revision = revision.toULongLong(&ok);
  if (!ok || QString::number(configuration.revision) != revision) return false;
  const auto approved = object.value("approved").toArray(), revoked = object.value("revoked").toArray();
  if (approved.size() > MaxApproved || revoked.size() > 256) return false;
  QSet<QString> ids; QString previous;
  for (const auto &value : approved) {
    const auto row = value.toObject(); EnvironmentIdentity identity;
    if (row.keys() != QStringList{"id", "identity"} || !row.value("identity").isObject() ||
        !parseIdentity(row.value("identity").toObject(), identity) || row.value("id") != identity.id ||
        (!previous.isEmpty() && identity.id <= previous)) return false;
    previous = identity.id; ids.insert(identity.id); configuration.approved.append(identity);
  }
  previous.clear();
  for (const auto &value : revoked) {
    const auto id = value.toString();
    // A revocation tombstone is deny-only. It may also name a legacy identity
    // retained by a running host, without approving that identity after restart.
    if (!delivery::digest(id) || (!previous.isEmpty() && id <= previous)) return false;
    previous = id; configuration.revoked.insert(id);
  }
  configuration.activeId = object.value("activeId").toString(); configuration.enabled = object.value("enabled").toBool();
  if ((!configuration.activeId.isEmpty() && !ids.contains(configuration.activeId)) ||
      (configuration.enabled && configuration.activeId.isEmpty())) return false;
  configuration.exists = true; return true;
}
} // namespace environment_config

QString environmentConfigPath() {
  // A stable product path shared with the deployment CLI, independent of the
  // current executable name. Project roots never participate in this lookup.
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
      .filePath("paleo/paleo_workbench/agent-python.json");
}
LegacyPythonEnvironment captureLegacyPythonEnvironment() {
  LegacyPythonEnvironment out;
  for (const auto name : {"PALEO_AGENT_PYTHON_ROOT", "PALEO_AGENT_PYTHON_MANIFEST_SHA256",
      "PALEO_AGENT_PYTHON_SCRIPTS_ROOT", "PALEO_AGENT_PYTHON_ENABLE_TRUSTED", "PALEO_AGENT_PYTHON_ENABLE_GENERATED"})
    if (qEnvironmentVariableIsSet(name)) out.insert(QLatin1String(name), qEnvironmentVariable(name));
  return out;
}
QJsonObject environmentCompatibility() {
  return {{"contract", "paleo.python-environment-compat.v1"}, {"profile", QLatin1String(ProfileId)},
      {"pythonVersion", QLatin1String(PythonVersion)}, {"numpyVersion", QLatin1String(NumpyVersion)},
      {"sourceRuntimeLockSha256", runtimeSourceLockSha256()}, {"scripts", delivery::compiledScriptHashes()}};
}
bool environmentPolicy(const QJsonObject &object, ExecutionPolicy &policy) {
  if (object.keys() != policy.json().keys() || !object.value("id").isString() ||
      !object.value("allowTrusted").isBool() || !object.value("allowGenerated").isBool()) return false;
  const auto number = [&](const char *key, qint64 &target) {
    const auto value = object.value(QLatin1String(key)).toDouble(-1);
    if (!std::isfinite(value) || value < 1 || value > double(1LL << 40) || value != std::floor(value)) return false;
    target = qint64(value); return true;
  };
  if (!number("codeBytes", policy.codeBytes) || !number("inputBytes", policy.inputBytes) ||
      !number("outputBytes", policy.outputBytes) || !number("logBytes", policy.logBytes) ||
      !number("diskBytes", policy.diskBytes) || !number("memoryBytes", policy.memoryBytes)) return false;
  for (const auto &pair : {qMakePair("cpuSeconds", &policy.cpuSeconds), qMakePair("wallSeconds", &policy.wallSeconds),
                          qMakePair("threads", &policy.threads), qMakePair("concurrent", &policy.concurrent)}) {
    qint64 value = 0; if (!number(pair.first, value) || value > 86400) return false; *pair.second = int(value);
  }
  policy.id = object.value("id").toString(); policy.allowTrusted = object.value("allowTrusted").toBool();
  policy.allowGenerated = object.value("allowGenerated").toBool();
  return !policy.id.isEmpty() && policy.id.size() <= 128;
}
QJsonObject EnvironmentIdentity::json() const {
  return {{"runtime", QJsonObject{{"root", runtimeRoot}, {"manifestSha256", runtimeSha256}}},
      {"scripts", QJsonObject{{"root", scriptsRoot}, {"manifestSha256", scriptsSha256},
                              {"mode", legacyTemplates ? "compiled-legacy" : scriptsRoot.isEmpty() && scriptsSha256.isEmpty() ? "none" : "manifest"}}},
      {"policy", policy.json()}, {"compatibility", compatibility}};
}
QJsonObject EnvironmentConfiguration::json() const {
  auto identities = approved;
  std::sort(identities.begin(), identities.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
  QJsonArray entries, denied;
  for (const auto &identity : identities) entries.append(QJsonObject{{"id", identity.id}, {"identity", identity.json()}});
  auto revokedIds = revoked.values(); std::sort(revokedIds.begin(), revokedIds.end());
  for (const auto &id : revokedIds) denied.append(id);
  return {{"schema", QLatin1String(EnvironmentConfigSchema)}, {"revision", QString::number(revision)},
      {"enabled", enabled}, {"activeId", activeId}, {"approved", entries}, {"revoked", denied},
      {"permissionCeiling", permissionCeiling.json()}};
}
} // namespace paleo::python
