// 层：视图
#include "aiconfigurationwiring.h"
#include "aiassistdock.h"
#include "llmconfigdialog.h"
#include "pythonenvironmentdialog.h"
#include "workflow/aichatcontroller.h"
#include "workflow/pythonenvironmentcontroller.h"
#include <QPointer>

namespace {
void openEnvironment(AiAssistDock *dock, QWidget *parent) {
  auto *host = dock ? dock->controller() : nullptr;
  if (!host) return;
  auto *dialog = new PythonEnvironmentDialog(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  auto *controller = host->createPythonEnvironmentController(dialog);
  QObject::connect(dialog, &PythonEnvironmentDialog::installRequested, controller, &PythonEnvironmentController::install);
  QObject::connect(dialog, &PythonEnvironmentDialog::activationRequested, controller, &PythonEnvironmentController::activate);
  QObject::connect(dialog, &PythonEnvironmentDialog::reloadRequested, controller, &PythonEnvironmentController::reload);
  QObject::connect(dialog, &PythonEnvironmentDialog::probeRequested, controller, &PythonEnvironmentController::probe);
  QObject::connect(dialog, &PythonEnvironmentDialog::disableRequested, controller, &PythonEnvironmentController::disable);
  QObject::connect(dialog, &PythonEnvironmentDialog::revokeRequested, controller, &PythonEnvironmentController::revoke);
  QObject::connect(dialog, &PythonEnvironmentDialog::rollbackRequested, controller, &PythonEnvironmentController::rollback);
  QObject::connect(dialog, &PythonEnvironmentDialog::exportRequested, controller, &PythonEnvironmentController::exportDiagnostics);
  QObject::connect(dialog, &PythonEnvironmentDialog::cancelRequested, controller, &PythonEnvironmentController::cancel);
  QObject::connect(controller, &PythonEnvironmentController::configurationChanged, dialog, &PythonEnvironmentDialog::setConfiguration);
  QObject::connect(controller, &PythonEnvironmentController::diagnosticsChanged, dialog, &PythonEnvironmentDialog::setDiagnostics);
  QObject::connect(controller, &PythonEnvironmentController::installationCompleted, dialog, &PythonEnvironmentDialog::setInstalledCandidate);
  QObject::connect(controller, &PythonEnvironmentController::busyChanged, dialog, &PythonEnvironmentDialog::setBusy);
  QObject::connect(controller, &PythonEnvironmentController::messageChanged, dialog, &PythonEnvironmentDialog::showMessage);
  dialog->open();
}
}

void openAiConfiguration(AiAssistDock *dock, QWidget *parent) {
  auto *controller = dock ? dock->controller() : nullptr;
  if (!controller) return;
  const QPointer<AiAssistDock> owner(dock);
  LlmConfigDialog dialog(controller->config(), parent);
  QObject::connect(&dialog, &LlmConfigDialog::pythonEnvironmentRequested, &dialog,
                   [owner, &dialog] { if (owner) openEnvironment(owner, &dialog); });
  QObject::connect(&dialog, &LlmConfigDialog::applyRequested, &dialog,
      [owner, &dialog](const LlmConfig &form, const QByteArray &newKey, bool clearKey) {
    auto *current = owner ? owner->controller() : nullptr;
    if (!current) { dialog.onApplyResult(false, QObject::tr("助手编排器不可用")); return; }
    current->applyConfig(form, newKey, clearKey, &dialog,
                         [&dialog](bool ok, const QString &error) { dialog.onApplyResult(ok, error); });
  });
  dialog.exec();
}
