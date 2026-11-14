// 层：功能
#include "pythonoperationdiagnostics.h"
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>

namespace paleo::python {
bool storePythonOperationBytes(const QString &path,QByteArrayView bytes) {
  QSaveFile file(path);return file.open(QIODevice::WriteOnly) && file.write(bytes.data(),bytes.size())==bytes.size() && file.commit();
}
void appendPythonProcessDiagnostics(capability::OperationWorkResult &out,
    const capability::OperationWorkContext &context,const ProcessResult &process,const ExecutionPolicy &policy) {
  out.diagnostics.ok=true;
  const QJsonObject status{{"contract","paleo.python-execution.v1"},{"code",process.code},
    {"exitCode",process.exitCode},{"cpuMicros",QString::number(process.cpuMicros)},
    {"observedPeakMemoryBytes",QString::number(process.peakMemoryBytes)},{"memoryPeakComplete",process.memoryPeakComplete}};
  const auto add=[&](const QString &port,QByteArrayView bytes) {
    const auto path=QDir(context.privateDir).filePath(port+".txt");
    if(storePythonOperationBytes(path,bytes))out.diagnostics.artifacts.append({port,path,{},{},{{"diagnostic",true}}, {}});
  };
  add("EXECUTION_LOG",QJsonDocument(status).toJson(QJsonDocument::Compact));
  if(!process.error.isEmpty())add("STDERR_LOG",QByteArrayView(process.error).first(qMin<qint64>(process.error.size(),policy.logBytes)));
  if(!process.outputLog.isEmpty())add("STDOUT_LOG",QByteArrayView(process.outputLog).first(qMin<qint64>(process.outputLog.size(),policy.logBytes)));
}
} // namespace paleo::python
