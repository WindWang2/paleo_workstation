// 层：功能
#include "aichattoolrunner.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "tileoperationservice.h"
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace {
bool decimal(const QJsonObject &args,const QString &key,quint64 &out) {
  const auto value=args.value(key);if(value.isUndefined()){out=0;return true;}
  if(!value.isString())return false;
  static const QRegularExpression exact(QStringLiteral("^(0|[1-9][0-9]{0,19})$"));
  const auto text=value.toString();bool ok=false;
  if(!exact.match(text).hasMatch())return false;out=text.toULongLong(&ok);return ok;
}
bool pageInteger(const QJsonObject &args,const QString &key,int fallback,int min,int max,int &out) {
  if(!args.contains(key)){out=fallback;return true;}
  const auto value=args.value(key);const double number=value.toDouble();
  if(!value.isDouble() || !std::isfinite(number) || number!=std::floor(number) || number<min || number>max)return false;
  out=int(number);return true;
}
}
void AiChatToolRunner::runPersistentOperation(const QJsonObject &args) {
  const auto fail=[&](const QString &code){finishActive(false,QJsonObject{{"ok",false},{"code",code}});};
  const auto host=m_context.capabilityHost;
  if(!host || !host->recovery() || m_resultSession.isEmpty())return fail("operation_host_or_session_unavailable");
  const auto scope=host->callerScope(paleo::host::Caller::Agent,m_resultSession);
  const auto id=args.value("operation_id").toString();if(id.isEmpty() || id.toUtf8().size()>256)return fail("invalid_operation_id");
  const auto name=m_activeCall.name;auto *service=host->recovery();QJsonObject response;
  if(name==QLatin1String("paleo.operation_get"))response=service->get(scope,id);
  else if(name==QLatin1String("paleo.operation_results"))response=service->results(scope,id);
  else if(name==QLatin1String("paleo.operation_cancel"))response=service->cancel(scope,id);
  else if(name==QLatin1String("paleo.operation_events")) {
    quint64 after=0;int size=0,bytes=0;
    if(!decimal(args,"after_revision",after) || !pageInteger(args,"page_size",32,1,64,size) ||
       !pageInteger(args,"max_bytes",8192,1024,32768,bytes))return fail("invalid_event_page");
    response=service->events(scope,id,after,size,bytes);
  } else {
    const QPointer<AiChatToolRunner> guard(this);
    const int generation=m_generation;const auto call=m_activeCall.id;QString code;PaleoTask *task=nullptr;
    if(name==QLatin1String("paleo.operation_resume"))
      task=service->start(scope,id,this,QStringLiteral("Resume operation"),[this,generation,call](auto result) {
        if(generation==m_generation && m_active && m_activeCall.id==call)
          finishActive(result.ok,paleo::capability::chatToolResponse(result));
      },true,&code);
    else if(name==QLatin1String("paleo.operation_read")) {
      quint64 offset=0;int bytes=0;const auto port=args.value("port_id").toString();
      if(port.isEmpty() || port.toUtf8().size()>256 || !decimal(args,"offset",offset) ||
         offset>quint64(std::numeric_limits<qint64>::max()) || !pageInteger(args,"max_bytes",8192,1,24576,bytes))
        return fail("invalid_result_page");
      task=service->read(scope,id,port,qint64(offset),bytes,this,[this,generation,call](auto result) {
        if(generation==m_generation && m_active && m_activeCall.id==call)finishActive(result.value("ok").toBool(),result);
      },&code);
    } else return fail("tool_not_found");
    if(!guard){if(task)task->requestInterruption();return;}
    if(generation!=m_generation || !activeContextIsCurrent()) {
      if(task){if(m_explicitCancelGeneration>generation)task->requestCancel();else task->requestInterruption();}return;
    }
    m_activeTask=task;if(!task)fail(code);return;
  }
  finishActive(response.value("ok").toBool(),response);
}
void AiChatToolRunner::runTileV2(const QJsonObject &args) {
  if(!m_context.capabilityHost || !m_context.tileOperations || m_resultSession.isEmpty()) {
    finishActive(false,{{"code","tile_provider_or_session_unavailable"}});return;
  }
  const auto scope=m_context.capabilityHost->callerScope(paleo::host::Caller::Agent,m_resultSession);
  const int generation=m_generation;const auto id=m_activeCall.id;QString code;
  const QPointer<AiChatToolRunner> guard(this);
  auto *task=m_context.tileOperations->start(args,scope,this,[this,generation,id](auto result) {
    if(generation==m_generation && m_active && m_activeCall.id==id)
      finishActive(result.ok,paleo::capability::chatToolResponse(result));
  },&code);
  if(!guard){if(task)task->requestInterruption();return;}
  if(generation!=m_generation || !activeContextIsCurrent()) {
    if(task){if(m_explicitCancelGeneration>generation)task->requestCancel();else task->requestInterruption();}return;
  }
  m_activeTask=task;if(!task)finishActive(false,{{"code",code}});
}
