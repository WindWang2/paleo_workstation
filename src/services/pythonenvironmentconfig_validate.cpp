// 层：数据
#include "pythonenvironmentconfig_internal.h"
#include "pythondelivery_internal.h"
#include "pythondelivery_files.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace paleo::python {
namespace environment_config {
QString verifyScripts(const EnvironmentIdentity &identity, const std::function<bool()> &cancelled) {
  if (!identity.policy.allowTrusted && identity.scriptsRoot.isEmpty() && identity.scriptsSha256.isEmpty()) return {};
  if (identity.legacyTemplates) {
    if (!identity.policy.allowTrusted) return {};
    if (!delivery::directory(identity.scriptsRoot, false)) return "python_template_package_missing";
    const auto hashes = delivery::compiledScriptHashes();
    for (auto it = hashes.begin(); it != hashes.end(); ++it) {
      if (cancelled && cancelled()) return "cancelled";
      QFile file;
      if (!delivery::openRegular(file, QDir(identity.scriptsRoot).filePath(it.key() + ".py")) ||
          file.size() < 1 || file.size() > 256 * 1024) return "python_template_unavailable";
      const auto bytes = file.read(256 * 1024 + 1);
      if (bytes.size() != file.size() || QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != it.value())
        return "python_template_changed";
    }
    return {};
  }
  QFile file;
  if (!delivery::digest(identity.scriptsSha256) ||
      !delivery::openRegular(file, QDir(identity.scriptsRoot).filePath("scripts-manifest.json")) ||
      file.size() < 1 || file.size() > 64 * 1024) return "python_template_manifest_invalid";
  const auto bytes = file.read(64 * 1024 + 1);
  if (bytes.size() != file.size() || QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != identity.scriptsSha256)
    return "python_template_changed";
  const auto manifest = QJsonDocument::fromJson(bytes).object();
  QJsonArray entries;
  auto rows = manifest.value("scripts").toArray();
  for (const auto &row : manifest.value("licenses").toArray()) rows.append(row);
  for (const auto &value : rows) {
    const auto row = value.toObject();
    entries.append(QJsonObject{{"path", row.value("path")}, {"size", row.value("size")},
                               {"sha256", row.value("sha256")}, {"executable", false}});
  }
  entries.append(QJsonObject{{"path", "scripts-manifest.json"}, {"size", bytes.size()},
                             {"sha256", identity.scriptsSha256}, {"executable", false}});
  QVector<QJsonObject> sorted;
  for (const auto &entry : entries) sorted.append(entry.toObject());
  std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
    return a.value("path").toString().toUtf8() < b.value("path").toString().toUtf8();
  });
  entries = {}; for (const auto &entry : sorted) entries.append(entry);
  const QJsonObject header{{"schema", "paleo.python-delivery.v1"}, {"kind", "scripts"}, {"platform", "all"},
      {"manifestSha256", identity.scriptsSha256}, {"licenseStatus", manifest.value("licenseStatus")}, {"entries", entries}};
  delivery::Header parsed;
  if (!delivery::parseHeader(canonical(header), parsed)) return "python_template_manifest_invalid";
  return delivery::verifyInstalled(parsed, identity.scriptsRoot, cancelled);
}
} // namespace environment_config

QString verifyEnvironmentIdentity(EnvironmentIdentity &identity, std::shared_ptr<RuntimeProvider> &provider,
                                  const std::function<bool()> &cancelled) {
  if (identity.legacyTemplates || identity.compatibility != environmentCompatibility()) return "python_environment_incompatible";
  if (!identity.policy.validate().isEmpty()) return "python_policy_invalid";
  const bool noScripts = !identity.policy.allowTrusted && identity.scriptsRoot.isEmpty() && identity.scriptsSha256.isEmpty();
  if (!delivery::digest(identity.runtimeSha256) || (!noScripts && !delivery::digest(identity.scriptsSha256))) return "python_manifest_anchor_required";
  if (!delivery::directory(identity.runtimeRoot, false) || (!noScripts && !delivery::directory(identity.scriptsRoot, false)))
    return "python_environment_missing";
  identity.runtimeRoot = QFileInfo(identity.runtimeRoot).canonicalFilePath();
  if (!noScripts) identity.scriptsRoot = QFileInfo(identity.scriptsRoot).canonicalFilePath();
  provider = std::make_shared<RuntimeProvider>(RuntimeConfig{identity.runtimeRoot, identity.runtimeSha256, true}, identity.policy);
  const auto runtime = provider->inspectIntegrity(cancelled); if (!runtime.ready()) return runtime.code;
  const auto code = environment_config::verifyScripts(identity, cancelled); if (!code.isEmpty()) return code;
  if (cancelled && cancelled()) return "cancelled";
  identity.id = environment_config::identityId(identity); return {};
}
EnvironmentConfigurationResult legacyEnvironment(const LegacyPythonEnvironment &environment,
    const std::function<bool()> &cancelled) {
  Q_UNUSED(cancelled)
  EnvironmentConfigurationResult result; result.code = "ok";
  const auto root = environment.value("PALEO_AGENT_PYTHON_ROOT");
  const bool trusted = environment.value("PALEO_AGENT_PYTHON_ENABLE_TRUSTED") == "1";
  const bool generated = environment.value("PALEO_AGENT_PYTHON_ENABLE_GENERATED") == "1";
  if (root.isEmpty() && !trusted && !generated) return result;
  EnvironmentIdentity identity; identity.runtimeRoot = delivery::checkedAbsolutePath(root);
  identity.runtimeSha256 = environment.value("PALEO_AGENT_PYTHON_MANIFEST_SHA256");
  identity.scriptsRoot = delivery::checkedAbsolutePath(environment.value("PALEO_AGENT_PYTHON_SCRIPTS_ROOT"));
  identity.legacyTemplates = true; identity.compatibility = environmentCompatibility();
  identity.policy.allowTrusted = trusted; identity.policy.allowGenerated = generated;
  if (!delivery::digest(identity.runtimeSha256) || !QDir::isAbsolutePath(identity.runtimeRoot)) {
    result.code = "python_environment_legacy_invalid"; return result;
  }
  identity.id = environment_config::identityId(identity);
  result.configuration.activeId = identity.id; result.configuration.approved.append(identity);
  result.configuration.permissionCeiling = identity.policy;
  result.configuration.enabled = !root.isEmpty(); return result;
}
} // namespace paleo::python
