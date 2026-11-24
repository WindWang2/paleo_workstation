// 层：组装根
#include "agentnavigationwiring.h"
#include "appcontext.h"
#include "../ui/paleomainwindow.h"
#include "../qgis/agentmapnavigation.h"
#include "../ui/agentnavigationadapter.h"
#include "../workflow/agentnavigation.h"
#include "../workflow/aichatcontroller.h"

// app/agentnavigationwiring.cpp — I4：组装根装配。
//
// 依赖全部来自既有宿主：主画布控制器/图层服务/工程服务（qgis 定位）、
// 主窗预览标签宿主 + SelectionContext + WellMapLink（视图适配）、共享
// PreviewDocService（typed 接缝内部使用，经 DataPreviewTabs 持有的门面）。
// runner 的受信 catalog/documentId 上下文沿用既有 setContext 路径——本
// 装配不注入任何模型可设参数。
paleo::agent::navigation::AgentNavigation *installAgentNavigation(AiChatToolRunner *runner,
                                                                  AppContext *ctx,
                                                                  PaleoMainWindow *window)
{
  if (!runner || !ctx || !window)
    return nullptr;
  if (auto *existing = ctx->findChild<paleo::agent::navigation::AgentNavigation *>(
          QStringLiteral("agentNavigationOrchestrator")))
    return existing;
  auto *mapNavigation = new AgentMapNavigation(ctx->canvasCtl(), ctx->layerSvc(),
                                               ctx->projectSvc(), ctx);
  mapNavigation->setObjectName(QStringLiteral("agentMapNavigation"));
  AgentNavigationAdapter *adapter = window->attachAgentNavigation(ctx->selection(),
                                                                  ctx->wellLink());
  auto *navigation = new paleo::agent::navigation::AgentNavigation(ctx);
  navigation->setObjectName(QStringLiteral("agentNavigationOrchestrator"));
  navigation->setMapAccess(mapNavigation, mapNavigation);
  if (adapter) {
    // 视图适配器同时供给选择与预览两个访问面（owner 即其自身——与主窗
    // 同寿命，QPointer 守卫）。
    navigation->setSelectionAccess(adapter, adapter);
    navigation->setPreviewAccess(adapter, adapter);
  }
  runner->setNavigation(navigation, ctx);
  return navigation;
}
