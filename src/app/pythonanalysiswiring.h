// 层：组装根
#pragma once
#include "workflow/pythonanalysisoperation.h"
class DataCatalog;
namespace paleo::document {class DocumentSession;}
namespace paleo::python {class TrustedReadinessProvider;}

paleo::python::AnalysisPlanProvider assemblePythonAnalysisProvider(DataCatalog *,paleo::document::DocumentSession *,paleo::capability::CapabilityHost *);
QJsonObject pythonAnalysisReadiness(std::shared_ptr<paleo::python::RuntimeProvider>,const QString &scriptsRoot,
    std::shared_ptr<paleo::python::TrustedReadinessProvider> execution={});
paleo::capability::CapabilityExecutionAdapter gateTrustedAnalysis(paleo::capability::CapabilityExecutionAdapter,
    std::shared_ptr<paleo::python::TrustedReadinessProvider>);
