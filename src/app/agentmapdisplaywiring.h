// 层：组装根
#pragma once
#include <QString>

class AiChatToolRunner;
class QgisProjectService;
class QgisLayerService;
class QgisCanvasController;
class QObject;
class AgentMapDisplayAdapter;

// app/agentmapdisplaywiring.h — I3：把地图显示适配器装进聊天工具 runner。
// adapter parent 归宿主传入的 owner（生产 = AppContext：与原生主画布/
// 工程/图层服务同寿命）；幂等——重复安装复用既有适配器（objectName 钉
// 死），不重建第二条供给。
AgentMapDisplayAdapter *installAgentMapDisplay(AiChatToolRunner *runner,
                                                QgisProjectService *projectSvc,
                                                QgisLayerService *layerSvc,
                                                QgisCanvasController *canvasCtl,
                                                QObject *owner);
