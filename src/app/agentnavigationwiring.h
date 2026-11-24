// 层：组装根
#pragma once
#include <QString>

class AiChatToolRunner;
class AppContext;
class PaleoMainWindow;
class AgentMapNavigation;
class AgentNavigationAdapter;
class QObject;
namespace paleo::agent::navigation { class AgentNavigation; }

// app/agentnavigationwiring.h — I4：把语义导航装配进聊天工具 runner。
// qgis 定位实现（AgentMapNavigation）与 ui 适配器（AgentNavigationAdapter，
// 经主窗 attachAgentNavigation 工厂）都 parent 归 AppContext/主窗（与画布/
// 工程服务/预览标签同寿命）；编排器 AgentNavigation 也归 AppContext。幂等
// ——重复安装复用既有实例（objectName 钉死），不重建第二条供给。
paleo::agent::navigation::AgentNavigation *installAgentNavigation(AiChatToolRunner *runner,
                                                                  AppContext *ctx,
                                                                  PaleoMainWindow *window);
