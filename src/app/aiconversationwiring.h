// 层：组装根
#pragma once

class PaleoMainWindow;
class AiChatController;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; }
namespace paleo::chat { class AiOperationPresentation; }

// app/ — G5 会话工作台装配：为「AI 助手」页签注入操作展示 adapter。
//
// 依赖全部来自既有组装根（ctx.aiChat()/capabilityHost()/documentSession()）；
// 本函数不创建服务、不扩张 AppContext 生命周期。幂等：adapter 复用同一
// 实例（objectName 查找），重复调用只重挂，不重复订阅。须在
// PaleoMainWindow::attachAiAssistant 之后调用（dock 已建）。
paleo::chat::AiOperationPresentation *installConversationPresentation(
    PaleoMainWindow *window, AiChatController *chat,
    paleo::capability::CapabilityHost *host,
    paleo::document::DocumentSession *document);
