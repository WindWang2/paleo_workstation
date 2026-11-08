# P2 data-query —— 统一筛选、列漏斗与批操作选区（docs/progress/p2-data-query-20261108.md）。
# 新增源文件/测试的挂载点（见 CMakeLists.txt「并行开发挂点」段；不直接改
# 根 CMakeLists 的模块源列表）。

target_sources(paleo_ui PRIVATE src/ui/pages/dataops/dataopsquery.cpp)

# 纯值面 oracle 回归：查询语义（条件/列约束/状态串 v2/诊断/编译计数）——
# 不实例化 DataListPanel，直接锁 CompiledQuery/DataQuery 行为。
add_paleo_test(tst_dataquery LIBS paleo_ui)

# 端到端：真实临时工程 + DataListPanel 五视图/分页/选区/漏斗提交面。
# RUN_SERIAL：含 300+ 资产刷新与 stat 走查断言（tst_assetpaging 同先例）。
add_paleo_test(tst_dataquery_ui LIBS paleo_ui)
target_link_libraries(tst_dataquery_ui PRIVATE ${CMAKE_DL_LIBS})
set_tests_properties(tst_dataquery_ui PROPERTIES RUN_SERIAL TRUE)
