// 层：视图
#include "paleomainwindow.h"
#include "agentnavigationadapter.h"
#include "../linkage/selectioncontext.h"
#include "../linkage/wellmaplink.h"
#include "datapreview/datapreviewtabs.h"

// paleomainwindow_agent_navigation.cpp — I4：主窗侧导航适配器工厂。
//
// 只做窄接线：把主窗持有的预览标签宿主 + 选择总线 + 井地图链接交给
// AgentNavigationAdapter（parent=主窗，同寿命）。幂等（objectName 钉死），
// 不开放任意 QWidget 查询，也不改既有编辑/页面成员。空指针入参（壳未建
// 预览面/无选择总线的小环境）时返回 nullptr——组装根按未安装处理并让
// 工具如实报不可用。
AgentNavigationAdapter *PaleoMainWindow::attachAgentNavigation(SelectionContext *selection,
                                                               WellMapLink *wellLink)
{
  if (auto *existing = findChild<AgentNavigationAdapter *>(
          QStringLiteral("agentNavigationAdapter")))
    return existing;
  if (!m_previewTabs || !selection)
    return nullptr;
  auto *adapter = new AgentNavigationAdapter(m_previewTabs, selection, wellLink, this);
  adapter->setObjectName(QStringLiteral("agentNavigationAdapter"));
  return adapter;
}
