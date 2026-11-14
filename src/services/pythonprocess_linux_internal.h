// 层：数据
#pragma once
#include "pythonprocess_internal.h"
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>

namespace paleo::python::process_detail::linux_native {
struct Fd {
  int value=-1;
  explicit Fd(int fd=-1):value(fd) {}
  ~Fd();
  Fd(const Fd &)=delete;
};
// Execution authority belongs to the caller. A prepared launch never grants it.
// Strong sandbox preparation must not fall back to trusted preparation.
struct Prepared {
  QString unit;
  QStringList arguments;
  QProcessEnvironment environment;
  std::shared_ptr<Fd> filter, blockRead, blockWrite;
  qint64 fileBytes=0;
};
QJsonObject verifiedHelpers();
QProcessEnvironment helperEnvironment();
QStringList scopeArguments(const QString &unit,const ExecutionPolicy &policy);
QString prepareEnvelope(const Launch &request,Prepared &prepared);
QString verifyResourceLimits(const QString &directory,const ExecutionPolicy &policy);
void configureProcess(QProcess &process,const Prepared &prepared,const ExecutionPolicy &policy);
ProcessResult runPreparedChild(const Launch &request,Prepared &prepared);
}
