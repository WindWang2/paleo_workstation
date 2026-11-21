// 层：组装根
#include "aiconversationwiring.h"
#include "../ui/ai/aiassistdock.h"
#include "../ui/paleomainwindow.h"
#include "../workflow/aichatcontroller.h"
#include "../workflow/aioperationpresentation.h"
#include "../workflow/capabilityhost.h"
#include "../workflow/documentsession.h"

// app/ — G5 装配实现。adapter 生命周期挂主窗（窗口在则 adapter 在；宿主
// controller/host/document 实例由 AppContext 持有，跨工程重绑不换实例）。
paleo::chat::AiOperationPresentation *installConversationPresentation(
    PaleoMainWindow *window, AiChatController *chat,
    paleo::capability::CapabilityHost *host,
    paleo::document::DocumentSession *document) {
  if (!window || !chat || !host)
    return nullptr;
  auto *presenter = window->findChild<paleo::chat::AiOperationPresentation *>(
    QStringLiteral("aiOperationPresenter"));
  if (!presenter) {
    paleo::chat::AiOperationPresentation::Deps deps;
    deps.chat = chat;
    deps.host = host;
    deps.document = document;
    presenter = new paleo::chat::AiOperationPresentation(deps, window);
    presenter->setObjectName(QStringLiteral("aiOperationPresenter"));
  }
  auto *dock = window->findChild<AiAssistDock *>(QStringLiteral("aiAssistantDock"));
  if (dock)
    dock->attachOperationPresenter(presenter); // 幂等（同指针直接返回）
  return presenter;
}
