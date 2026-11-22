// 层：视图
#include "aiconfigurationwiring.h"
#include "aiassistdock.h"
#include "llmconfigdialog.h"
#include "llmdiagnosticspanel.h"
#include "pythonenvironmentdialog.h"
#include "workflow/aichatcontroller.h"
#include "workflow/llmconnectiondiagnostics.h"
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
  // H6（模型接入诊断）：独立协调对象——专属网络对象随对话框析构终止在途
  // 探测；不触碰 controller 聊天编排/配置提交/钥匙串。候选含本次凭据仅存
  // 内存（不落盘、不进报告）。装配的视图意图与状态回流全在下面连接里。
  auto *diagnostics = new paleo::chat::LlmConnectionDiagnostics(&dialog);
  QObject::connect(&dialog, &LlmConfigDialog::diagnosticsActionRequested,
                   &dialog, [owner, &dialog, diagnostics](int action) {
    if (!owner) return;
    if (action == static_cast<int>(LlmDiagnosticsPanel::Action::Cancel)) {
      diagnostics->cancel(); // 只取消诊断自身；不中断密钥提交
      return;
    }
    diagnostics->runAction(
      static_cast<paleo::chat::LlmDiagnosticsAction>(action),
      dialog.buildDiagnosticCandidate());
  });
  QObject::connect(diagnostics,
                   &paleo::chat::LlmConnectionDiagnostics::summaryChanged,
                   &dialog, &LlmConfigDialog::setDiagnosticsSummary);
  QObject::connect(diagnostics,
                   &paleo::chat::LlmConnectionDiagnostics::busyChanged, &dialog,
                   &LlmConfigDialog::setDiagnosticsBusy);
  QObject::connect(diagnostics,
                   &paleo::chat::LlmConnectionDiagnostics::modelCandidatesChanged,
                   &dialog, &LlmConfigDialog::setModelCandidates);
  // 本地拒绝（零网络）：面板提示行呈现，不写配置、不冒充探测成功。
  QObject::connect(diagnostics,
                   &paleo::chat::LlmConnectionDiagnostics::candidateRejected,
                   &dialog, [&dialog](const QString &reason) {
    dialog.diagnosticsPanel()->showNotice(reason);
  });
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
