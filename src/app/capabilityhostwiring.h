// 层：组装根
#pragma once
#include "workflow/capabilityhost.h"
class ProjectDirLock;
class SelectionContext;
namespace paleo::document { class DocumentSession; }

// Shared by AppContext and the production assembly integration test.
paleo::capability::HostBinding captureCapabilityBinding(
    paleo::document::DocumentSession *document, DataCatalog *catalog,
    ProjectDirLock *writeLock, SelectionContext *selection);
void configureCapabilityCallers(paleo::capability::CapabilityHost *host);
