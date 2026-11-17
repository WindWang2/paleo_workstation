// 层：功能
#include "pythonenvironmentcontroller.h"
#include "services/pythonenvironmentregistry.h"
#include <QRegularExpression>
#include <QSet>

namespace {
QJsonObject activeIdentity(const QJsonObject &configuration) {
  const auto id = configuration.value("activeId").toString();
  for (const auto &value : configuration.value("approved").toArray()) {
    const auto row = value.toObject();
    if (row.value("id").toString() == id) return row.value("identity").toObject();
  }
  return {};
}
bool sameConfiguration(const QJsonObject &configuration, const QJsonObject &status) {
  return configuration.value("revision").isString() && configuration.value("revision") == status.value("revision") &&
      configuration.value("activeId") == status.value("activeIdentityId") && configuration.value("source") == status.value("source");
}
QString executionState(bool allowed, const QJsonObject &probe, bool trusted, bool blocked) {
  if (!allowed) return "disabled";
  if (blocked) return "unavailable";
  if (probe.isEmpty()) return "not_checked";
  return probe.value(trusted ? "executionReady" : "ready").toBool() ? "ready" : "unavailable";
}
QString sourceLabel(const QString &source) {
  if (source == "host-config") return QObject::tr("宿主持久配置");
  if (source == "legacy-env") return QObject::tr("兼容环境变量");
  if (source == "pending") return QObject::tr("正在载入");
  return QObject::tr("尚未配置");
}
QJsonObject safeStatus(const QJsonObject &input, int depth = 0) {
  const QSet<QString> scalarKeys{
    "code", "ready", "executionReady", "enabled", "source", "revision", "activeIdentityId", "approvedCount",
    "allowTrusted", "allowGenerated", "compatible", "legacyTemplates", "integrityCode", "runtimeIntegrityCode",
    "emergencyDenied", "inMemoryDeny", "memoryDeniedCount", "legacyEnvironmentIgnored", "legacyDenyTrusted", "legacyDenyGenerated",
    "runtimeVersion", "pythonVersion", "numpyVersion", "platform", "profile", "backend", "policyId", "id",
    "cpuSeconds", "wallSeconds", "memoryBytes", "inputBytes", "outputBytes", "codeBytes", "diskBytes", "logBytes",
    "threads", "concurrent", "threadCountEnforced", "threadLimitEnforced", "threadsEnforced", "cpuLimitEnforced",
    "memoryLimitEnforced", "wallLimitEnforced", "pidsLimitEnforced", "network", "filesystem", "subprocess", "cpu", "memory", "wall",
    "processDiskWrites", "registryWrites", "maxProcesses", "memoryAccounting", "cpuAccounting", "cpuPollingMilliseconds",
    "cpuParallelismLimit", "checksAtStart", "requiresRuntimeProbe", "runtimeProbeVerified", "probeContract", "probeCodeSha256",
    "runtimeManifestSha256", "probeElapsedMilliseconds", "cpuMicros", "observedPeakMemoryBytes", "memoryPeakComplete",
    "hostSnapshotReservedBytes", "childMemoryLimitBytes", "osBuild"};
  const QSet<QString> objectKeys{"limits", "enforced", "enforcedLimits", "resourceLimits", "isolation", "runtime", "policy", "permissionCeiling"};
  static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_.+-]{1,192}$"));
  QJsonObject output;
  for (auto it = input.begin(); it != input.end(); ++it) {
    if (objectKeys.contains(it.key()) && it.value().isObject() && depth < 3) output.insert(it.key(), safeStatus(it.value().toObject(), depth + 1));
    else if (scalarKeys.contains(it.key())) {
      if (it.value().isBool() || it.value().isDouble()) output.insert(it.key(), it.value());
      else if (it.value().isString() && token.match(it.value().toString()).hasMatch()) {
        const auto value = it.value().toString();
        if ((it.key() == "id" || it.key() == "policyId") && value != paleo::python::ExecutionPolicy{}.id) continue;
        if (it.key() == "source" && !QStringList{"host-config", "legacy-env", "disabled", "pending"}.contains(value)) continue;
        output.insert(it.key(), it.value());
      }
    }
  }
  return output;
}
QString integrityState(const QString &code) {
  if (code == "ok") return "verified";
  return code.isEmpty() || code.contains("pending") || code.contains("verifying") ? "not_checked" : "error";
}
QString guidance(const QJsonObject &configuration, const QJsonObject &status, const QJsonObject &trusted, const QJsonObject &generated) {
  const auto ceiling = configuration.value("permissionCeiling").toObject();
  if (status.value("inMemoryDeny").toBool() && status.value("enabled").toBool() &&
      !configuration.value("revoked").toArray().contains(status.value("activeIdentityId")) &&
      (ceiling.value("allowTrusted").toBool() || ceiling.value("allowGenerated").toBool()))
    return QObject::tr("本次会话已停用相关计算权限，但持久配置仍允许执行。请保存停用或撤销配置。");
  if (status.value("runtimeIntegrityCode") != "ok" && !status.value("activeIdentityId").toString().isEmpty())
    return QObject::tr("请核对运行时与清单校验码；完整性未通过时不能执行。");
  if (status.value("allowTrusted").toBool() && status.value("integrityCode") != "ok")
    return QObject::tr("可信模板完整性未通过；运行时与生成计算状态分别检查。");
  if (status.value("allowGenerated").toBool() && !generated.isEmpty() && !generated.value("ready").toBool())
    return QObject::tr("生成计算当前不可执行。请检查平台隔离与资源条件；不会回退到普通进程。");
  if (status.value("allowTrusted").toBool() && !trusted.isEmpty() && !trusted.value("executionReady").toBool())
    return QObject::tr("可信分析当前不可执行。请核对平台执行后端。");
  if (!status.value("enabled").toBool()) return QObject::tr("安装、启用与可执行状态分别检查；启用前不会运行分析。");
  return {};
}
} // namespace

QJsonObject redactedPythonEnvironmentStatus(const QJsonObject &status) { return safeStatus(status); }

void PythonEnvironmentController::publishSnapshot(bool replaceForm) {
  if (!m_registry) return;
  const auto configuration = m_registry->configurationSnapshot(), status = m_registry->status();
  const bool consistent = sameConfiguration(configuration, status);
  const auto identity = consistent ? activeIdentity(configuration) : QJsonObject{};
  if (m_probeStatus != status) { m_trustedProbe = {}; m_generatedProbe = {}; }
  const auto runtimeCode = status.value("runtimeIntegrityCode").toString();
  const auto fullCode = status.value("integrityCode").toString(), statusCode = status.value("code").toString();
  const bool runtimeBlocked = !consistent || statusCode != "ok" || runtimeCode != "ok";
  const auto id = status.value("activeIdentityId").toString();
  const auto installed = id.isEmpty() ? "unconfigured" : runtimeCode == "ok" ? "installed" :
      runtimeCode == "python_environment_missing" ? "missing" : "not_checked";
  QJsonObject projection{{"installed", installed}, {"integrity", integrityState(statusCode == "ok" ? runtimeCode : statusCode)},
      {"templatesIntegrity", identity.value("scripts").toObject().value("mode") == "none" ?
          QStringLiteral("not_required") : integrityState(fullCode)}, {"sourceLabel", sourceLabel(status.value("source").toString())},
      {"trusted", executionState(status.value("allowTrusted").toBool(), m_trustedProbe, true, runtimeBlocked || fullCode != "ok")},
      {"generated", executionState(status.value("allowGenerated").toBool(), m_generatedProbe, false, runtimeBlocked)},
      {"savedEnabled", status.value("enabled")}, {"inMemoryDeny", status.value("inMemoryDeny")},
      {"message", guidance(configuration, status, m_trustedProbe, m_generatedProbe)}};
  projection.insert("details", redactedDiagnostics());
  const QPointer<PythonEnvironmentController> guard(this); const auto generation = m_generation;
  if (replaceForm && consistent) {
    bool valid = false; const auto revision = configuration.value("revision").toString().toULongLong(&valid);
    if (valid) {
      m_formRevision = revision; m_formLoaded = true;
      const auto runtime = identity.value("runtime").toObject(), scripts = identity.value("scripts").toObject();
      const auto ceiling = configuration.value("permissionCeiling").toObject();
      const auto policy = ceiling.isEmpty() ? identity.value("policy").toObject() : ceiling; m_formPolicy = policy;
      QJsonObject form{{"runtimeRoot", runtime.value("root")}, {"runtimeManifestSha256", runtime.value("manifestSha256")},
        {"scriptsRoot", scripts.value("root")}, {"scriptsManifestSha256", scripts.value("manifestSha256")},
        {"allowTrusted", status.value("enabled").toBool() && policy.value("allowTrusted").toBool()},
        {"allowGenerated", status.value("enabled").toBool() && policy.value("allowGenerated").toBool()}};
      QJsonArray history; const auto revoked = configuration.value("revoked").toArray();
      for (const auto &row : configuration.value("approved").toArray()) {
        const auto approved = row.toObject(); const auto approvedId = approved.value("id").toString();
        if (!revoked.contains(approvedId)) history.append(QJsonObject{{"id", approvedId}, {"label", approvedId.left(12)}});
      }
      emit configurationChanged(form, history); if (!guard || generation != m_generation) return;
    }
  }
  emit diagnosticsChanged(projection);
}
QJsonObject PythonEnvironmentController::redactedDiagnostics() const {
  const auto status = m_registry ? m_registry->status() : QJsonObject{{"code", "python_environment_service_unavailable"}};
  const auto configuration = m_registry ? m_registry->configurationSnapshot() : QJsonObject{};
  const bool same = m_probeStatus == status;
  return {{"schema", "paleo.python-environment-diagnostics.v1"}, {"status", safeStatus(status)},
          {"trusted", same ? safeStatus(m_trustedProbe) : QJsonObject{}}, {"generated", same ? safeStatus(m_generatedProbe) : QJsonObject{}},
          {"requestedPolicy", sameConfiguration(configuration, status) ? safeStatus(activeIdentity(configuration).value("policy").toObject()) : QJsonObject{}}};
}
