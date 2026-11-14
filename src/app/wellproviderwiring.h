// 层：组装根
#pragma once
#include "workflow/aichattoolrunner.h"
namespace paleo::document {class DocumentSession;}
namespace paleo::capability {class CapabilityHost;}
class DataCatalog;

AiChatToolRunner::FrozenFaciesProvider assembleWellFaciesProvider(DataCatalog *catalog,
    paleo::document::DocumentSession *session,paleo::capability::CapabilityHost *host);
