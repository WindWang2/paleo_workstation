// 层：功能
#include "aichattoolrunner.h"
#include "services/pythonruntime.h"

void AiChatToolRunner::runPythonEnvironment() {
  if(!m_taskService){finishActive(false,{{"code","task_service_unavailable"}});return;}
  auto provider=m_context.pythonRuntime;
  if(!provider)provider=std::make_shared<paleo::python::RuntimeProvider>(paleo::python::RuntimeConfig{});
  const auto generation=m_generation;const auto id=m_activeCall.id;
  const QPointer<AiChatToolRunner> guard(this);
  auto result=std::make_shared<QJsonObject>();
  auto *task=m_taskService->start(QObject::tr("Python 环境状态"),[provider,result](PaleoTask *task) {
    const auto inspected=provider->inspect([task]{return task->cancelRequested();});
    *result=inspected.json();return QString();
  });
  if(!guard){if(task)task->requestCancel();return;}
  m_activeTask=task;
  const QPointer<PaleoTask> safeTask(task);
  connect(task,&PaleoTask::finished,this,[guard,safeTask,result,generation,id] {
    if(!guard || generation!=guard->m_generation || !guard->m_active || guard->m_activeCall.id!=id)return;
    if(!guard->activeContextIsCurrent()){guard->finishActive(false,{{"code","stale_session"}});return;}
    if(!safeTask || safeTask->cancelRequested()){guard->finishActive(false,{{"code","cancelled"}});return;}
    guard->finishActive(true,*result);
  },Qt::SingleShotConnection);
}
