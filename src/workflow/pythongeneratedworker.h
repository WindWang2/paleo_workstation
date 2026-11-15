// 层：功能
#pragma once
#include "capabilityhost.h"
#include "services/pythonanalysisreport.h"
#include "services/pythonsandboxprovider.h"
namespace paleo::python {
struct GeneratedWorkerChecks {
  std::function<QString(const QJsonObject &,const std::shared_ptr<RuntimeProvider> &,
    const std::shared_ptr<SandboxProvider> &,bool)> plan;
  std::function<QString(const InputBundle &,const QJsonObject &)> bundle;
  std::function<AnalysisReportValidation(const QByteArray &,const QJsonObject &,const QString &,
    const QString &,const ExecutionPolicy &)> report;
  QString invalidPlanCode;
};
// Shared strong execution/diagnostics/REPORT storage; contract-specific input checks stay explicit.
capability::OperationWorker generatedReportWorker(const QJsonObject &,std::shared_ptr<RuntimeProvider>,
  std::shared_ptr<SandboxProvider>,GeneratedWorkerChecks);
}
