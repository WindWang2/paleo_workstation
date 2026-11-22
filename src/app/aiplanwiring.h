// 层：组装根
#pragma once

class PaleoMainWindow;
class AiChatController;
namespace paleo::capability { class CapabilityHost; }
namespace paleo::document { class DocumentSession; }
namespace paleo::chat { class AiPlanPresentation; }

// app/ — H5 计划审阅装配：为「AI 助手」页签注入计划展示/执行 adapter。
//
// 依赖全部来自既有组装根（与 installConversationPresentation 同源）；本
// 函数不创建服务、不扩张 AppContext 生命周期。幂等：adapter 复用同一实例
// （objectName 查找），重复调用只重挂，不重复订阅；Dock 晚创建/销毁重建
// 由 findChild + attach 的幂等分支覆盖。须在 PaleoMainWindow::
// attachAiAssistant 之后调用（dock 已建）。
paleo::chat::AiPlanPresentation *installPlanWorkbench(
    PaleoMainWindow *window, AiChatController *chat,
    paleo::capability::CapabilityHost *host,
    paleo::document::DocumentSession *document);
