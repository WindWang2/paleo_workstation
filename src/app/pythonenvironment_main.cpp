// 层：组装根
#include "services/pythondelivery.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

// Explicit deployment entry point, never registered in the Agent tool registry.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("paleo_python_environment");
  QCommandLineParser parser;
  parser.setApplicationDescription("Install a host-approved offline Python generation; execution stays disabled.");
  parser.addHelpOption();
  parser.addOption({"package", "Prepared PALEOPY1 package", "path"});
  parser.addOption({"sha256", "Independent host-approved package SHA256", "sha256"});
  parser.addOption({"store", "Absolute dedicated runtime or scripts store", "path"});
  parser.process(app);
  if (!parser.isSet("package") || !parser.isSet("sha256") || !parser.isSet("store")) parser.showHelp(2);
  const auto result = paleo::python::installDelivery({QFileInfo(parser.value("package")).absoluteFilePath(),
      parser.value("sha256"), parser.value("store")});
  const QJsonObject output{{"code", result.code}, {"installed", result.ready()}, {"kind", result.kind},
      {"directory", result.root}, {"manifestSha256", result.manifestSha256}, {"licenseStatus", result.licenseStatus},
      {"alreadyInstalled", result.alreadyInstalled}, {"executionEnabled", false}, {"redistributionAuthorized", false}};
  const auto bytes = QJsonDocument(output).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
  return result.ready() ? 0 : 1;
}
