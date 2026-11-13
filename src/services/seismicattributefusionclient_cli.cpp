// 层：数据
#include "seismicattributefusionclient_internal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace paleo::fusion_detail {
namespace {
QString executable(const QString &program)
{
  if (program.isEmpty()) return {};
  const auto path = QStandardPaths::findExecutable(program);
  return path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
}

void drain(QProcess &process, QByteArray &pending, QByteArray &tail, const Progress &progress, bool final)
{
  const auto chunk = process.readAllStandardOutput();
  tail = (tail + chunk + process.readAllStandardError()).right(4096);
  pending += chunk;
  while (pending.contains('\n') || (final && !pending.isEmpty())) {
    const qsizetype end = pending.contains('\n') ? pending.indexOf('\n') : pending.size();
    const auto line = pending.left(end).trimmed();
    pending.remove(0, end + 1);
    if (!line.startsWith("PROGRESS ")) continue;
    const auto space = line.indexOf(' ', 9);
    bool ok = false;
    const int percent = line.mid(9, space < 0 ? -1 : space - 9).toInt(&ok);
    if (ok && percent >= 0 && percent <= 100 && progress)
      progress(percent, space < 0 ? QString() : QString::fromUtf8(line.mid(space + 1)));
  }
  // A noisy child cannot grow the line accumulator without bound.
  if (pending.size() > 64 * 1024) pending.clear();
}
} // namespace

SeismicFusionResult cli(const SeismicFusionRequest &request, const Progress &progress, Control &control)
{
  auto script = request.scriptPath.isEmpty() ? SeismicAttributeFusionClient::defaultServiceScript() : request.scriptPath;
  if (script.isEmpty()) return failure(SeismicAttributeFusionClient::tr("Local fusion service is unavailable. Configure PALEO_FUSION_SCRIPT to enable optional CLI execution."));
  if (!QFileInfo(script).isFile() || !QFileInfo(script).isReadable())
    return failure(SeismicAttributeFusionClient::tr("Service script is not readable: %1").arg(script));
  script = QFileInfo(script).absoluteFilePath();
  auto program = request.pythonExe;
  if (program.isEmpty() && qEnvironmentVariableIsEmpty("PALEO_FUSION_PYTHON")) {
    const QDir dir(QFileInfo(script).absolutePath());
    for (const auto &name : {QStringLiteral(".venv/Scripts/python.exe"), QStringLiteral(".venv/bin/python")}) {
      const auto candidate = dir.filePath(name);
      if (QFileInfo(candidate).isExecutable()) { program = candidate; break; }
    }
  }
  if (program.isEmpty()) program = SeismicAttributeFusionClient::defaultPythonExecutable();
  const auto python = executable(program);
  if (python.isEmpty()) return failure(SeismicAttributeFusionClient::tr("Python executable was not found: %1").arg(program));

  QTemporaryDir files(QDir::tempPath() + QStringLiteral("/paleo-fusion-control-XXXXXX"));
  if (!files.isValid()) return failure(SeismicAttributeFusionClient::tr("Cannot create fusion request directory."));
  QTemporaryDir generatedOutput;
  auto output = request.outputDir;
  if (output.isEmpty()) {
    if (!generatedOutput.isValid()) return failure(SeismicAttributeFusionClient::tr("Cannot create fusion output directory."));
    output = generatedOutput.path();
  }
  output = QFileInfo(output).absoluteFilePath();
  if (!QDir().mkpath(output)) return failure(SeismicAttributeFusionClient::tr("Cannot create fusion output directory: %1").arg(output));
  const auto requestPath = files.filePath(QStringLiteral("request.json"));
  const auto resultPath = files.filePath(QStringLiteral("result.json"));
  auto object = SeismicAttributeFusionClient::buildRequestJson(request);
  object.insert(QStringLiteral("output_dir"), output);
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QSaveFile file(requestPath);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    return failure(SeismicAttributeFusionClient::tr("Cannot write fusion request file."));

  const auto stopped = control.interruption();
  if (!stopped.isEmpty()) return failure(stopped);
  QProcess process;
  process.setWorkingDirectory(QFileInfo(script).absolutePath());
  process.start(python, {QStringLiteral("-X"), QStringLiteral("utf8"), script,
                        QStringLiteral("--run"), requestPath, QStringLiteral("--result"), resultPath});
  const auto interrupt = [&](const QString &error) {
    process.kill();
    process.waitForFinished(1000);
    return failure(error);
  };
  while (process.state() == QProcess::Starting) {
    if (!control.interruption().isEmpty()) return interrupt(control.interruption());
    process.waitForStarted(20);
  }
  if (process.error() == QProcess::FailedToStart)
    return failure(SeismicAttributeFusionClient::tr("Cannot start fusion service: %1").arg(process.errorString()));
  QByteArray pending, tail;
  while (process.state() != QProcess::NotRunning) {
    if (!control.interruption().isEmpty()) return interrupt(control.interruption());
    process.waitForFinished(20);
    drain(process, pending, tail, progress, false);
  }
  drain(process, pending, tail, progress, true);
  if (!control.interruption().isEmpty()) return failure(control.interruption());
  QFile resultFile(resultPath);
  if (!resultFile.open(QIODevice::ReadOnly) || resultFile.size() > 8 * 1024 * 1024)
    return failure(SeismicAttributeFusionClient::tr("Fusion process returned no readable result within the JSON size limit: %1").arg(QString::fromUtf8(tail)));
  QJsonParseError jsonError;
  const auto document = QJsonDocument::fromJson(resultFile.readAll(), &jsonError);
  if (jsonError.error != QJsonParseError::NoError || !document.isObject())
    return failure(SeismicAttributeFusionClient::tr("Fusion process returned invalid result JSON."));
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    auto error = document.object().value(QStringLiteral("error")).toString();
    if (error.isEmpty()) error = SeismicAttributeFusionClient::tr("Fusion process failed (%1): %2").arg(process.exitCode()).arg(QString::fromUtf8(tail));
    return failure(error);
  }
  auto result = parse(document.object(), output, request.maxResultCells, &control);
  if (result.ok && request.outputDir.isEmpty()) generatedOutput.setAutoRemove(false);
  return result;
}
} // namespace paleo::fusion_detail
