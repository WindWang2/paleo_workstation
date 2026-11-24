# I4：基于业务身份的井、地震与地图导航原子（六个 paleo.navigation*/
# select_wells/clear_well_selection/map_locate/open_well_view/open_seismic_line）。
# 新增源码经 target_sources 挂入分层库（不动共享清单）；测试用最小链接集。
target_sources(paleo_ai PRIVATE
  src/ai/chat/navigationtools.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/agentnavigation.cpp
  src/workflow/aichattoolrunner_navigation.cpp)
target_sources(paleo_qgis PRIVATE
  src/qgis/agentmapnavigation.cpp)
target_sources(paleo_ui PRIVATE
  src/ui/agentnavigationadapter.cpp
  src/ui/datapreview/datapreviewtabs_navigation.cpp
  src/ui/paleomainwindow_agent_navigation.cpp)
target_sources(paleo_app PRIVATE
  src/app/agentnavigationwiring.cpp)
# 导航原子覆盖真实选择总线/主画布/预览页与 runner 分派：链 paleo_ui +
# paleo_qgis + paleo_workflow（offscreen 断言；真实可见窗口验证走手动
# PALEO_I4_GUI_EVIDENCE_DIR 流程，同 I3 先例）。
add_paleo_test(tst_i4_navigation_atoms LIBS paleo_workflow paleo_qgis paleo_ai paleo_ui)
target_compile_definitions(tst_i4_navigation_atoms PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
