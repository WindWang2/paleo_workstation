// 层：组装根
#include "agentmapdisplaywiring.h"

#include "qgis/agentmapdisplayadapter.h"
#include "workflow/aichattoolrunner.h"

AgentMapDisplayAdapter *installAgentMapDisplay(AiChatToolRunner *runner,
                                                QgisProjectService *projectSvc,
                                                QgisLayerService *layerSvc,
                                                QgisCanvasController *canvasCtl,
                                                QObject *owner) {
  if (!runner || !layerSvc)
    return nullptr; // 组装残缺：不装假供给（readiness 将如实不广播）
  if (owner) {
    if (auto *existing = owner->findChild<AgentMapDisplayAdapter *>(
            QStringLiteral("agentMapDisplayAdapter"))) {
      runner->setMapDisplay(existing, existing);
      return existing;
    }
  }
  auto *adapter = new AgentMapDisplayAdapter(projectSvc, layerSvc, canvasCtl, owner);
  adapter->setObjectName(QStringLiteral("agentMapDisplayAdapter"));
  runner->setMapDisplay(adapter, adapter);
  return adapter;
}
