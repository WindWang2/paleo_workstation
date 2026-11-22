// 层：组装根
#include "aiplanwiring.h"
#include "../ui/ai/aiassistdock.h"
#include "../ui/paleomainwindow.h"
#include "../workflow/aichatcontroller.h"
#include "../workflow/aiplanpresentation.h"
#include "../workflow/capabilityhost.h"
#include "../workflow/documentsession.h"

// app/ — H5 计划审阅装配实现。adapter 生命周期挂主窗（窗口在则 adapter
// 在；宿主 controller/host/document 实例由 AppContext 持有，跨工程重绑不
// 换实例）。不构造第二套 AppContext。
paleo::chat::AiPlanPresentation *installPlanWorkbench(
    PaleoMainWindow *window, AiChatController *chat,
    paleo::capability::CapabilityHost *host,
    paleo::document::DocumentSession *document) {
  if (!window || !chat || !host)
    return nullptr;
  auto *presenter = window->findChild<paleo::chat::AiPlanPresentation *>(
    QStringLiteral("aiPlanPresenter"));
  if (!presenter) {
    paleo::chat::AiPlanPresentation::Deps deps;
    deps.chat = chat;
    deps.host = host;
    deps.document = document;
    presenter = new paleo::chat::AiPlanPresentation(deps, window);
    presenter->setObjectName(QStringLiteral("aiPlanPresenter"));
  }
  auto *dock = window->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  if (dock)
    dock->attachPlanPresenter(presenter); // 幂等（同指针直接返回）
  return presenter;
}
