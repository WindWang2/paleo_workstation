// 层：组装根
#include "services/pythondelivery.h"
#include "services/pythonenvironmentregistry.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

namespace {
bool localPackageArgument(const QString &path) {
#ifdef Q_OS_WIN
  if (path.startsWith('/') || path.startsWith('\\')) return false;
  if (path.contains(':'))
    return path.size() >= 3 && path.at(0).isLetter() && path.at(1) == ':' &&
        (path.at(2) == '/' || path.at(2) == '\\') && !path.mid(2).contains(':');
#else
  Q_UNUSED(path)
#endif
  return true;
}
}
// Explicit deployment entry point, never registered in the Agent tool registry.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("paleo_python_environment");
  QCommandLineParser parser;
  parser.setApplicationDescription("Install or configure a host-approved optional Python environment; no interpreter is executed.");
  parser.addHelpOption();
  parser.addOption({"package", "Prepared PALEOPY1 package", "path"});
  parser.addOption({"sha256", "Independent host-approved package SHA256", "sha256"});
  parser.addOption({"store", "Absolute dedicated runtime or scripts store", "path"});
  parser.addOption({"configuration", "Absolute host configuration file (default: product user configuration)", "path"});
  parser.addOption({"activate", "Verify and activate a complete host-approved identity"});
  parser.addOption({"disable", "Persistently disable all Python execution"});
  parser.addOption({"revoke", "Revoke an approved identity without deleting its files", "identity"});
  parser.addOption({"rollback", "Select a compatible approved identity without restoring wider permissions", "identity"});
  parser.addOption({"status", "Report host configuration and legacy deny sources; no execution probe"});
  parser.addOption({"runtime-root", "Host-approved runtime generation root", "path"});
  parser.addOption({"runtime-sha256", "Independent runtime manifest SHA256", "sha256"});
  parser.addOption({"scripts-root", "Host-approved scripts generation root (required for trusted)", "path"});
  parser.addOption({"scripts-sha256", "Independent scripts manifest SHA256 (required for trusted)", "sha256"});
  parser.addOption({"trusted", "Explicitly grant trusted fixed-template execution"});
  parser.addOption({"generated", "Explicitly grant generated execution; actual strong probe remains required"});
  parser.addOption({"expected-revision", "CAS revision from the last configuration snapshot", "revision"});
  parser.process(app);
  const int operations = int(parser.isSet("package")) + int(parser.isSet("activate")) + int(parser.isSet("disable")) +
      int(parser.isSet("revoke")) + int(parser.isSet("rollback")) + int(parser.isSet("status"));
  if (operations != 1) parser.showHelp(2);
  if (!parser.isSet("package")) {
    const auto path = parser.isSet("configuration") ? parser.value("configuration") : paleo::python::environmentConfigPath();
    auto registry = std::make_shared<paleo::python::PythonEnvironmentRegistry>(path);
    const auto legacy = paleo::python::captureLegacyPythonEnvironment();
    registry->loadConfiguration(legacy, registry->beginRefresh());
    const auto before = registry->configurationSnapshot();
    bool revisionOk = false;
    const auto revision = (parser.isSet("expected-revision") ? parser.value("expected-revision") : before.value("revision").toString()).toULongLong(&revisionOk);
    paleo::python::EnvironmentConfigurationResult configured;
    if (parser.isSet("status")) configured.code = before.value("code").toString();
    else if (!revisionOk) configured.code = "python_environment_config_changed";
    else if (parser.isSet("disable")) { registry->denyInMemory(); configured = registry->disable(revision); }
    else if (parser.isSet("revoke")) { registry->denyInMemory(parser.value("revoke")); configured = registry->revoke(parser.value("revoke"), revision); }
    else if (parser.isSet("rollback")) configured = registry->rollback(parser.value("rollback"), revision);
    else {
      paleo::python::EnvironmentCandidate candidate;
      candidate.runtimeRoot = parser.value("runtime-root"); candidate.runtimeSha256 = parser.value("runtime-sha256");
      candidate.scriptsRoot = parser.value("scripts-root"); candidate.scriptsSha256 = parser.value("scripts-sha256");
      candidate.policy.allowTrusted = parser.isSet("trusted"); candidate.policy.allowGenerated = parser.isSet("generated");
      configured = registry->activate(candidate, revision);
    }
    const QJsonObject object{{"code", configured.code}, {"configuration", registry->configurationSnapshot()},
        {"status", registry->status()}, {"executionProbed", false}, {"redistributionAuthorized", false}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    return configured.ok() ? 0 : 1;
  }
  if (!parser.isSet("sha256") || !parser.isSet("store")) parser.showHelp(2);
  paleo::python::DeliveryResult result;
  const auto package = parser.value("package");
  if (localPackageArgument(package))
    result = paleo::python::installDelivery({QFileInfo(package).absoluteFilePath(), parser.value("sha256"), parser.value("store")});
  const QJsonObject output{{"code", result.code}, {"installed", result.ready()}, {"kind", result.kind},
      {"directory", result.root}, {"manifestSha256", result.manifestSha256}, {"licenseStatus", result.licenseStatus},
      {"alreadyInstalled", result.alreadyInstalled}, {"executionEnabled", false}, {"redistributionAuthorized", false}};
  const auto bytes = QJsonDocument(output).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
  return result.ready() ? 0 : 1;
}
