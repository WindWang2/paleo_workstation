// 层：功能
#pragma once
#include "pythonanalysisoperation.h"
#include "services/pythonenvironmentregistry.h"

namespace paleo::python {
// Install this adapter once per document binding. Registry activation changes
// only future captures; frozen workers retain their provider and mode token.
capability::CapabilityExecutionAdapter environmentAnalysisAdapter(AnalysisKind,
    std::shared_ptr<PythonEnvironmentRegistry>, AnalysisPlanProvider);
} // namespace paleo::python
