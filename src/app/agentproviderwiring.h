// 层：组装根
#pragma once
#include "workflow/aichattoolrunner.h"
namespace paleo::document { class DocumentSession; }

AiChatToolRunner::TrackingInputProvider trackingInputProvider(paleo::document::DocumentSession *document);
