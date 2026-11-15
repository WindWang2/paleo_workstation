// 层：功能
#include "pythonenvironmentcontroller.h"
#include "services/pythonenvironmentregistry.h"
#include <QJsonDocument>
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
QString executionState(bool allowed, const QJsonObject &probe, bool trusted, bool blocked) {
  if (!allowed) return "disabled";
  if (blocked) return "unavailable";
  if (probe.isEmpty()) return "not_checked";
  return probe.value(trusted ? "executionReady" : "ready").toBool() ? "ready" : "unavailable";
}
QString sourceLabel(const QString &source) {
  if (source == "managed") return QObject::tr("宿主持久配置");
  if (source == "legacy") return QObject::tr("兼容环境变量");
  if (source == "pending") return QObject::tr("正在载入");
  return QObject::tr("尚未配置");
}
QJsonObject safeStatus(const QJsonObject &input, int depth = 0) {
  const QSet<QString> scalarKeys{
    "code", "ready", "executionReady", "enabled", "source", "revision", "activeIdentityId", "approvedCount",
    "allowTrusted", "allowGenerated", "compatible", "legacyTemplates", "integrityCode", "emergencyDenied", "inMemoryDeny",
    "runtimeVersion", "pythonVersion", "numpyVersion", "platform", "profile", "backend", "policyId", "id",
    "cpuSeconds", "wallSeconds", "memoryBytes", "inputBytes", "outputBytes", "codeBytes", "diskBytes", "logBytes",
    "threads", "concurrent", "threadCountEnforced", "threadLimitEnforced", "threadsEnforced", "cpuLimitEnforced",
    "memoryLimitEnforced", "wallLimitEnforced", "pidsLimitEnforced", "network", "filesystem", "subprocess", "cpu", "memory", "wall"};
  const QSet<QString> objectKeys{"limits", "enforced", "enforcedLimits", "resourceLimits", "isolation", "runtime", "policy", "backend"};
  static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9_.:+-]{1,192}$"));
  QJsonObject output;
  for (auto it = input.begin(); it != input.end(); ++it) {
    if (objectKeys.contains(it.key()) && it.value().isObject() && depth < 3) output.insert(it.key(), safeStatus(it.value().toObject(), depth + 1));
    else if (scalarKeys.contains(it.key())) {
      if (it.value().isBool() || it.value().isDouble()) output.insert(it.key(), it.value());
      else if (it.value().isString() && token.match(it.value().toString()).hasMatch()) output.insert(it.key(), it.value());
    }
  }
  return output;
}
QString guidance(const QJsonObject &status, const QJsonObject &trusted, const QJsonObject &generated) {
  const auto integrity = status.value("integrityCode").toString();
  if (integrity != "ok" && !integrity.isEmpty()) return QObject::tr("请核对运行时与清单校验码；完整性未通过时不能执行。");
  if (status.value("allowGenerated").toBool() && !generated.isEmpty() && !generated.value("ready").toBool())
    return QObject::tr("生成计算当前不可执行。请检查诊断中的平台隔离与资源条件；不会回退到普通进程。");
  if (status.value("allowTrusted").toBool() && !trusted.isEmpty() && !trusted.value("executionReady").toBool())
    return QObject::tr("可信分析当前不可执行。请核对模板与平台执行后端。");
  if (!status.value("enabled").toBool()) return QObject::tr("安装、启用与可执行状态分别检查；启用前不会运行分析。");
  return {};
}
}
void PythonEnvironmentController::publishSnapshot(bool replaceForm) {
  if (!m_registry) return;
  const auto configuration = m_registry->configurationSnapshot();
  const auto status = m_registry->status();
  const auto identity = activeIdentity(configuration);
  if (m_probeIdentity != status.value("activeIdentityId").toString() || m_probeRevision != status.value("revision").toString()) {
    m_trustedProbe = {}; m_generatedProbe = {};
  }
  const auto integrityCode = status.value("integrityCode").toString();
  const auto statusCode = status.value("code").toString();
  const bool blocked = (statusCode != "ok" && !statusCode.isEmpty()) || integrityCode != "ok";
  const auto id = status.value("activeIdentityId").toString();
  const auto installed = id.isEmpty() ? "unconfigured" : integrityCode == "python_environment_missing" ? "missing" : "installed";
  const auto observedIntegrity = statusCode == "ok" ? integrityCode : statusCode;
  const auto integrity = observedIntegrity == "ok" ? "verified" : observedIntegrity.isEmpty() || observedIntegrity.contains("pending") || id.isEmpty() ? "not_checked" : "error";
  QJsonObject projection{{"installed", installed}, {"integrity", integrity}, {"sourceLabel", sourceLabel(status.value("source").toString())},
      {"trusted", executionState(status.value("allowTrusted").toBool(), m_trustedProbe, true, blocked)},
      {"generated", executionState(status.value("allowGenerated").toBool(), m_generatedProbe, false, blocked)},
      {"message", guidance(status, m_trustedProbe, m_generatedProbe)}};
  const QPointer<PythonEnvironmentController> guard(this); const auto generation = m_generation;
  if (replaceForm) {
    bool valid = false; const auto revision = configuration.value("revision").toString().toULongLong(&valid);
    if (valid) {
      m_formRevision = revision; m_formLoaded = true;
      const auto runtime = identity.value("runtime").toObject(), scripts = identity.value("scripts").toObject();
      const auto ceiling = configuration.value("permissionCeiling").toObject();
      const auto policy = ceiling.isEmpty() ? identity.value("policy").toObject() : ceiling;
      m_formPolicy = policy;
      QJsonObject form{{"runtimeRoot", runtime.value("root")}, {"runtimeManifestSha256", runtime.value("manifestSha256")},
        {"scriptsRoot", scripts.value("root")}, {"scriptsManifestSha256", scripts.value("manifestSha256")},
        {"allowTrusted", status.value("enabled").toBool() && policy.value("allowTrusted").toBool()},
        {"allowGenerated", status.value("enabled").toBool() && policy.value("allowGenerated").toBool()}};
      QJsonArray history;
      const auto revoked = configuration.value("revoked").toArray();
      for (const auto &row : configuration.value("approved").toArray()) {
        const auto approved = row.toObject(); const auto approvedId = approved.value("id").toString();
        if (!revoked.contains(approvedId)) history.append(QJsonObject{{"id", approvedId}, {"label", approvedId.left(12)}});
      }
      emit configurationChanged(form, history);
      if (!guard || generation != m_generation) return;
    }
  }
  emit diagnosticsChanged(projection);
}
QJsonObject PythonEnvironmentController::redactedDiagnostics() const {
  const auto status = m_registry ? m_registry->status() : QJsonObject{{"code", "python_environment_service_unavailable"}};
  const bool same = m_probeIdentity == status.value("activeIdentityId").toString() && m_probeRevision == status.value("revision").toString();
  return {{"schema", "paleo.python-environment-diagnostics.v1"}, {"status", safeStatus(status)},
          {"trusted", same ? safeStatus(m_trustedProbe) : QJsonObject{}},
          {"generated", same ? safeStatus(m_generatedProbe) : QJsonObject{}},
          {"requestedPolicy", safeStatus(activeIdentity(m_registry ? m_registry->configurationSnapshot() : QJsonObject{}).value("policy").toObject())}};
}
