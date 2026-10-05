# 方向 47：多 realization / 不确定性——集合契约（catalog）+ 逐像元统计核
#（algorithms）+ 编排（workflow）+ 面板/图签（ui）。源文件与测试集中在此，
# 根 CMakeLists 只登记本文件，不改模块源列表。

target_sources(paleo_store PRIVATE src/catalog/realizationset.cpp)
target_sources(paleo_algorithms PRIVATE src/algorithms/ensemblestats.cpp)

# 契约：集合/成员寻址 round-trip + 缺号诚实面 + 缺键兼容（无 epoch bump）。
add_paleo_test(tst_realizationset LIBS paleo_store)
# 统计核：合成成员场 → 均值/总体标准差/分位数逐像元收敛；带式与整幅 parity。
add_paleo_test(tst_ensemblestats LIBS paleo_algorithms)

target_sources(paleo_workflow PRIVATE src/workflow/realizationworkflow.cpp)
# Batch 4（UI 面板）到位后启用：
# target_sources(paleo_ui PRIVATE src/ui/realization/realizationpanel.cpp)
# 编排：成员栅格→集合登记→统计派生→两集合差值 + provenance 断言。
add_paleo_test(tst_realizationworkflow LIBS paleo_workflow)
# Batch 4 启用：add_paleo_test(tst_realizationpanel LIBS paleo_ui)
