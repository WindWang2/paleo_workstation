// 层：功能
// workflow/aichattoolrunner_operationlist.cpp — H4：paleo.operation_list 的
// runner 分支。异步经 Recovery::listOperations（TaskService worker 扫描），
// 世代号/callId 守卫与 resume/read 同款：取消/换绑/迟到终态不回灌。
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "operationdiscovery.h"
#include <QPointer>

void AiChatToolRunner::runOperationList(const QString &scope,const QJsonObject &args) {
  // 同步失败与 worker 失败同一 payloadContract：工具结果信封据此判定契约，
  // 不因拒绝发生在建任务之前而退化成 legacy.unversioned。
  const auto fail=[&](const QString &code){finishActive(false,QJsonObject{{"ok",false},{"code",code},
      {"contract",QLatin1String(paleo::capability::OperationDiscoveryContract)}});};
  const auto host=m_context.capabilityHost;
  if(!host || !host->recovery() || m_resultSession.isEmpty())return fail("operation_host_or_session_unavailable");
  const int generation=m_generation;const auto call=m_activeCall.id;QString code;
  const QPointer<AiChatToolRunner> guard(this);
  auto *task=host->recovery()->listOperations(scope,args,this,
    [this,generation,call](QJsonObject result){
      if(generation==m_generation && m_active && m_activeCall.id==call)
        finishActive(result.value("ok").toBool(),result);
    },&code);
  // taskAdded 可能同步换绑/销毁本 runner——旧回调不清掉新请求的句柄。
  if(!guard){if(task)task->requestInterruption();return;}
  if(generation!=m_generation || !activeContextIsCurrent()) {
    if(task){if(m_explicitCancelGeneration>generation)task->requestCancel();else task->requestInterruption();}
    return;
  }
  m_activeTask=task;if(!task)fail(code);
}
