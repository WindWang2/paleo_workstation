// 层：组装根
#pragma once
#include "workflow/pythonnumerictableoperation.h"
class DataCatalog;
namespace paleo::document {class DocumentSession;}
paleo::python::NumericTablePlanProvider assemblePythonNumericTableProvider(DataCatalog *,
  paleo::document::DocumentSession *,paleo::capability::CapabilityHost *);
