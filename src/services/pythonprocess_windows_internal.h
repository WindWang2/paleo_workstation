// 层：数据
#pragma once
#include "pythonprocess_windows.h"
#ifdef Q_OS_WIN
#include <vector>

namespace paleo::python::windows_process {
class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE value) : m_value(value) {}
  ~Handle(){reset();}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : m_value(other.take()) {}
  Handle &operator=(Handle &&other) noexcept {if(this!=&other)reset(other.take());return *this;}
  bool valid() const {return m_value && m_value!=INVALID_HANDLE_VALUE;}
  HANDLE get() const {return m_value;}
  HANDLE take(){const auto value=m_value;m_value=nullptr;return value;}
  void reset(HANDLE value=nullptr){if(valid())CloseHandle(m_value);m_value=value;}
private:
  HANDLE m_value = nullptr;
};
std::wstring native(const QString &path);
bool cancelled(const process_detail::Launch &request);
struct Pipes {
  Handle inputRead,inputWrite,outputRead,outputWrite,errorRead,errorWrite;
  bool create(QString *error);
  void closeChildEnds(){inputRead.reset();outputWrite.reset();errorWrite.reset();}
};
struct Job {
  Handle handle,completion;
  bool create(const ExecutionPolicy &policy,QString *error);
  QString notification();
  void stop(DWORD exit=90) const {if(handle.valid())TerminateJobObject(handle.get(),exit);}
};
struct Child {
  Handle process;
  DWORD id = 0;
  bool start(const process_detail::Launch &request,const Security *security,const Job &job,Pipes &pipes,QString *error);
};
// Overlapped stdin is pumped with stdout/stderr on the worker thread. It cannot
// block the owner while a child refuses stdin or floods its output channels.
struct Writer {
  Handle pipe,event;
  OVERLAPPED pending{};
  QList<QByteArray> chunks;
  qsizetype index=0,offset=0;
  DWORD error=0;
  bool active=false;
  bool start(Handle input,QList<QByteArray> data,QString *detail);
  void pump();
  void close();
  ~Writer(){close();}
};
void metrics(const Child &child,const Job &job,ProcessResult &result);
} // namespace paleo::python::windows_process
#endif
