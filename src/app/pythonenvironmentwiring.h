// 层：组装根
#pragma once
#include "pythonanalysiswiring.h"

bool assemblePythonEnvironmentRegistry(AiChatToolRunner *, AiChatToolContext &, DataCatalog *,
    paleo::document::DocumentSession *, paleo::capability::CapabilityHost *, const std::function<bool()> &);
