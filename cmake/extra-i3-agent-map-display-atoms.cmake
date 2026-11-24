# I3：受管图层与地图显示原子（paleo.map_layer* 五工具）。
# 新增源码经 target_sources 挂入分层库（不动共享清单）；测试用最小链接集。
target_sources(paleo_ai PRIVATE
  src/ai/chat/mapdisplaytools.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/agentmapdisplay.cpp
  src/workflow/aichattoolrunner_display.cpp)
target_sources(paleo_qgis PRIVATE
  src/qgis/agentmapdisplayadapter.cpp)
target_sources(paleo PRIVATE
  src/app/agentmapdisplaywiring.cpp)
# GUI 证据走真实 LayerTreePanel/属性业务页（手动 PALEO_I3_GUI_EVIDENCE_DIR
# 触发；offscreen 车道只跑断言），故链接 paleo_ui。
add_paleo_test(tst_i3_map_display_atoms LIBS paleo_workflow paleo_qgis paleo_ai paleo_ui)
