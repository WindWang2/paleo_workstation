// 层：组装根
#pragma once
#include "workflow/capabilityhost.h"
class ProjectDirLock;
class SelectionContext;
class AiChatToolRunner;
namespace paleo::document { class DocumentSession; }
namespace paleo::command { class CommandDispatcher; }

// Shared by AppContext and the production assembly integration test.
paleo::capability::HostBinding captureCapabilityBinding(
    paleo::document::DocumentSession *document, DataCatalog *catalog,
    ProjectDirLock *writeLock, SelectionContext *selection,AiChatToolRunner *chatRunner=nullptr);
void configureCapabilityCallers(paleo::capability::CapabilityHost *host);
void bindCapabilityDispatcher(paleo::command::CommandDispatcher *dispatcher,
                             paleo::capability::CapabilityHost *host);
