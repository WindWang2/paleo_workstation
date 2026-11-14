// 层：功能
#pragma once
#include "capabilityrecovery.h"
#include "services/pythonprocess.h"

namespace paleo::python {
bool storePythonOperationBytes(const QString &path,QByteArrayView bytes);
void appendPythonProcessDiagnostics(capability::OperationWorkResult &out,
    const capability::OperationWorkContext &context,const ProcessResult &process,const ExecutionPolicy &policy);
} // namespace paleo::python
