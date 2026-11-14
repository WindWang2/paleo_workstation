// 层：功能
#include "aichattoolrunner.h"
#include "services/pythonruntime.h"
#include "services/pythontrustedreadiness.h"
#include <QJsonArray>
#include <QThread>

bool AiChatToolRunner::pythonExecutionAllowed() const {
  if(QThread::currentThread()!=thread() || !m_context.pythonRuntime)return false;
  const auto policy=m_context.pythonRuntime->policy();
  return m_context.pythonRuntime->configuration().enabled && (policy.allowTrusted || policy.allowGenerated);
}

void AiChatToolRunner::runPythonEnvironment() {
  if(!m_taskService){finishActive(false,{{"code","task_service_unavailable"}});return;}
  auto provider=m_context.pythonRuntime;
  const auto execution=m_context.pythonTrusted;
  if(!provider)provider=std::make_shared<paleo::python::RuntimeProvider>(paleo::python::RuntimeConfig{});
  const auto generation=m_generation;const auto id=m_activeCall.id;
  const QPointer<AiChatToolRunner> guard(this);
  const auto readiness=m_context.runtimeReadiness;
  auto result=std::make_shared<QJsonObject>();
  auto *task=m_taskService->start(QObject::tr("Python 环境状态"),[provider,execution,result](PaleoTask *task) {
    const auto inspected=provider->inspect([task]{return task->cancelRequested();});
    *result=inspected.json();result->insert("policy",provider->policy().json());
    if(execution && provider->policy().allowTrusted)
      result->insert("trustedExecution",execution->inspect([task]{return task->cancelRequested();}));
    return QString();
  });
  if(!guard){if(task)task->requestCancel();return;}
  if(!task){finishActive(false,{{"code","task_service_unavailable"}});return;}
  m_activeTask=task;
  const QPointer<PaleoTask> safeTask(task);
  connect(task,&PaleoTask::finished,this,[guard,safeTask,result,generation,id,readiness] {
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
    guard->finishActive(true,*result);
  },Qt::SingleShotConnection);
}
