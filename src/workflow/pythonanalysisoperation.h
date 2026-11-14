// 层：功能
#pragma once
#include "capabilityhost.h"
#include "services/pythonanalysis.h"

namespace paleo::python {
// Owner-only mapping of public source selectors to exact host identities. The
// returned validation runs again on the owner after background materialization.
using AnalysisPlanProvider=std::function<QString(const capability::ExecuteRequest &,
    AnalysisReadPlan &,std::function<QString()> &)>;
capability::CapabilityExecutionAdapter analysisScriptAdapter(AnalysisKind kind,
    std::shared_ptr<RuntimeProvider> runtime,const QString &templateRoot,AnalysisPlanProvider selector);
} // namespace paleo::python
