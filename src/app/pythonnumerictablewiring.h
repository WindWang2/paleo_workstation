// 层：组装根
#pragma once
#include "workflow/pythonnumerictableoperation.h"
#include "workflow/aichattoolrunner.h"
class DataCatalog;
namespace paleo::document {class DocumentSession;}
paleo::python::NumericTablePlanProvider assemblePythonNumericTableProvider(DataCatalog *,
  paleo::document::DocumentSession *,paleo::capability::CapabilityHost *);
void assembleNumericTableAgent(AiChatToolContext &,DataCatalog *,paleo::document::DocumentSession *,paleo::capability::CapabilityHost *);
