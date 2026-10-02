# goal/fault-surface — 断棒成面、断距与体域阻断。
# 源文件与测试集中在此。根 CMakeLists 的模块源列表不改；清单只加本文件。
# faultsurface 是既有 algorithms 模块下的子目录，不走 scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/faultsurface/faultsurface.cpp)

target_sources(paleo_workflow PRIVATE
  src/workflow/faultsurfaceworkflow.cpp)
target_link_libraries(paleo_workflow PUBLIC paleo_algorithms)

# 成面几何、断距、剖面交线、体域阻断、100×200 墙钟。主体是功能断言，串行防抖动。
add_paleo_test(tst_faultsurface LIBS paleo_algorithms)
set_tests_properties(tst_faultsurface PROPERTIES RUN_SERIAL TRUE)

add_paleo_test(tst_faultsurfaceworkflow LIBS paleo_workflow)

target_sources(paleo_ui PRIVATE
  src/ui/seismic3d/faultsurfacerenderer.cpp)

add_paleo_test(tst_faultsurfaceview LIBS paleo_ui)
