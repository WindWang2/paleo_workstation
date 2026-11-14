// 层：数据
#include "pythonprocess_windows_internal.h"
#include <QElapsedTimer>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <QDir>
#include <QThread>

namespace paleo::python::windows_process {
namespace {
bool valid(const process_detail::Launch &r){
  const auto &p=r.policy;
  if(!p.validate().isEmpty()||r.arguments.size()>256||r.inputChunks.size()>4096||
      r.program.contains(QChar(0))||r.workingDirectory.contains(QChar(0))||!QFileInfo(r.program).isAbsolute()||!QFileInfo(r.workingDirectory).isAbsolute())return false;
  qsizetype chars=0;for(const auto &arg:r.arguments){if(arg.contains(QChar(0))||arg.size()>32760)return false;chars+=arg.size();if(chars>32760)return false;}
  const auto transport=p.inputBytes+p.codeBytes+4096;
  qint64 input=0;for(const auto &part:r.inputChunks){if(part.size()>transport-input)return false;input+=part.size();}return true;
}
enum class Drain { Empty,Data,Closed,Failure };
Drain drain(const Handle &pipe,QByteArray &bytes,qint64 maximum,const QString &budget,QString &code){
  DWORD available=0;if(!PeekNamedPipe(pipe.get(),nullptr,0,nullptr,&available,nullptr)){
    if(GetLastError()==ERROR_BROKEN_PIPE)return Drain::Closed;code="python_process_pipe_failed";return Drain::Failure;}
  if(!available)return Drain::Empty;const auto remaining=maximum-bytes.size();if(remaining<=0){code=budget;return Drain::Failure;}
  char buffer[16*1024];const auto count=DWORD(qMin<qint64>(qMin<qint64>(available,sizeof(buffer)),remaining));DWORD got=0;
  if(!ReadFile(pipe.get(),buffer,count,&got,nullptr)||!got){code="python_process_pipe_failed";return Drain::Failure;}bytes.append(buffer,got);return Drain::Data;
}
struct Stop {
  Job &job;Child &child;Writer &writer;
  ~Stop(){if(child.process.valid()){job.stop();if(WaitForSingleObject(child.process.get(),5000)!=WAIT_OBJECT_0)TerminateProcess(child.process.get(),91);}writer.close();}
};
ProcessResult unavailable(const QString &detail){ProcessResult result;result.error=detail.toUtf8();return result;}
} // namespace
ProcessResult runChild(const process_detail::Launch &request,const Security *security){
  if(!valid(request))return unavailable("invalid_native_launch");
  ProcessResult result;result.code.clear();if(cancelled(request)){result.code="cancelled";return result;}
  const auto permit=request.permit?request.permit:process_detail::acquireProcessPermit([&]{return cancelled(request);});
  if(!permit){result.code="cancelled";return result;}
  QElapsedTimer clock;clock.start();Job job;Pipes pipes;Child child;Writer writer;QString detail;
  if(!job.create(request.policy,&detail)||!pipes.create(&detail))return unavailable(detail);Stop stop{job,child,writer};
  if(!child.start(request,security,job,pipes,&detail)){result.code="python_process_start_failed";result.error=detail.toUtf8();return result;}pipes.closeChildEnds();
  if(!writer.start(std::move(pipes.inputWrite),request.inputChunks,&detail))return unavailable(detail);
  bool finished=false;
  while(true){
    if(cancelled(request)){result.code="cancelled";break;}
    if(clock.elapsed()>=qint64(request.policy.wallSeconds)*1000){result.code="python_process_timeout";break;}
    result.code=job.notification();if(!result.code.isEmpty())break;
    metrics(child,job,result);if(result.cpuMicros>=qint64(request.policy.cpuSeconds)*1000000){result.code="python_process_cpu_limit";break;}
    writer.pump();const auto out=drain(pipes.outputRead,result.output,request.policy.outputBytes,"python_process_output_limit",result.code);
    const auto err=drain(pipes.errorRead,result.error,request.policy.logBytes,"python_process_log_limit",result.code);if(!result.code.isEmpty())break;
    finished=WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0;if(finished&&out!=Drain::Data&&err!=Drain::Data)break;
    if(out!=Drain::Data&&err!=Drain::Data)QThread::msleep(10);
  }
  if(!finished||!result.code.isEmpty())job.stop();WaitForSingleObject(child.process.get(),5000);writer.close();metrics(child,job,result);
  if(result.code.isEmpty())result.code=job.notification();DWORD exit=0;
  if(!GetExitCodeProcess(child.process.get(),&exit)){if(result.code.isEmpty())result.code="python_process_failed";}else result.exitCode=int(exit);
  if(result.code.isEmpty()&&exit!=0)result.code="python_process_failed";
  if(result.code.isEmpty()&&writer.error)result.code="python_process_input_failed";
  if(result.code.isEmpty())result.code="ok";if(!result.ok())result.output.clear();return result;
}
} // namespace paleo::python::windows_process
#else
namespace paleo::python::windows_process {
ProcessResult runChild(const process_detail::Launch &,const Security *){return {};}
}
#endif
