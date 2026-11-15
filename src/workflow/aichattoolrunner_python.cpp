// 层：功能
#include "aichattoolrunner.h"
#include "services/pythonruntime.h"
#include "services/pythontrustedreadiness.h"
#include "services/pythonsandboxprovider.h"
#include "services/pythonenvironmentregistry.h"
#include <QJsonArray>
#include <QThread>

namespace {
void changedEnvironment(QJsonObject &result) {
  result.insert("ready", false); result.insert("generatedCodeAvailable", false);
  result.insert("code", "python_environment_config_changed");
  result.remove("trustedExecution"); result.remove("sandbox"); result.remove("trustedAnalyses");
}
} // namespace

void AiChatToolRunner::setPythonEnvironments(std::shared_ptr<paleo::python::PythonEnvironmentRegistry> registry) {
  if (QThread::currentThread() == thread() && !m_pythonEnvironments) m_pythonEnvironments = std::move(registry);
}
std::shared_ptr<paleo::python::RuntimeProvider> AiChatToolRunner::pythonRuntime() const {
  if (m_pythonEnvironments) {
    auto current = m_pythonEnvironments->active(paleo::python::EnvironmentMode::Generated);
    if (!current.runtime) current = m_pythonEnvironments->active(paleo::python::EnvironmentMode::Trusted);
    if (current.runtime) return current.runtime;
    return std::make_shared<paleo::python::RuntimeProvider>(paleo::python::RuntimeConfig{});
  }
  return m_context.pythonRuntime;
}
std::shared_ptr<paleo::python::TrustedReadinessProvider> AiChatToolRunner::pythonTrusted() const {
  if (m_pythonEnvironments) {
    const auto current = m_pythonEnvironments->active(paleo::python::EnvironmentMode::Trusted);
    if (current.trusted) return current.trusted;
    return std::make_shared<paleo::python::TrustedReadinessProvider>(pythonRuntime());
  }
  return m_context.pythonTrusted;
}
std::shared_ptr<paleo::python::SandboxProvider> AiChatToolRunner::pythonSandbox() const {
  if (m_pythonEnvironments) {
    const auto current = m_pythonEnvironments->active(paleo::python::EnvironmentMode::Generated);
    if (current.sandbox) return current.sandbox;
    return std::make_shared<paleo::python::SandboxProvider>(pythonRuntime());
  }
  return m_context.pythonSandbox;
}

bool AiChatToolRunner::pythonExecutionAllowed() const {
  if(QThread::currentThread()==thread() && m_pythonEnvironments) {
    const auto state=m_pythonEnvironments->status();
    return state.value("allowTrusted").toBool() || state.value("allowGenerated").toBool();
  }
  if(QThread::currentThread()!=thread() || !m_context.pythonRuntime)return false;
  const auto policy=m_context.pythonRuntime->policy();
  return m_context.pythonRuntime->configuration().enabled && (policy.allowTrusted || policy.allowGenerated);
}

void AiChatToolRunner::runPythonEnvironment() {
  if(!m_taskService){finishActive(false,{{"code","task_service_unavailable"}});return;}
  auto provider=pythonRuntime();
  const auto execution=pythonTrusted();
  const auto sandbox=pythonSandbox();
  const auto environments=m_pythonEnvironments;
  if(!provider)provider=std::make_shared<paleo::python::RuntimeProvider>(paleo::python::RuntimeConfig{});
  const auto generation=m_generation;const auto id=m_activeCall.id;
  const QPointer<AiChatToolRunner> guard(this);
  const auto readiness=m_context.runtimeReadiness;
  auto result=std::make_shared<QJsonObject>();
  auto *task=m_taskService->start(QObject::tr("Python 环境状态"),[provider,execution,sandbox,environments,result](PaleoTask *task) {
    const auto cancelled=[task]{return task->cancelRequested();};
    if(environments) {
      const auto state=environments->status();
      auto selection=environments->active(paleo::python::EnvironmentMode::Generated);
      if(!selection.runtime)selection=environments->active(paleo::python::EnvironmentMode::Trusted);
      if(!selection.runtime || selection.identity.id!=state.value("activeIdentityId").toString()) {
        *result={{"ready",false},{"code",selection.code},{"configuration",state}};return QString();
      }
      const auto inspected=selection.runtime->inspectIntegrity(cancelled);
      *result=inspected.json();result->insert("policy",selection.runtime->policy().json());
      result->insert("runtimeVerification","content_identity");result->insert("configuration",state);
      if(state.value("allowTrusted").toBool())result->insert("trustedExecution",environments->probe(selection.identity.id,paleo::python::EnvironmentMode::Trusted,cancelled));
      if(state.value("allowGenerated").toBool()) {
        const auto proof=environments->probe(selection.identity.id,paleo::python::EnvironmentMode::Generated,cancelled);
        result->insert("sandbox",proof);result->insert("generatedCodeAvailable",inspected.ready() && proof.value("ready").toBool());
      }
      if(environments->status()!=state)changedEnvironment(*result);
      return QString();
    }
    const auto inspected=provider->policy().allowGenerated?provider->inspectIntegrity(cancelled):provider->inspect(cancelled);
    *result=inspected.json();result->insert("policy",provider->policy().json());
    result->insert("runtimeVerification",provider->policy().allowGenerated?"content_identity":"runtime_import_probe");
    if(execution && provider->policy().allowTrusted)
      result->insert("trustedExecution",execution->inspect([task]{return task->cancelRequested();}));
    if(sandbox && provider->policy().allowGenerated) {
      const auto status=sandbox->inspect([task]{return task->cancelRequested();});result->insert("sandbox",status);
      result->insert("generatedCodeAvailable",inspected.ready() && status.value("ready").toBool());
    }
    return QString();
  });
  if(!guard){if(task)task->requestCancel();return;}
  if(!task){finishActive(false,{{"code","task_service_unavailable"}});return;}
  m_activeTask=task;
  const QPointer<PaleoTask> safeTask(task);
  connect(task,&PaleoTask::finished,this,[guard,safeTask,result,generation,id,readiness,environments] {
    if(!guard || generation!=guard->m_generation || !guard->m_active || guard->m_activeCall.id!=id)return;
    if(!guard->activeContextIsCurrent()){guard->finishActive(false,{{"code","stale_session"}});return;}
    if(!safeTask || safeTask->cancelRequested()){guard->finishActive(false,{{"code","cancelled"}});return;}
    if(readiness) {
      QJsonArray analyses;
      for(const auto &name:{QLatin1String("paleo.well_curve_quality"),QLatin1String("paleo.attribute_correlation")}) {
        const auto code=readiness(name,{{"publish_intent","compute-only"}},false);
        if(!guard || generation!=guard->m_generation || !guard->m_active || guard->m_activeCall.id!=id)return;
        analyses.append(QJsonObject{{"tool",name},{"computeReady",code.isEmpty()},{"code",code.isEmpty()?QStringLiteral("ok"):code}});
      }
      result->insert("trustedAnalyses",analyses);
    }
    if(!guard->activeContextIsCurrent()){guard->finishActive(false,{{"code","stale_session"}});return;}
    if(environments && result->value("configuration").toObject()!=environments->status())changedEnvironment(*result);
    guard->finishActive(true,*result);
  },Qt::SingleShotConnection);
}
