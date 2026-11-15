// 层：功能
#include "pythonenvironmentcontroller.h"
#include "services/paleotaskservice.h"
#include "services/pythondelivery.h"
#include "services/pythonenvironmentregistry.h"
#include <QSaveFile>
#include <QJsonDocument>
#include <atomic>

using namespace paleo::python;
namespace {
struct Result {
  std::atomic_bool ready{false};
  QString code;
  QJsonObject data;
};
QString mutationCode(const EnvironmentConfigurationResult &result) { return result.code; }
QString failedMessage(const QString &code) {
  if (code == "cancelled") return QObject::tr("操作已取消。");
  if (code.contains("revision") || code.contains("conflict") || code.contains("stale"))
    return QObject::tr("配置已变更。请重新载入后再应用，当前输入仍保留。");
  if (code.contains("anchor") || code.contains("manifest") || code.contains("changed"))
    return QObject::tr("交付校验未通过。请核对独立交付记录，并使用与本应用匹配的环境包。");
  if (code.contains("space")) return QObject::tr("安装空间不足。旧版本已保留，请选择空间充足的专用目录。");
  if (code.contains("store")) return QObject::tr("安装目录不可用。请使用运行时或模板各自的专用目录。");
  if (code.contains("revoked") || code.contains("denied")) return QObject::tr("此环境未获执行授权。请选择已核验且允许使用的版本。");
  if (code.contains("commit_uncertain")) return QObject::tr("安装选择的持久状态尚未确认。请核对已安装版本，再用同一交付包重试。");
  return QObject::tr("环境操作未完成。请导出诊断检查原因。");
}
}
PythonEnvironmentController::PythonEnvironmentController(std::shared_ptr<PythonEnvironmentRegistry> registry,
    PaleoTaskService *service, QObject *parent) : QObject(parent), m_registry(std::move(registry)), m_service(service) {
  if (service) connect(service, &QObject::destroyed, this, [this] {
    ++m_generation; m_task.clear(); m_busy = false;
    const QPointer<PythonEnvironmentController> guard(this);
    emit busyChanged(false);
    if (guard) emit messageChanged(tr("任务服务已关闭，环境操作已中断。"));
  });
}
PythonEnvironmentController::~PythonEnvironmentController() { if (m_task) m_task->requestInterruption(); }
void PythonEnvironmentController::start(Kind kind, const QString &title, Work work, bool replaceForm) {
  if (!m_registry || !m_service) { emit messageChanged(tr("宿主环境服务尚未就绪。")); return; }
  if (m_busy) { emit messageChanged(tr("已有环境操作正在进行。")); return; }
  const QPointer<PythonEnvironmentController> guard(this);
  const QPointer<PaleoTaskService> service(m_service);
  const auto generation = ++m_generation;
  const auto state = std::make_shared<Result>();
  m_busy = true; emit busyChanged(true);
  if (!guard || !service || generation != m_generation) return;
  auto *raw = service->start(title, [state, work = std::move(work)](PaleoTask *task) {
    state->code = work(task, state->data);
    state->ready.store(true, std::memory_order_release);
    return state->code == "ok" ? QString() : state->code;
  }, QString(), PaleoTask::Priority::High, true);
  if (!guard || generation != m_generation) { if (service && raw) raw->requestInterruption(); return; }
  if (!service || !raw) { m_busy = false; emit busyChanged(false); return; }
  m_task = raw;
  connect(raw, &PaleoTask::finished, this, [this, generation, state, kind, replaceForm, service] {
    if (generation != m_generation) return;
    const QPointer<PythonEnvironmentController> owner(this);
    const auto task = m_task;
    const bool current = task && service && task->session() == service->session();
    const bool cancelled = !task || task->cancelRequested();
    m_task.clear(); m_busy = false; m_disabling = false;
    emit busyChanged(false);
    if (!owner || generation != m_generation) return;
    if (!current || cancelled) { emit messageChanged(tr("环境操作已中断，未应用过期结果。")); return; }
    if (!state->ready.load(std::memory_order_acquire)) { emit messageChanged(tr("环境操作未产生结果。")); return; }
    complete(kind, state->code, state->data, replaceForm);
  });
}
void PythonEnvironmentController::reload() {
  const auto registry = m_registry; const auto legacy = captureLegacyPythonEnvironment();
  start(Kind::Reload, tr("载入 Python 环境"), [registry, legacy](PaleoTask *task, QJsonObject &) {
    return registry->refresh(legacy, registry->beginRefresh(), [task] { return task->cancelRequested(); });
  }, true);
}
void PythonEnvironmentController::install(const QString &package, const QString &anchor, const QString &store) {
  start(Kind::Install, tr("安装 Python 离线环境"), [request = DeliveryRequest{package, anchor, store}](PaleoTask *task, QJsonObject &data) {
    task->reportStage("hash");
    const auto result = installDelivery(request, [task] { return task->cancelRequested(); });
    data = {{"kind", result.kind}, {"root", result.root}, {"manifestSha256", result.manifestSha256},
            {"licenseStatus", result.licenseStatus}, {"alreadyInstalled", result.alreadyInstalled}};
    return result.code;
  });
}
void PythonEnvironmentController::activate(const QJsonObject &form) {
  if (!m_formLoaded) { emit messageChanged(tr("请先载入宿主配置。")); return; }
  EnvironmentCandidate candidate;
  if (!m_formPolicy.isEmpty() && !environmentPolicy(m_formPolicy, candidate.policy)) {
    emit messageChanged(tr("已载入的执行策略无效，请重新载入配置。")); return;
  }
  candidate.runtimeRoot = form.value("runtimeRoot").toString(); candidate.runtimeSha256 = form.value("runtimeManifestSha256").toString();
  candidate.scriptsRoot = form.value("scriptsRoot").toString(); candidate.scriptsSha256 = form.value("scriptsManifestSha256").toString();
  candidate.policy.allowTrusted = form.value("allowTrusted").toBool(); candidate.policy.allowGenerated = form.value("allowGenerated").toBool();
  const auto registry = m_registry; const auto revision = m_formRevision;
  start(Kind::Activate, tr("验证并应用 Python 环境"), [registry, candidate, revision](PaleoTask *task, QJsonObject &) {
    return mutationCode(registry->activate(candidate, revision, [task] { return task->cancelRequested(); }));
  }, true);
}
void PythonEnvironmentController::probe() {
  const auto registry = m_registry; const auto legacy = captureLegacyPythonEnvironment();
  start(Kind::Probe, tr("检查 Python 可执行状态"), [registry, legacy](PaleoTask *task, QJsonObject &data) {
    const auto stopped = [task] { return task->cancelRequested(); };
    const auto code = registry->refresh(legacy, registry->beginRefresh(), stopped);
    if (code != "ok") return code;
    const auto status = registry->status(); const auto id = status.value("activeIdentityId").toString();
    data = {{"identityId", id}, {"revision", status.value("revision")}};
    if (status.value("allowTrusted").toBool()) data.insert("trusted", registry->probe(id, EnvironmentMode::Trusted, stopped));
    if (stopped()) return QString("cancelled");
    if (status.value("allowGenerated").toBool()) data.insert("generated", registry->probe(id, EnvironmentMode::Generated, stopped));
    return stopped() ? QString("cancelled") : QString("ok");
  });
}
void PythonEnvironmentController::disable() {
  if (!m_registry || m_disabling) return;
  m_registry->denyInMemory();
  const QPointer<PythonEnvironmentController> guard(this);
  if (m_task) m_task->requestCancel();
  if (!guard) return;
  ++m_generation; m_task.clear(); m_busy = false;
  const auto registry = m_registry;
  const auto revision = registry->configurationSnapshot().value("revision").toString().toULongLong();
  m_disabling = true;
  start(Kind::Disable, tr("保存 Python 停用配置"), [registry, revision](PaleoTask *, QJsonObject &) {
    return mutationCode(registry->disable(revision));
  }, true);
}
void PythonEnvironmentController::revoke(const QString &identityId) {
  if (!m_registry || m_busy) return;
  m_registry->denyInMemory(identityId);
  const auto registry = m_registry; const auto revision = m_formRevision;
  start(Kind::Revoke, tr("撤销 Python 环境授权"), [registry, revision, identityId](PaleoTask *, QJsonObject &) {
    return mutationCode(registry->revoke(identityId, revision));
  }, true);
}
void PythonEnvironmentController::rollback(const QString &identityId) {
  if (!m_formLoaded) { emit messageChanged(tr("请先载入宿主配置。")); return; }
  const auto registry = m_registry; const auto revision = m_formRevision;
  start(Kind::Rollback, tr("回退 Python 环境"), [registry, revision, identityId](PaleoTask *task, QJsonObject &) {
    return mutationCode(registry->rollback(identityId, revision, [task] { return task->cancelRequested(); }));
  }, true);
}
void PythonEnvironmentController::exportDiagnostics(const QString &path) {
  const auto bytes = QJsonDocument(redactedDiagnostics()).toJson(QJsonDocument::Indented);
  start(Kind::Export, tr("导出 Python 环境诊断"), [path, bytes](PaleoTask *task, QJsonObject &) {
    if (task->cancelRequested()) return QString("cancelled");
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) return QString("python_diagnostics_write_failed");
    if (task->cancelRequested()) { output.cancelWriting(); return QString("cancelled"); }
    return output.commit() ? QString("ok") : QString("python_diagnostics_write_failed");
  });
}
void PythonEnvironmentController::cancel() { if (m_task && !m_disabling) m_task->requestCancel(); }
void PythonEnvironmentController::complete(Kind kind, const QString &code, const QJsonObject &data, bool replaceForm) {
  const QPointer<PythonEnvironmentController> guard(this);
  if (code != "ok") {
    publishSnapshot(false);
    if (guard) emit messageChanged((kind == Kind::Disable ? tr("本次会话已停用，但停用配置尚未保存。") : QString()) + failedMessage(code));
    return;
  }
  if (kind == Kind::Probe) {
    m_probeIdentity = data.value("identityId").toString(); m_probeRevision = data.value("revision").toString();
    m_trustedProbe = data.value("trusted").toObject(); m_generatedProbe = data.value("generated").toObject();
  }
  if (kind == Kind::Install) {
    emit installationCompleted(data);
    if (guard) emit messageChanged(tr("离线环境已安装，尚未启用。请验证并应用所需权限。"));
    return;
  }
  publishSnapshot(replaceForm);
  if (!guard) return;
  if (kind == Kind::Disable) emit messageChanged(tr("Python 计算已停用并保存。"));
  else if (kind == Kind::Export) emit messageChanged(tr("脱敏诊断已导出。"));
  else if (kind == Kind::Activate || kind == Kind::Rollback) emit messageChanged(tr("配置已应用；可执行状态须单独检查。"));
}
