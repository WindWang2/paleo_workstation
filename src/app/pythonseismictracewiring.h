// 层：组装根
#pragma once
#include "workflow/pythonseismictraceoperation.h"
class DataCatalog;
namespace paleo::document {class DocumentSession;}
paleo::python::SeismicTracePlanProvider assemblePythonSeismicTraceProvider(DataCatalog *,
  paleo::document::DocumentSession *,paleo::capability::CapabilityHost *);
